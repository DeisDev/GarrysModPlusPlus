#include "effects.h"

#include "bsp.h"
#include "client_hooks.h"
#include "engine_interfaces.h"
#include "entities.h"
#include "gbuffer.h"
#include "lighting.h"
#include "log.h"
#include "mod.h"
#include "renderer.h"
#include "scene.h"
#include "settings.h"
#include "skinned_casters.h"
#include "studio_model.h"

#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp::effects {

namespace {

constexpr UINT kMapPollInterval = 30;
constexpr int kMaxBones = 256;
constexpr bsp::Float3 kDefaultReflectivity = {0.5f, 0.5f, 0.5f};
constexpr float kDefaultAlphaReference = 0.5f;
// Alpha and mask textures are read from a mip no larger than this; shadows and reflections need no more detail.
constexpr uint32_t kSceneTextureSize = 256;
constexpr uint32_t kOpaqueTriangle = 0xFFFFFFFF;
const D3DFORMAT kIntzFormat = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

constexpr uint32_t kJitterSamples = 8;
constexpr uint32_t kRayReconstructionPhases = 32;
// The local player's held weapon is drawn as a world model right next to the camera; in first person it would
// shadow the viewmodel and cast a floating shadow, so weapon world models this close to the eye are left out.
constexpr float kHeldWeaponDistance = 96.0f;

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

float Halton(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float fraction = 1.0f / static_cast<float>(base);
    for (; index > 0; index /= base, fraction /= static_cast<float>(base))
        result += fraction * static_cast<float>(index % base);
    return result;
}

struct ModelEntry {
    int sceneModel = -1;
    std::shared_ptr<SkinnedModel> skinned;
};

struct MaterialInfo {
    // Translucent surfaces (glass) cast no shadow; alpha-tested ones (foliage, fences) are cut by their alpha.
    bool translucent = false;
    bool alphaTest = false;
    float alphaReference = kDefaultAlphaReference;
    std::string baseTexture;
    bsp::Float3 reflectivity = kDefaultReflectivity;
    // Reflections and self-illumination, for Scene::AddMaterial.
    uint32_t sceneMaterial = Scene::kNoMaterial;
};

// What Scene::AddModel needs beyond positions, indices and albedo; alpha-tested triangles are moved last.
struct TriangleData {
    UINT opaqueTriangles = 0;
    std::vector<uint32_t> alphaTriangles;
    // Scene material of each triangle.
    std::vector<uint32_t> materials;
};

struct SceneTexture {
    UINT view = DescriptorHeap::kInvalid;
    bool alpha = false;
};

struct State {
    IDirect3DDevice9* device = nullptr;
    ComPtr<IDirect3DTexture9> depthTexture;
    ComPtr<IDirect3DSurface9> depthSurface;
    bool depthReplaced = false;
    bool gbufferReady = false;
    DWORD renderThread = 0;
    uint64_t frame = 0;
    bool warnedQueuedRendering = false;
    std::unique_ptr<Lighting> lighting;
    Scene scene;
    bool sceneInitialized = false;
    std::string failedMap;
    std::unordered_map<std::string, ModelEntry> models;
    std::unordered_map<std::string, std::optional<MaterialInfo>> materials;
    // Textures uploaded for alpha tests and material masks, by VTF path.
    std::unordered_map<std::string, SceneTexture> textures;
    std::vector<std::string> mapMaterials;
    uint32_t modelErrors = 0;
    std::vector<entities::ModelEntity> entities;
    std::vector<SceneInstance> dynamicInstances;
    std::vector<SkinnedEntity> skinnedEntities;
    std::vector<studio::Matrix3x4> bones;
    std::vector<engine::DynamicLight> dynamicLights;
    std::vector<bsp::Mesh> brushModels;
    bool viewmodelCapture = false;
    // How far this frame's image is shifted, in render pixels (x right, y down), for Ray Reconstruction.
    float pixelOffset[2] = {};
    D3DVIEWPORT9 viewmodelViewport = {};
    const std::vector<SkinnedEntity> noSkinnedEntities;
};

State g_state;

void RunConsoleRequests()
{
    const Options& options = GetOptions();
    if (!options.devMode || !g_state.scene.Ready())
        return;
    const std::filesystem::path path = options.gameDirectory / L"gmodpp_console.txt";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"r") != 0)
        return;
    std::string commands;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), file) != nullptr)
        commands += buffer;
    std::fclose(file);
    DeleteFileW(path.c_str());
    if (!commands.empty() && commands.back() != '\n')
        commands += '\n';
    log::Info("Console request: {}", commands);
    engine::ExecuteCommand(commands);
}

std::vector<std::byte> ReadOptional(const std::string& path, size_t maxBytes = SIZE_MAX)
{
    std::vector<std::byte> data;
    if (!engine::ReadGameFile(path, data, maxBytes))
        data.clear();
    return data;
}

const SceneTexture& TextureFor(const std::string& vtfPath)
{
    auto cached = g_state.textures.find(vtfPath);
    if (cached != g_state.textures.end())
        return cached->second;
    SceneTexture& texture = g_state.textures[vtfPath];
    const std::vector<std::byte> file = ReadOptional(vtfPath);
    std::string error;
    const std::optional<studio::TextureImage> image = studio::ReadTexture(file, kSceneTextureSize, error);
    if (image) {
        constexpr DXGI_FORMAT kFormats[] = {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_R8_UNORM};
        texture.view = g_state.scene.AddTexture(kFormats[static_cast<int>(image->format)], image->width, image->height, image->mips);
        texture.alpha = image->alpha;
        if (texture.view == DescriptorHeap::kInvalid)
            error = "creating the texture failed";
    }
    if (texture.view == DescriptorHeap::kInvalid && ++g_state.modelErrors <= 10)
        log::Info("Scene: texture '{}' unusable ({}); alpha tests and masks that need it are skipped", vtfPath, file.empty() ? "file not found" : error);
    return texture;
}

// A VMT colour: "[r g b]" in 0-1 or "{r g b}" in 0-255; one number sets all three.
bsp::Float3 MaterialColor(std::string_view vmt, std::string_view key, bsp::Float3 fallback)
{
    std::optional<std::string> value = studio::MaterialValue(vmt, key);
    if (!value)
        return fallback;
    const float scale = value->find('{') != std::string::npos ? 1.0f / 255.0f : 1.0f;
    std::replace_if(value->begin(), value->end(), [](char c) { return c == '[' || c == ']' || c == '{' || c == '}'; }, ' ');
    float v[3] = {};
    int count = 0;
    const char* p = value->data();
    const char* end = p + value->size();
    while (count < 3) {
        while (p < end && std::isspace(static_cast<unsigned char>(*p)))
            ++p;
        const auto [next, ec] = std::from_chars(p, end, v[count]);
        if (ec != std::errc())
            break;
        p = next;
        ++count;
    }
    if (count == 1)
        v[1] = v[2] = v[0];
    else if (count != 3)
        return fallback;
    return {v[0] * scale, v[1] * scale, v[2] * scale};
}

// The scene material for a VMT's env_cubemap reflection and self-illumination (as LightmappedGeneric and
// VertexLitGeneric compute their masks), or Scene::kNoMaterial when it has neither.
uint32_t SceneMaterial(std::string_view vmt, const std::string& baseTexture)
{
    auto enabled = [&](std::string_view key) {
        const std::optional<std::string> value = studio::MaterialValue(vmt, key);
        return value && !value->empty() && value->front() != '0';
    };
    const std::optional<std::string> envmap = studio::MaterialValue(vmt, "$envmap");
    const bool reflective = envmap && !envmap->empty();
    const bool selfIllum = enabled("$selfillum");
    if (!reflective && !selfIllum)
        return Scene::kNoMaterial;
    auto mask = [](const std::string& texture, uint32_t channel, bool invert) {
        const UINT view = texture.empty() ? DescriptorHeap::kInvalid : TextureFor(texture).view;
        return view != DescriptorHeap::kInvalid ? Scene::PackMask(view, channel, invert) : Scene::kMaskOff;
    };
    GpuMaterial material = {};
    material.envmapMask = Scene::kMaskOff;
    if (reflective) {
        const std::string envmapMask = studio::MaterialTexture(vmt, "$envmapmask");
        if (!envmapMask.empty())
            material.envmapMask = mask(envmapMask, 0, false);
        else if (enabled("$basealphaenvmapmask"))
            material.envmapMask = mask(baseTexture, 3, true);
        else if (enabled("$normalmapalphaenvmapmask"))
            material.envmapMask = mask(studio::MaterialTexture(vmt, "$bumpmap"), 3, false);
        else
            material.envmapMask = Scene::PackMask(DescriptorHeap::kInvalid, 0, false);
        const bsp::Float3 tint = MaterialColor(vmt, "$envmaptint", {1.0f, 1.0f, 1.0f});
        material.envmapTint[0] = tint.x;
        material.envmapTint[1] = tint.y;
        material.envmapTint[2] = tint.z;
        material.envmapContrast = MaterialColor(vmt, "$envmapcontrast", {0.0f, 0.0f, 0.0f}).x;
        material.envmapSaturation = MaterialColor(vmt, "$envmapsaturation", {1.0f, 1.0f, 1.0f}).x;
    }
    material.selfIllumMask = Scene::kMaskOff;
    if (selfIllum) {
        const std::string selfIllumMask = studio::MaterialTexture(vmt, "$selfillummask");
        material.selfIllumMask = selfIllumMask.empty() ? mask(baseTexture, 3, false) : mask(selfIllumMask, 0, false);
        const bsp::Float3 tint = MaterialColor(vmt, "$selfillumtint", {1.0f, 1.0f, 1.0f});
        material.selfIllumTint[0] = tint.x;
        material.selfIllumTint[1] = tint.y;
        material.selfIllumTint[2] = tint.z;
    }
    if (material.envmapMask == Scene::kMaskOff && material.selfIllumMask == Scene::kMaskOff)
        return Scene::kNoMaterial;
    return g_state.scene.AddMaterial(material);
}

// The first of a texture's candidate VMTs that exists, with what the scene needs from it.
const std::optional<MaterialInfo>& MaterialFor(const std::string& vmtPath)
{
    auto cached = g_state.materials.find(vmtPath);
    if (cached != g_state.materials.end())
        return cached->second;
    std::optional<MaterialInfo>& entry = g_state.materials[vmtPath];
    std::vector<std::byte> vmt = ReadOptional(vmtPath);
    if (vmt.empty())
        return entry;
    std::string text(reinterpret_cast<const char*>(vmt.data()), vmt.size());
    // Maps ship cubemap-patched copies of their materials ("patch" shader) that include the original.
    if (std::optional<std::string> include = studio::MaterialValue(text, "include")) {
        std::string path = *include;
        std::replace(path.begin(), path.end(), '\\', '/');
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::vector<std::byte> included = ReadOptional(path);
        text.append("\n").append(reinterpret_cast<const char*>(included.data()), included.size());
    }
    auto enabled = [&](std::string_view key) {
        const std::optional<std::string> value = studio::MaterialValue(text, key);
        return value && !value->empty() && value->front() != '0';
    };
    entry.emplace();
    entry->translucent = enabled("$translucent");
    entry->alphaTest = enabled("$alphatest");
    if (std::optional<std::string> reference = studio::MaterialValue(text, "$alphatestreference")) {
        float value = 0.0f;
        if (std::from_chars(reference->data(), reference->data() + reference->size(), value).ec == std::errc() && value > 0.0f)
            entry->alphaReference = value;
    }
    entry->baseTexture = studio::MaterialTexture(text, "$basetexture");
    if (!entry->baseTexture.empty()) {
        std::vector<std::byte> header = ReadOptional(entry->baseTexture, studio::kVtfReflectivityHeaderSize);
        if (std::optional<bsp::Float3> reflectivity = studio::TextureReflectivity(header))
            entry->reflectivity = *reflectivity;
    }
    entry->sceneMaterial = SceneMaterial(text, entry->baseTexture);
    return entry;
}

// The alpha-test entry of a material's triangles, or kOpaqueTriangle.
uint32_t TriangleAlphaFor(const MaterialInfo& material)
{
    if (!material.alphaTest || material.baseTexture.empty())
        return kOpaqueTriangle;
    const SceneTexture& texture = TextureFor(material.baseTexture);
    return texture.view != DescriptorHeap::kInvalid && texture.alpha ? Scene::PackAlpha(texture.view, material.alphaReference) : kOpaqueTriangle;
}

// Moves the triangles whose `triangleAlpha` is not kOpaqueTriangle behind the others, reordering `indices` and
// every per-triangle vector in `perTriangle` alike.
template <class... PerTriangle>
TriangleData PartitionTriangles(std::vector<uint32_t>& indices, const std::vector<uint32_t>& triangleAlpha, PerTriangle&... perTriangle)
{
    const size_t triangles = triangleAlpha.size();
    std::vector<size_t> order(triangles);
    std::iota(order.begin(), order.end(), size_t{0});
    const auto firstAlpha = std::stable_partition(order.begin(), order.end(), [&](size_t t) { return triangleAlpha[t] == kOpaqueTriangle; });
    TriangleData partition;
    partition.opaqueTriangles = static_cast<UINT>(firstAlpha - order.begin());
    for (auto it = firstAlpha; it != order.end(); ++it)
        partition.alphaTriangles.push_back(triangleAlpha[*it]);
    if (partition.alphaTriangles.empty())
        return partition;
    std::vector<uint32_t> sortedIndices(indices.size());
    for (size_t i = 0; i < triangles; ++i)
        std::copy_n(indices.begin() + order[i] * 3, 3, sortedIndices.begin() + i * 3);
    indices = std::move(sortedIndices);
    auto permute = [&](auto& values) {
        std::remove_reference_t<decltype(values)> sorted(values.size());
        for (size_t i = 0; i < triangles; ++i)
            sorted[i] = values[order[i]];
        values = std::move(sorted);
    };
    (permute(perTriangle), ...);
    return partition;
}

// The map material (texdata) a BSP triangle uses, or nothing.
const std::optional<MaterialInfo>* MapMaterial(uint32_t texdata)
{
    if (texdata >= g_state.mapMaterials.size() || g_state.mapMaterials[texdata].empty())
        return nullptr;
    std::string path = "materials/" + g_state.mapMaterials[texdata] + ".vmt";
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return &MaterialFor(path);
}

// Partitions a BSP mesh by its materials' alpha tests and describes it for Scene::AddModel.
ModelGeometry PrepareMesh(bsp::Mesh& mesh, TriangleData& triangles)
{
    std::vector<uint32_t> triangleAlpha(mesh.triangleMaterials.size(), kOpaqueTriangle);
    for (size_t t = 0; t < triangleAlpha.size(); ++t) {
        const std::optional<MaterialInfo>* material = MapMaterial(mesh.triangleMaterials[t]);
        if (material != nullptr && *material)
            triangleAlpha[t] = TriangleAlphaFor(**material);
    }
    triangles = PartitionTriangles(mesh.indices, triangleAlpha, mesh.triangleAlbedo, mesh.triangleMaterials);
    triangles.materials.resize(mesh.triangleMaterials.size(), Scene::kNoMaterial);
    for (size_t t = 0; t < triangles.materials.size(); ++t) {
        const std::optional<MaterialInfo>* material = MapMaterial(mesh.triangleMaterials[t]);
        if (material != nullptr && *material)
            triangles.materials[t] = (*material)->sceneMaterial;
    }
    return {mesh.positions, mesh.indices, mesh.triangleAlbedo, triangles.opaqueTriangles, mesh.uvs, triangles.alphaTriangles, triangles.materials};
}

uint32_t PackAlbedo(const bsp::Float3& c)
{
    auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return channel(c.x) | channel(c.y) << 8 | channel(c.z) << 16 | 0xFF000000u;
}

// Loads a model once: a BLAS in its bind pose for rigid use, plus skinning data for multi-bone models.
// Brush entity models ("*N") come from the map.
const ModelEntry& ModelFor(const std::string& mdlPath)
{
    auto cached = g_state.models.find(mdlPath);
    if (cached != g_state.models.end())
        return cached->second;
    ModelEntry& entry = g_state.models[mdlPath];
    if (mdlPath.starts_with('*')) {
        size_t number = 0;
        const auto [end, ec] = std::from_chars(mdlPath.data() + 1, mdlPath.data() + mdlPath.size(), number);
        if (ec == std::errc() && end == mdlPath.data() + mdlPath.size() && number < g_state.brushModels.size()) {
            TriangleData triangles;
            entry.sceneModel = g_state.scene.AddModel(PrepareMesh(g_state.brushModels[number], triangles));
        }
        return entry;
    }

    const std::string base = mdlPath.size() > 4 ? mdlPath.substr(0, mdlPath.size() - 4) : mdlPath;
    std::vector<std::byte> mdl = ReadOptional(mdlPath);
    std::vector<std::byte> vvd = ReadOptional(base + ".vvd");
    std::vector<std::byte> vtx = ReadOptional(base + ".dx90.vtx");
    std::vector<std::vector<std::string>> texturePaths;
    studio::Geometry geometry;
    std::string error;
    if (mdl.empty() || vvd.empty() || vtx.empty()) {
        error = "model files are missing";
    } else if (studio::ReadMaterialPaths(mdl, texturePaths, error)) {
        std::vector<bool> skip(texturePaths.size(), false);
        std::vector<uint32_t> textureAlbedo(texturePaths.size(), PackAlbedo(kDefaultReflectivity));
        std::vector<uint32_t> textureAlpha(texturePaths.size(), kOpaqueTriangle);
        std::vector<uint32_t> textureMaterial(texturePaths.size(), Scene::kNoMaterial);
        for (size_t t = 0; t < texturePaths.size(); ++t) {
            for (const std::string& vmtPath : texturePaths[t]) {
                const std::optional<MaterialInfo>& material = MaterialFor(vmtPath);
                if (!material)
                    continue;
                skip[t] = material->translucent && !material->alphaTest;
                textureAlbedo[t] = PackAlbedo(material->reflectivity);
                textureAlpha[t] = TriangleAlphaFor(*material);
                textureMaterial[t] = material->sceneMaterial;
                break;
            }
        }
        if (studio::BuildGeometry(mdl, vvd, vtx, skip, geometry, error) && !geometry.indices.empty()) {
            std::vector<uint32_t> triangleAlbedo(geometry.triangleTextures.size());
            std::vector<uint32_t> triangleAlpha(geometry.triangleTextures.size());
            for (size_t i = 0; i < triangleAlbedo.size(); ++i) {
                const uint16_t texture = geometry.triangleTextures[i];
                triangleAlbedo[i] = texture < textureAlbedo.size() ? textureAlbedo[texture] : PackAlbedo(kDefaultReflectivity);
                triangleAlpha[i] = texture < textureAlpha.size() ? textureAlpha[texture] : kOpaqueTriangle;
            }
            TriangleData triangles = PartitionTriangles(geometry.indices, triangleAlpha, triangleAlbedo, geometry.triangleTextures);
            for (uint16_t texture : geometry.triangleTextures)
                triangles.materials.push_back(texture < textureMaterial.size() ? textureMaterial[texture] : Scene::kNoMaterial);
            entry.sceneModel = g_state.scene.AddModel({geometry.positions, geometry.indices, triangleAlbedo, triangles.opaqueTriangles, geometry.uvs,
                triangles.alphaTriangles, triangles.materials});
            if (entry.sceneModel >= 0 && studio::BoneCount(mdl) > 1) {
                auto skinned = std::make_shared<SkinnedModel>();
                skinned->opaqueTriangles = triangles.opaqueTriangles;
                skinned->geometry = std::move(geometry);
                if (studio::ReadPoseToBone(mdl, skinned->poseToBone) && g_state.lighting->Skinned().PrepareModel(*skinned))
                    entry.skinned = std::move(skinned);
            }
            return entry;
        }
    }
    if (!error.empty() && ++g_state.modelErrors <= 10)
        log::Info("Scene: model '{}' skipped: {}", mdlPath, error);
    return entry;
}

bool IsFirstPersonModel(const std::string& path)
{
    return path.starts_with("models/weapons/v_") || path.starts_with("models/weapons/c_");
}

bool IsHeldWeapon(const entities::ModelEntity& entity, const std::optional<std::array<float, 3>>& eye)
{
    if (!eye || !entity.model.starts_with("models/weapons/w_"))
        return false;
    float distanceSquared = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = entity.transform[i][3] - (*eye)[i];
        distanceSquared += d * d;
    }
    return distanceSquared < kHeldWeaponDistance * kHeldWeaponDistance;
}

void ClearScene()
{
    g_state.lighting->WaitForGpu();
    g_state.lighting->Skinned().Clear();
    g_state.lighting->InvalidateHistory();
    g_state.scene.Clear();
    g_state.models.clear();
    g_state.brushModels.clear();
    g_state.mapMaterials.clear();
    g_state.textures.clear();
    // Scene material indices and texture views are reset with the scene.
    g_state.materials.clear();
}

void UpdateScene()
{
    if (!engine::Ready())
        return;
    if (!g_state.sceneInitialized) {
        Renderer* renderer = GetRenderer();
        g_state.sceneInitialized = g_state.scene.Initialize(renderer->Device(), renderer->Queue(), renderer->Descriptors());
        if (!g_state.sceneInitialized)
            return;
    }
    std::string level = engine::LevelName();
    if (level.empty()) {
        if (g_state.scene.Ready()) {
            ClearScene();
            log::Info("Scene cleared");
        }
        return;
    }
    if (level == g_state.scene.Name() || level == g_state.failedMap)
        return;

    std::vector<std::byte> file;
    bsp::WorldGeometry world;
    std::string error;
    if (!engine::ReadGameFile(level, file)) {
        error = "the game filesystem could not read it";
    } else if (bsp::LoadWorld(file, world, error)) {
        ClearScene();
        g_state.mapMaterials = std::move(world.materials);
        TriangleData worldTriangles;
        const ModelGeometry worldGeometry = PrepareMesh(world.mesh, worldTriangles);
        if (g_state.scene.LoadMap(level, world, worldGeometry)) {
            g_state.brushModels = std::move(world.brushModels);
            std::vector<SceneInstance> instances;
            const auto addProps = [&](const std::vector<bsp::StaticProp>& props, UINT mask) {
                for (const bsp::StaticProp& prop : props) {
                    SceneInstance& instance = instances.emplace_back();
                    instance.model = ModelFor(world.propModels[prop.model]).sceneModel;
                    instance.mask = mask;
                    std::memcpy(instance.transform, prop.transform, sizeof(instance.transform));
                }
            };
            addProps(world.props, 0);
            const size_t worldProps = std::count_if(instances.begin(), instances.end(), [](const SceneInstance& instance) { return instance.model >= 0; });
            if (!world.skybox.indices.empty()) {
                TriangleData skyboxTriangles;
                SceneInstance& skybox = instances.emplace_back();
                skybox.model = g_state.scene.AddModel(PrepareMesh(world.skybox, skyboxTriangles));
                skybox.mask = kSkyboxMask;
                const float identity[3][4] = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
                std::memcpy(skybox.transform, identity, sizeof(skybox.transform));
            }
            addProps(world.skyboxProps, kSkyboxMask);
            std::erase_if(instances, [](const SceneInstance& instance) { return instance.model < 0; });
            log::Info("Scene '{}': {} faces, {} displacements, {} skybox faces and {} skybox props, {} world triangles ({} alpha-tested), {} of {} static props, {} lights; "
                      "sun {} dir ({:.2f} {:.2f} {:.2f}) color ({:.2f} {:.2f} {:.2f}), sky ({:.2f} {:.2f} {:.2f})",
                level, world.faceCount, world.displacementCount, world.skyboxFaceCount, world.skyboxProps.size(), world.mesh.indices.size() / 3, worldTriangles.alphaTriangles.size(),
                worldProps,
                world.props.size(), world.lights.size(), world.sun.present ? "present" : "missing", world.sun.direction.x, world.sun.direction.y,
                world.sun.direction.z, world.sun.color.x, world.sun.color.y, world.sun.color.z, world.sun.ambient.x, world.sun.ambient.y,
                world.sun.ambient.z);
            g_state.scene.SetStaticInstances(std::move(instances));
            return;
        }
        error = "building acceleration structures failed";
    }
    log::Info("Scene: cannot load '{}': {}", level, error);
    g_state.failedMap = level;
}

void CollectEntities()
{
    g_state.dynamicInstances.clear();
    g_state.skinnedEntities.clear();
    entities::Collect(g_state.entities);
    const std::optional<std::array<float, 3>> eye = gbuffer::EyePosition(gbuffer::LastCaptureViewProj());
    const int localPlayer = engine::LocalPlayerIndex();
    for (const entities::ModelEntity& entity : g_state.entities) {
        if (IsFirstPersonModel(entity.model) || IsHeldWeapon(entity, eye))
            continue;
        const ModelEntry& model = ModelFor(entity.model);
        if (model.sceneModel < 0)
            continue;
        const UINT mask = entity.index == localPlayer ? kLocalPlayerMask : kDynamicMask;
        if (model.skinned) {
            g_state.bones.resize(kMaxBones);
            if (!entities::SetupBones(entity.renderable, reinterpret_cast<float (*)[3][4]>(g_state.bones.data()), kMaxBones))
                continue;
            g_state.bones.resize(std::min<size_t>(model.skinned->poseToBone.size(), kMaxBones));
            g_state.skinnedEntities.push_back({entity.index, model.sceneModel, model.skinned, g_state.bones, mask});
            continue;
        }
        SceneInstance& instance = g_state.dynamicInstances.emplace_back();
        instance.model = model.sceneModel;
        instance.mask = mask;
        std::memcpy(instance.transform, entity.transform, sizeof(instance.transform));
    }
}

}

bool PrepareDeviceParameters(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS& params)
{
    D3DDISPLAYMODE mode;
    HRESULT hr = d3d->GetAdapterDisplayMode(adapter, &mode);
    if (SUCCEEDED(hr))
        hr = d3d->CheckDeviceFormat(adapter, D3DDEVTYPE_HAL, mode.Format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, kIntzFormat);
    if (FAILED(hr)) {
        log::Info("Effects disabled: INTZ depth textures are not supported ({:#010x})", Hex(hr));
        g_state.depthReplaced = false;
        return false;
    }
    if (params.MultiSampleType != D3DMULTISAMPLE_NONE)
        log::Info("Effects: disabling {}x MSAA on the back buffer so depth can be read", static_cast<int>(params.MultiSampleType));
    params.MultiSampleType = D3DMULTISAMPLE_NONE;
    params.MultiSampleQuality = 0;
    params.EnableAutoDepthStencil = FALSE;
    g_state.depthReplaced = true;
    return true;
}

void OnDeviceReady(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& params)
{
    g_state.device = device;
    g_state.renderThread = GetCurrentThreadId();
    if (!g_state.depthReplaced)
        return;

    HRESULT hr = device->CreateTexture(params.BackBufferWidth, params.BackBufferHeight, 1, D3DUSAGE_DEPTHSTENCIL, kIntzFormat,
        D3DPOOL_DEFAULT, &g_state.depthTexture, nullptr);
    if (SUCCEEDED(hr))
        hr = g_state.depthTexture->GetSurfaceLevel(0, &g_state.depthSurface);
    if (SUCCEEDED(hr))
        hr = device->SetDepthStencilSurface(g_state.depthSurface.Get());
    if (FAILED(hr)) {
        log::Info("Effects: creating the INTZ depth buffer failed ({:#010x})", Hex(hr));
        g_state.depthTexture.Reset();
        g_state.depthSurface.Reset();
        return;
    }
    log::Info("Effects: INTZ depth buffer {}x{} bound", params.BackBufferWidth, params.BackBufferHeight);
    g_state.gbufferReady = gbuffer::Initialize(device, params);

    Renderer* renderer = GetRenderer();
    if (!g_state.lighting && renderer != nullptr && renderer->SupportsRaytracing()) {
        auto lighting = std::make_unique<Lighting>();
        if (lighting->Initialize(*renderer, device))
            g_state.lighting = std::move(lighting);
    }
}

void OnBeforeReset()
{
    if (g_state.lighting)
        g_state.lighting->WaitForGpu();
    gbuffer::Release();
    g_state.gbufferReady = false;
    g_state.depthSurface.Reset();
    g_state.depthTexture.Reset();
}

namespace {
void FinishCapture(gbuffer::ViewKind kind);
}

void OnPresent()
{
    ++g_state.frame;
    if (g_state.viewmodelCapture) {
        g_state.viewmodelCapture = false;
        FinishCapture(gbuffer::ViewKind::Viewmodel);
    }
    if (gbuffer::Active() && g_state.device != nullptr) {
        log::Info("Effects: a G-buffer capture was still open at present; closing it");
        gbuffer::End(g_state.device, gbuffer::ViewKind::Main);
    }
    client::TryInstall();
    engine::TryInitialize();
    entities::TryInitialize();
    if (g_state.frame % kMapPollInterval == 0)
        SyncSettingsFile(GetOptions().gameDirectory / L"gmodpp_settings.ini");
    if (g_state.lighting && g_state.frame % kMapPollInterval == 0) {
        UpdateScene();
        RunConsoleRequests();
    }
}

namespace {

// A capture can start when the game draws into the back buffer with our depth buffer on the render thread.
bool CanCapture(IDirect3DDevice9* device, ComPtr<IDirect3DSurface9>& backBuffer)
{
    if (device == nullptr || !g_state.lighting || !g_state.gbufferReady || !g_state.scene.Ready() || gbuffer::Active() ||
        !GetSettings().realtimeLighting)
        return false;
    if (GetCurrentThreadId() != g_state.renderThread) {
        if (!g_state.warnedQueuedRendering) {
            g_state.warnedQueuedRendering = true;
            log::Info("Effects: views are drawn on a different thread than Direct3D (mat_queue_mode != 0); effects disabled");
        }
        return false;
    }
    ComPtr<IDirect3DSurface9> renderTarget;
    ComPtr<IDirect3DSurface9> depthStencil;
    if (FAILED(device->GetRenderTarget(0, &renderTarget)) || FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) ||
        FAILED(device->GetDepthStencilSurface(&depthStencil)))
        return false;
    return renderTarget == backBuffer && depthStencil == g_state.depthSurface;
}

void FinishCapture(gbuffer::ViewKind kind)
{
    IDirect3DDevice9* device = g_state.device;
    if (device == nullptr || !gbuffer::Active())
        return;
    gbuffer::End(device, kind);
    ComPtr<IDirect3DSurface9> backBuffer;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
        return;
    switch (kind) {
    case gbuffer::ViewKind::Main:
        CollectEntities();
        engine::ActiveDynamicLights(g_state.dynamicLights);
        break;
    case gbuffer::ViewKind::Skybox:
        g_state.dynamicInstances.clear();
        g_state.skinnedEntities.clear();
        g_state.dynamicLights.clear();
        break;
    case gbuffer::ViewKind::Viewmodel:
        // Same scene as the main view; characters are not skinned a second time.
        break;
    }
    g_state.scene.SetDynamicLights(g_state.dynamicLights);
    ViewInputs inputs{backBuffer.Get(), g_state.depthTexture.Get(), gbuffer::NormalTexture(), gbuffer::FlagsTexture(), gbuffer::LightTexture(),
        gbuffer::AlbedoTexture(), gbuffer::EmissiveTexture(), gbuffer::ProjectedLights(), kind, 0.0f, 1.0f, g_state.frame, g_state.pixelOffset[0],
        g_state.pixelOffset[1]};
    if (kind == gbuffer::ViewKind::Viewmodel) {
        inputs.depthMin = g_state.viewmodelViewport.MinZ;
        inputs.depthMax = g_state.viewmodelViewport.MaxZ;
    }
    const std::vector<SkinnedEntity>& skinned = kind == gbuffer::ViewKind::Viewmodel ? g_state.noSkinnedEntities : g_state.skinnedEntities;
    g_state.lighting->Run(inputs, g_state.scene, g_state.dynamicInstances, skinned);
}

// Source draws the viewmodel with DepthRange(0, 0.1); a few sprites use 0..0.01, which is not the viewmodel.
bool IsViewmodelRange(const D3DVIEWPORT9& viewport) { return viewport.MinZ == 0.0f && std::fabs(viewport.MaxZ - 0.1f) < 1e-4f; }

}

void OnViewport(const D3DVIEWPORT9& viewport)
{
    if (g_state.viewmodelCapture) {
        if (IsViewmodelRange(viewport))
            return;
        g_state.viewmodelCapture = false;
        FinishCapture(gbuffer::ViewKind::Viewmodel);
        return;
    }
    ComPtr<IDirect3DSurface9> backBuffer;
    if (!IsViewmodelRange(viewport) || !CanCapture(g_state.device, backBuffer))
        return;
    // The viewmodel is drawn after the anti-aliasing resolve, so it is not jittered.
    gbuffer::SetJitter(0.0f, 0.0f);
    g_state.viewmodelViewport = viewport;
    g_state.viewmodelCapture = gbuffer::Begin(g_state.device);
}

void OnViewOpaqueBegin()
{
    IDirect3DDevice9* device = g_state.device;
    ComPtr<IDirect3DSurface9> backBuffer;
    if (!CanCapture(device, backBuffer))
        return;
    // A Halton(2, 3) sub-pixel pattern for temporal anti-aliasing, in NDC units (y is up in NDC).
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    D3DSURFACE_DESC desc;
    backBuffer->GetDesc(&desc);
    UINT renderWidth = 0;
    UINT renderHeight = 0;
    g_state.pixelOffset[0] = g_state.pixelOffset[1] = 0.0f;
    if (g_state.lighting->RayReconstructionSize(desc.Width, desc.Height, renderWidth, renderHeight)) {
        // Ray Reconstruction wants many phases, more as the upscale grows (DLSS programming guide 3.7.1).
        const float ratio = static_cast<float>(desc.Width) / static_cast<float>(renderWidth);
        const uint32_t phases = std::max(kRayReconstructionPhases, static_cast<uint32_t>(std::ceil(8.0f * ratio * ratio)));
        const uint32_t sample = static_cast<uint32_t>(g_state.frame % phases) + 1;
        g_state.pixelOffset[0] = Halton(sample, 2) - 0.5f;
        g_state.pixelOffset[1] = Halton(sample, 3) - 0.5f;
        // DLSS takes the image shift itself, as NVIDIA's Donut samples pass it (PlanarView pixel offset).
        jitterX = g_state.pixelOffset[0] * 2.0f / static_cast<float>(renderWidth);
        jitterY = -g_state.pixelOffset[1] * 2.0f / static_cast<float>(renderHeight);
    } else if (GetSettings().antiAliasing == AntiAliasing::Temporal) {
        const uint32_t sample = static_cast<uint32_t>(g_state.frame % kJitterSamples) + 1;
        jitterX = (Halton(sample, 2) - 0.5f) * 2.0f / static_cast<float>(desc.Width);
        jitterY = -(Halton(sample, 3) - 0.5f) * 2.0f / static_cast<float>(desc.Height);
    }
    gbuffer::SetJitter(jitterX, jitterY);
    gbuffer::Begin(device);
}

void OnViewOpaqueEnd(bool inSkybox)
{
    if (!g_state.viewmodelCapture)
        FinishCapture(inSkybox ? gbuffer::ViewKind::Skybox : gbuffer::ViewKind::Main);
}

}
