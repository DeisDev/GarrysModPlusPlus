#include "world.h"

#include "bsp.h"
#include "client_hooks.h"
#include "convex_hull.h"
#include "engine_interfaces.h"
#include "entities.h"
#include "light_queries.h"
#include "log.h"
#include "options.h"
#include "ropes.h"
#include "scene.h"
#include "settings.h"
#include "skinned_casters.h"
#include "studio_model.h"
#include "swaying_props.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace gmodpp::world {

namespace {


constexpr UINT kMapPollInterval = 30;
constexpr int kMaxBones = 256;
// The bone weapons' world models are merged onto (their root bone) and players hold them by.
constexpr char kRightHandBone[] = "ValveBiped.Bip01_R_Hand";
constexpr bsp::Float3 kDefaultReflectivity = {0.5f, 0.5f, 0.5f};
constexpr float kDefaultAlphaReference = 0.5f;
// Alpha and mask textures are read from a mip no larger than this; shadows and reflections need no more detail.
constexpr uint32_t kSceneTextureSize = 256;
constexpr uint32_t kOpaqueTriangle = 0xFFFFFFFF;
// Never a PackAlpha value (they always have a texture view below 0xFFFFFF) nor a PackTranslucent one (bit 31 set).
constexpr uint32_t kDroppedTriangle = 0x7FFFFFFF;

constexpr uint32_t kJitterSamples = 8;
constexpr uint32_t kRayReconstructionPhases = 32;
// VRAD shadows a static prop by the convex pieces of its collision model's first solid or, without one, by the
// convex hull of each of its meshes (CreateCollisionModel and AddPolysForRayTrace in
// utils/vrad/vradstaticprops.cpp). Lights placed in lamp models lose most of their light to those hulls, which the
// lamps' open meshes let out, so lights this close to a hull are blocked by it.
constexpr float kFixtureMargin = 2.0f;

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
    // Where the model is, in its own space: brush models (`*N`) keep their faces away from their origin.
    bsp::Float3 center = {};
    // Its vertices' sway when some of its materials sway in the wind.
    std::shared_ptr<SwayingModel> swaying;
    // A single-bone model's bind pose to its bone. It is drawn where its sequence puts that bone, not by its entity's
    // transform: the bone may turn away from its bind pose (models/nova/chair_plastic01 faces the other way), and
    // weapons are merged onto their holder's hand bone.
    std::optional<studio::Matrix3x4> rootPoseToBone;
    // Its kRightHandBone, if it has one: a holder's hand, or where a weapon merged onto that hand is held.
    int rightHand = -1;
    std::optional<studio::Matrix3x4> rightHandPoseToBone;
};

struct MaterialInfo {
    // Translucent surfaces (glass, foliage) let part of the light through; alpha-tested ones (foliage, fences) are
    // cut by their alpha. Clear ones block nothing: additive light effects (beams, glows) and Refract glass, which
    // VBSP makes a non-solid window brush that VRAD's shadows ignore (FindMiptex, AddBrushToRaytraceEnvironment).
    bool translucent = false;
    bool clear = false;
    bool alphaTest = false;
    float alphaReference = kDefaultAlphaReference;
    // $alpha.
    float opacity = 1.0f;
    std::string baseTexture;
    bsp::Float3 reflectivity = kDefaultReflectivity;
    // Reflections and self-illumination, for Scene::AddMaterial.
    uint32_t sceneMaterial = Scene::kNoMaterial;
    // Index of its $treesway parameters in State::swayMaterials.
    uint32_t swayMaterial = SwayingModel::kNoSway;
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
    ID3D12Device5* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    DescriptorHeap* descriptors = nullptr;
    bool opacityMicromaps = false;
    uint64_t frame = 0;
    // The main view's size, whose centre gmodpp_probe.request reports by default.
    UINT width = 0;
    UINT height = 0;
    std::unique_ptr<Lighting> lighting;
    Scene scene;
    bool sceneInitialized = false;
    std::string failedMap;
    std::unordered_map<std::string, ModelEntry> models;
    std::unordered_map<std::string, std::optional<MaterialInfo>> materials;
    std::vector<SwayMaterial> swayMaterials;
    // Textures uploaded for alpha tests and material masks, by VTF path.
    std::unordered_map<std::string, SceneTexture> textures;
    std::vector<std::string> mapMaterials;
    uint32_t modelErrors = 0;
    std::vector<entities::ModelEntity> entities;
    std::vector<SceneInstance> dynamicInstances;
    // Last main view's transform of each rigid dynamic entity (by entity index) and rope segment (RopeSegmentKey),
    // for motion vectors.
    std::unordered_map<int64_t, std::array<std::array<float, 4>, 3>> previousTransforms;
    std::unordered_map<int64_t, std::array<std::array<float, 4>, 3>> currentTransforms;
    std::vector<entities::Rope> ropes;
    // The rope prism's scene model for each rope material, or -1 for materials that cast no shadow.
    std::unordered_map<std::string, int> ropeModels;
    bool loggedRopes = false;
    std::vector<SkinnedEntity> skinnedEntities;
    std::vector<studio::Matrix3x4> bones;
    std::vector<engine::DynamicLight> dynamicLights;
    // The local player's origin from the last main view's entities.
    std::optional<std::array<float, 3>> localPlayer;
    std::vector<bsp::Mesh> brushModels;
    // The map's 3D skybox test (BSP tree, leaf areas and bounds), to keep its entities out of the main view's rays.
    bsp::WorldGeometry skyboxTest;
    // How far this frame's image is shifted, in render pixels (x right, y down).
    float pixelOffset[2] = {};
    const std::vector<SkinnedEntity> noSkinnedEntities;
};

State g_state;

void RunConsoleRequests(bool console)
{
    const Options& options = GetOptions();
    if (!options.devMode || !g_state.scene.Ready())
        return;
    // "x y": a pixel of the main view, by default its centre.
    const std::filesystem::path probe = options.gameDirectory / L"gmodpp_probe.request";
    if (FILE* request = nullptr; _wfopen_s(&request, probe.c_str(), L"r") == 0) {
        unsigned x = 0;
        unsigned y = 0;
        const bool given = fscanf_s(request, "%u %u", &x, &y) == 2;
        std::fclose(request);
        DeleteFileW(probe.c_str());
        g_state.lighting->RequestProbe(given ? x : g_state.width / 2, given ? y : g_state.height / 2);
    }
    if (!console)
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

float MaterialFloat(std::string_view vmt, std::string_view key, float fallback)
{
    const std::optional<std::string> value = studio::MaterialValue(vmt, key);
    float result = fallback;
    if (value && std::from_chars(value->data(), value->data() + value->size(), result).ec != std::errc())
        result = fallback;
    return result;
}

// A material's $treesway parameters, with the defaults VertexLitGeneric's InitParams gives missing ones
// (vertexlitgeneric_dx9_helper.cpp).
std::optional<SwayMaterial> MaterialSway(std::string_view vmt)
{
    const std::optional<std::string> mode = studio::MaterialValue(vmt, "$treesway");
    if (!mode || mode->empty() || (mode->front() != '1' && mode->front() != '2'))
        return std::nullopt;
    SwayMaterial sway;
    sway.mode = mode->front() == '2' ? 2u : 1u;
    sway.height = MaterialFloat(vmt, "$treeswayheight", sway.height);
    sway.startHeight = MaterialFloat(vmt, "$treeswaystartheight", sway.startHeight);
    sway.radius = MaterialFloat(vmt, "$treeswayradius", sway.radius);
    sway.startRadius = MaterialFloat(vmt, "$treeswaystartradius", sway.startRadius);
    sway.speed = MaterialFloat(vmt, "$treeswayspeed", sway.speed);
    sway.strength = MaterialFloat(vmt, "$treeswaystrength", sway.strength);
    sway.scrumbleFrequency = MaterialFloat(vmt, "$treeswayscrumblefrequency", sway.scrumbleFrequency);
    sway.scrumbleStrength = MaterialFloat(vmt, "$treeswayscrumblestrength", sway.scrumbleStrength);
    sway.highWindSpeedMultiplier = MaterialFloat(vmt, "$treeswayspeedhighwindmultiplier", sway.highWindSpeedMultiplier);
    sway.scrumbleFalloffExp = MaterialFloat(vmt, "$treeswayscrumblefalloffexp", sway.scrumbleFalloffExp);
    sway.falloffExp = MaterialFloat(vmt, "$treeswayfalloffexp", sway.falloffExp);
    sway.scrumbleSpeed = MaterialFloat(vmt, "$treeswayscrumblespeed", sway.scrumbleSpeed);
    sway.speedLerpStart = MaterialFloat(vmt, "$treeswayspeedlerpstart", sway.speedLerpStart);
    sway.speedLerpEnd = MaterialFloat(vmt, "$treeswayspeedlerpend", sway.speedLerpEnd);
    const std::optional<std::string> fixedWind = studio::MaterialValue(vmt, "$treeswaystatic");
    sway.staticWind = fixedWind && !fixedWind->empty() && fixedWind->front() != '0' ? 1u : 0u;
    return sway;
}

// The shader a VMT names (its first token), in lower case.
std::string ShaderName(std::string_view vmt)
{
    size_t i = 0;
    while (i < vmt.size()) {
        if (std::isspace(static_cast<unsigned char>(vmt[i]))) {
            ++i;
        } else if (vmt.substr(i, 2) == "//") {
            i = vmt.find('\n', i);
            if (i == std::string_view::npos)
                return {};
        } else {
            break;
        }
    }
    const bool quoted = i < vmt.size() && vmt[i] == '"';
    i += quoted ? 1 : 0;
    std::string name;
    for (; i < vmt.size(); ++i) {
        const char c = vmt[i];
        if (quoted ? c == '"' : std::isspace(static_cast<unsigned char>(c)) || c == '{')
            break;
        name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

// Shaders whose surfaces show their texture without lighting. Their LIGHTING_PREVIEW variants write the G-buffer
// like lit ones, so the material table tells them apart.
bool IsUnlitShader(std::string_view shader) { return shader == "unlitgeneric" || shader == "unlittwotexture" || shader == "monitorscreen"; }

// The scene material for a VMT's env_cubemap reflection and self-illumination (as LightmappedGeneric and
// VertexLitGeneric compute their masks) and whether it is unlit, or Scene::kNoMaterial when it has none of them.
uint32_t SceneMaterial(std::string_view vmt, std::string_view shader, const std::string& baseTexture, bool unlit)
{
    auto enabled = [&](std::string_view key) {
        const std::optional<std::string> value = studio::MaterialValue(vmt, key);
        return value && !value->empty() && value->front() != '0';
    };
    const std::optional<std::string> envmap = studio::MaterialValue(vmt, "$envmap");
    const bool reflective = envmap && !envmap->empty();
    const bool selfIllum = enabled("$selfillum");
    if (!reflective && !selfIllum && !unlit)
        return Scene::kNoMaterial;
    auto mask = [](const std::string& texture, uint32_t channel, bool invert) {
        const UINT view = texture.empty() ? DescriptorHeap::kInvalid : TextureFor(texture).view;
        return view != DescriptorHeap::kInvalid ? Scene::PackMask(view, channel, invert) : Scene::kMaskOff;
    };
    GpuMaterial material = {};
    material.flags = unlit ? Scene::kMaterialUnlit : 0u;
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
        // LightmappedGeneric's defaults (lightmappedgeneric_dx9.cpp): scale 0, min-max [0 1]; its shader multiplies
        // the reflection by lerp(1, saturate((lightmap - x) * (x + y)), scale).
        if (shader == "lightmappedgeneric" || shader == "worldvertextransition") {
            material.envmapLightScale = MaterialFloat(vmt, "$envmaplightscale", 0.0f);
            const bsp::Float3 minMax = MaterialColor(vmt, "$envmaplightscaleminmax", {0.0f, 1.0f, 0.0f});
            material.envmapLightMin = minMax.x;
            material.envmapLightRange = minMax.x + minMax.y;
        }
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
    if (material.envmapMask == Scene::kMaskOff && material.selfIllumMask == Scene::kMaskOff && !unlit)
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
    std::string shader = ShaderName(text);
    // Maps ship cubemap-patched copies of their materials ("patch" shader) that include the original.
    if (std::optional<std::string> include = studio::MaterialValue(text, "include")) {
        std::string path = *include;
        std::replace(path.begin(), path.end(), '\\', '/');
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::vector<std::byte> included = ReadOptional(path);
        const std::string_view includedText(reinterpret_cast<const char*>(included.data()), included.size());
        if (shader == "patch")
            shader = ShaderName(includedText);
        text.append("\n").append(includedText);
    }
    auto enabled = [&](std::string_view key) {
        const std::optional<std::string> value = studio::MaterialValue(text, key);
        return value && !value->empty() && value->front() != '0';
    };
    entry.emplace();
    if (std::optional<std::string> alpha = studio::MaterialValue(text, "$alpha")) {
        float value = 1.0f;
        if (std::from_chars(alpha->data(), alpha->data() + alpha->size(), value).ec == std::errc())
            entry->opacity = std::clamp(value, 0.0f, 1.0f);
    }
    entry->translucent = enabled("$translucent") || entry->opacity < 1.0f;
    entry->clear = enabled("$additive") || shader == "refract";
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
    entry->sceneMaterial = SceneMaterial(text, shader, entry->baseTexture, IsUnlitShader(shader));
    if (const std::optional<SwayMaterial> sway = MaterialSway(text)) {
        entry->swayMaterial = static_cast<uint32_t>(g_state.swayMaterials.size());
        g_state.swayMaterials.push_back(*sway);
    }
    return entry;
}

// The alpha-test or translucency entry of a material's triangles, kOpaqueTriangle, or kDroppedTriangle for
// clear materials.
uint32_t TriangleAlphaFor(const MaterialInfo& material)
{
    if (material.clear)
        return kDroppedTriangle;
    if (!material.alphaTest && !material.translucent)
        return kOpaqueTriangle;
    const SceneTexture* texture = material.baseTexture.empty() ? nullptr : &TextureFor(material.baseTexture);
    const UINT alphaView = texture != nullptr && texture->view != DescriptorHeap::kInvalid && texture->alpha ? texture->view : DescriptorHeap::kInvalid;
    if (material.alphaTest)
        return alphaView != DescriptorHeap::kInvalid ? Scene::PackAlpha(alphaView, material.alphaReference) : kOpaqueTriangle;
    return Scene::PackTranslucent(alphaView, material.opacity);
}

// Moves the triangles whose `triangleAlpha` is not kOpaqueTriangle behind the others and removes those marked
// kDroppedTriangle, reordering `indices` and every per-triangle vector in `perTriangle` alike.
template <class... PerTriangle>
TriangleData PartitionTriangles(std::vector<uint32_t>& indices, const std::vector<uint32_t>& triangleAlpha, PerTriangle&... perTriangle)
{
    std::vector<size_t> order;
    order.reserve(triangleAlpha.size());
    for (size_t t = 0; t < triangleAlpha.size(); ++t) {
        if (triangleAlpha[t] != kDroppedTriangle)
            order.push_back(t);
    }
    const size_t triangles = order.size();
    const auto firstAlpha = std::stable_partition(order.begin(), order.end(), [&](size_t t) { return triangleAlpha[t] == kOpaqueTriangle; });
    TriangleData partition;
    partition.opaqueTriangles = static_cast<UINT>(firstAlpha - order.begin());
    for (auto it = firstAlpha; it != order.end(); ++it)
        partition.alphaTriangles.push_back(triangleAlpha[*it]);
    if (partition.alphaTriangles.empty() && triangles == triangleAlpha.size())
        return partition;
    std::vector<uint32_t> sortedIndices(triangles * 3);
    for (size_t i = 0; i < triangles; ++i)
        std::copy_n(indices.begin() + order[i] * 3, 3, sortedIndices.begin() + i * 3);
    indices = std::move(sortedIndices);
    auto permute = [&](auto& values) {
        std::remove_reference_t<decltype(values)> sorted(triangles);
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
    triangles = PartitionTriangles(mesh.indices, triangleAlpha, mesh.triangleAlbedo, mesh.triangleMaterials, mesh.triangleFaces);
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
            bsp::Mesh& mesh = g_state.brushModels[number];
            if (!mesh.positions.empty()) {
                bsp::Float3 low = mesh.positions[0];
                bsp::Float3 high = low;
                for (const bsp::Float3& p : mesh.positions) {
                    low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                    high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
                }
                entry.center = {(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f};
            }
            TriangleData triangles;
            entry.sceneModel = g_state.scene.AddModel(PrepareMesh(mesh, triangles));
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
        std::vector<uint32_t> textureSway(texturePaths.size(), SwayingModel::kNoSway);
        for (size_t t = 0; t < texturePaths.size(); ++t) {
            for (const std::string& vmtPath : texturePaths[t]) {
                const std::optional<MaterialInfo>& material = MaterialFor(vmtPath);
                if (!material)
                    continue;
                skip[t] = material->clear;
                textureAlbedo[t] = PackAlbedo(material->reflectivity);
                textureAlpha[t] = TriangleAlphaFor(*material);
                textureMaterial[t] = material->sceneMaterial;
                textureSway[t] = material->swayMaterial;
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
            if (entry.sceneModel >= 0 && std::any_of(textureSway.begin(), textureSway.end(), [](uint32_t sway) { return sway != SwayingModel::kNoSway; })) {
                auto swaying = std::make_shared<SwayingModel>();
                swaying->sceneModel = entry.sceneModel;
                swaying->vertexMaterials.assign(geometry.positions.size(), SwayingModel::kNoSway);
                for (size_t t = 0; t < geometry.triangleTextures.size(); ++t) {
                    const uint16_t texture = geometry.triangleTextures[t];
                    for (size_t corner = 0; corner < 3; ++corner) {
                        if (texture < textureSway.size() && textureSway[texture] != SwayingModel::kNoSway)
                            swaying->vertexMaterials[geometry.indices[t * 3 + corner]] = textureSway[texture];
                    }
                }
                entry.swaying = std::move(swaying);
            }
            std::vector<studio::Matrix3x4> poseToBone;
            if (entry.sceneModel >= 0 && studio::ReadPoseToBone(mdl, poseToBone)) {
                if (poseToBone.size() == 1)
                    entry.rootPoseToBone = poseToBone[0];
                entry.rightHand = studio::BoneIndex(mdl, kRightHandBone);
                if (entry.rightHand >= 0 && static_cast<size_t>(entry.rightHand) < poseToBone.size())
                    entry.rightHandPoseToBone = poseToBone[entry.rightHand];
            }
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

// The convex hulls VRAD shadowed a static prop's model with, as model-space points (CreateCollisionModel in
// src/utils/vrad/vradstaticprops.cpp): the pieces of its collision model's first solid, or for a model without one a
// hull around each of its meshes (ComputeConvexHull).
std::vector<std::vector<bsp::Float3>> VradPropHulls(const std::string& path, bool& fromCollision)
{
    std::vector<std::vector<bsp::Float3>> hulls;
    const std::string base = path.size() > 4 ? path.substr(0, path.size() - 4) : path;
    const std::vector<std::byte> phy = ReadOptional(base + ".phy");
    studio::Geometry geometry;
    std::string error;
    fromCollision = !phy.empty();
    if (!phy.empty()) {
        if (!studio::ReadCollisionPieces(phy, hulls, error) && ++g_state.modelErrors <= 10)
            log::Info("Scene: collision model of '{}' skipped: {}", path, error);
    } else if (studio::BuildGeometry(ReadOptional(path), ReadOptional(base + ".vvd"), ReadOptional(base + ".dx90.vtx"), {}, geometry, error)) {
        std::vector<std::vector<uint32_t>> vertices;
        for (size_t t = 0; t < geometry.triangleTextures.size(); ++t) {
            const size_t mesh = geometry.triangleTextures[t];
            if (vertices.size() <= mesh)
                vertices.resize(mesh + 1);
            vertices[mesh].insert(vertices[mesh].end(), geometry.indices.begin() + t * 3, geometry.indices.begin() + t * 3 + 3);
        }
        for (std::vector<uint32_t>& mesh : vertices) {
            std::sort(mesh.begin(), mesh.end());
            mesh.erase(std::unique(mesh.begin(), mesh.end()), mesh.end());
            std::vector<bsp::Float3>& points = hulls.emplace_back();
            for (uint32_t v : mesh)
                points.push_back(geometry.positions[v]);
        }
    }
    return hulls;
}

// The convex hulls of static props that VRAD's shadows hid most of a light behind: those the light sits in or
// against.
std::vector<LightHull> FixtureHulls(const bsp::WorldGeometry& world)
{
    struct PropModel {
        bool read = false;
        bool bounded = false;
        bsp::Float3 mins = {};
        bsp::Float3 maxs = {};
        bool hullsRead = false;
        bool fromCollision = false;
        // Model-space points of each convex hull VRAD shadowed the model with.
        std::vector<std::vector<bsp::Float3>> hulls;
    };
    std::vector<PropModel> models(world.propModels.size());
    const auto transform = [](const float m[3][4], const bsp::Float3& p) {
        return bsp::Float3{m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3], m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
    };
    std::vector<LightHull> result;
    std::vector<uint32_t> nearby;
    size_t collisionHulls = 0;
    size_t failedHulls = 0;
    std::string failedModels;
    for (const bsp::StaticProp& prop : world.props) {
        if (prop.noShadow)
            continue;
        PropModel& model = models[prop.model];
        const std::string& path = world.propModels[prop.model];
        if (!model.read) {
            model.read = true;
            model.bounded = studio::ReadBounds(ReadOptional(path), model.mins, model.maxs);
        }
        bsp::Float3 low = {INFINITY, INFINITY, INFINITY};
        bsp::Float3 high = {-INFINITY, -INFINITY, -INFINITY};
        for (int corner = 0; corner < 8 && model.bounded; ++corner) {
            const bsp::Float3 p = transform(prop.transform, {corner & 1 ? model.maxs.x : model.mins.x, corner & 2 ? model.maxs.y : model.mins.y,
                corner & 4 ? model.maxs.z : model.mins.z});
            low = {std::min(low.x, p.x - kFixtureMargin), std::min(low.y, p.y - kFixtureMargin), std::min(low.z, p.z - kFixtureMargin)};
            high = {std::max(high.x, p.x + kFixtureMargin), std::max(high.y, p.y + kFixtureMargin), std::max(high.z, p.z + kFixtureMargin)};
        }
        nearby.clear();
        for (uint32_t i = 0; i < world.lights.size(); ++i) {
            if (!model.bounded || bsp::InBounds(world.lights[i].origin, low, high))
                nearby.push_back(i);
        }
        if (nearby.empty())
            continue;
        if (!model.hullsRead) {
            model.hullsRead = true;
            model.hulls = VradPropHulls(path, model.fromCollision);
        }
        for (const std::vector<bsp::Float3>& piece : model.hulls) {
            if (piece.empty())
                continue;
            std::vector<bsp::Float3> points(piece.size());
            std::transform(piece.begin(), piece.end(), points.begin(), [&](const bsp::Float3& p) { return transform(prop.transform, p); });
            std::vector<hull::Plane> planes = hull::Build(points);
            if (planes.empty()) {
                if (++failedHulls <= 4)
                    failedModels += (failedModels.empty() ? "" : ", ") + path;
                continue;
            }
            for (uint32_t light : nearby) {
                if (hull::Distance(planes, world.lights[light].origin) <= kFixtureMargin) {
                    result.push_back({light, planes});
                    collisionHulls += model.fromCollision ? 1 : 0;
                }
            }
        }
    }
    std::vector<uint32_t> lights;
    for (const LightHull& entry : result)
        lights.push_back(entry.light);
    std::sort(lights.begin(), lights.end());
    log::Info("Scene: {} lights sit in or against {} static prop hulls ({} from collision models), which block them as in VRAD ({} flat or "
              "failed hulls{}{})",
        std::unique(lights.begin(), lights.end()) - lights.begin(), result.size(), collisionHulls, failedHulls, failedModels.empty() ? "" : ": ",
        failedModels);
    return result;
}

// Static props as VRAD shadowed them when the map was compiled without -StaticPropPolys (AddPolysForRayTrace in
// src/utils/vrad/vradstaticprops.cpp): the convex hulls of VradPropHulls. Props marked "disable shadows" are left out.
void AddVradOccluders(const bsp::WorldGeometry& world, std::vector<SceneInstance>& instances)
{
    constexpr int kUnread = -2;
    std::vector<int> collisionModels(world.propModels.size(), kUnread);
    std::vector<bool> fromCollision(world.propModels.size(), false);
    const auto addModel = [](const std::vector<bsp::Float3>& triangles) {
        std::vector<uint32_t> indices(triangles.size());
        std::iota(indices.begin(), indices.end(), 0u);
        const std::vector<uint32_t> albedo(triangles.size() / 3, PackAlbedo({0.5f, 0.5f, 0.5f}));
        return g_state.scene.AddModel({triangles, indices, albedo, static_cast<UINT>(albedo.size()), {}, {}, {}});
    };
    size_t collisionProps = 0;
    size_t meshProps = 0;
    for (const bsp::StaticProp& prop : world.props) {
        if (prop.noShadow)
            continue;
        const std::string& path = world.propModels[prop.model];
        int& collision = collisionModels[prop.model];
        if (collision == kUnread) {
            collision = -1;
            bool collides = false;
            std::vector<bsp::Float3> triangles;
            for (const std::vector<bsp::Float3>& piece : VradPropHulls(path, collides)) {
                const std::vector<bsp::Float3> hull = hull::Triangles(piece);
                triangles.insert(triangles.end(), hull.begin(), hull.end());
            }
            fromCollision[prop.model] = collides;
            if (!triangles.empty())
                collision = addModel(triangles);
        }
        if (collision < 0)
            continue;
        SceneInstance instance;
        instance.mask = kVradOccluderMask;
        instance.model = collision;
        std::memcpy(instance.transform, prop.transform, sizeof(instance.transform));
        instances.push_back(instance);
        ++(fromCollision[prop.model] ? collisionProps : meshProps);
    }
    const float identity[3][4] = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
    for (const auto& [triangles, mask] : {std::pair(&world.brushOccluders, kVradOccluderMask), std::pair(&world.skyOccluders, kVradSkyMask)}) {
        if (triangles->empty())
            continue;
        SceneInstance instance;
        instance.model = addModel(*triangles);
        instance.mask = mask;
        std::memcpy(instance.transform, identity, sizeof(instance.transform));
        if (instance.model >= 0)
            instances.push_back(instance);
    }
    log::Info("Scene: VRAD's shadows for the bake: {} props by collision model, {} by mesh hulls; {} brush triangles, {} sky triangles", collisionProps,
        meshProps, world.brushOccluders.size() / 3, world.skyOccluders.size() / 3);
}

bool IsFirstPersonModel(const std::string& path)
{
    return path.starts_with("models/weapons/v_") || path.starts_with("models/weapons/c_");
}

// Entities of the 3D skybox's scaled-down scene (skybox brush entities, animated props) would shadow the map
// from far away, so they are traced like the skybox's static geometry.
bool InSkybox(const entities::ModelEntity& entity, const bsp::Float3& center)
{
    const auto& m = entity.transform;
    return g_state.skyboxTest.InSkybox({m[0][0] * center.x + m[0][1] * center.y + m[0][2] * center.z + m[0][3],
        m[1][0] * center.x + m[1][1] * center.y + m[1][2] * center.z + m[1][3], m[2][0] * center.x + m[2][1] * center.y + m[2][2] * center.z + m[2][3]});
}

// Identifies a map by its contents, for the baked residual cache.
uint64_t FileHash(std::span<const std::byte> data)
{
    uint64_t hash = 14695981039346656037ull ^ data.size();
    size_t i = 0;
    for (; i + 8 <= data.size(); i += 8) {
        uint64_t word;
        std::memcpy(&word, data.data() + i, sizeof(word));
        hash = (hash ^ word) * 1099511628211ull;
        hash ^= hash >> 29;
    }
    for (; i < data.size(); ++i)
        hash = (hash ^ static_cast<uint8_t>(data[i])) * 1099511628211ull;
    return hash;
}

void ClearScene()
{
    g_state.lighting->WaitForGpu();
    g_state.lighting->Skinned().Clear();
    g_state.lighting->Sway().Clear();
    g_state.lighting->InvalidateHistory();
    g_state.scene.Clear();
    light_queries::Clear();
    g_state.models.clear();
    g_state.brushModels.clear();
    g_state.skyboxTest = {};
    g_state.mapMaterials.clear();
    g_state.textures.clear();
    // Scene material indices and texture views are reset with the scene.
    g_state.materials.clear();
    g_state.swayMaterials.clear();
    g_state.ropeModels.clear();
    g_state.loggedRopes = false;
}

void UpdateScene()
{
    if (!engine::Ready())
        return;
    if (!g_state.sceneInitialized) {
        g_state.sceneInitialized = g_state.scene.Initialize(g_state.device, g_state.queue, *g_state.descriptors, g_state.opacityMicromaps);
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
    // With real-time lighting off the game lights itself; the map's scene loads once it is turned on.
    if (!GetSettings().realtimeLighting) {
        if (g_state.scene.Ready()) {
            ClearScene();
            log::Info("Scene cleared");
        }
        return;
    }

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
        if (g_state.scene.LoadMap(level, world, FixtureHulls(world), worldGeometry)) {
            g_state.brushModels = std::move(world.brushModels);
            g_state.skyboxTest.visibility.nodes = std::move(world.visibility.nodes);
            g_state.skyboxTest.visibility.leaves = std::move(world.visibility.leaves);
            g_state.skyboxTest.skyArea = world.skyArea;
            g_state.skyboxTest.skyMins = world.skyMins;
            g_state.skyboxTest.skyMaxs = world.skyMaxs;
            std::vector<SceneInstance> instances;
            std::vector<SwayingProp> swayingProps;
            const auto addProps = [&](const std::vector<bsp::StaticProp>& props, UINT mask) {
                for (const bsp::StaticProp& prop : props) {
                    const ModelEntry& model = ModelFor(world.propModels[prop.model]);
                    SceneInstance instance;
                    instance.model = model.sceneModel;
                    instance.mask = mask == 0 && prop.noShadow ? kShadowlessPropMask : mask;
                    instance.flags = prop.ignoreNormals ? kInstanceIgnoreNormals : 0;
                    std::memcpy(instance.transform, prop.transform, sizeof(instance.transform));
                    // The 3D skybox's scaled-down props sway too little to matter.
                    if (model.swaying && mask != kSkyboxMask)
                        swayingProps.push_back({model.swaying, instance});
                    else
                        instances.push_back(instance);
                }
            };
            addProps(world.props, 0);
            const size_t worldProps = swayingProps.size() +
                std::count_if(instances.begin(), instances.end(), [](const SceneInstance& instance) { return instance.model >= 0; });
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
            // A residual baked before for this map, by the map's contents (gmodpp_cache\baked).
            const std::filesystem::path cachePath = GetOptions().gameDirectory / L"gmodpp_cache" / L"baked" / std::format(L"{:016x}.bin", FileHash(file));
            std::vector<std::byte> cached;
            std::error_code sizeError;
            const uintmax_t cacheSize = std::filesystem::file_size(cachePath, sizeError);
            if (std::ifstream cache(cachePath, std::ios::binary); cache && !sizeError && cacheSize > sizeof(uint32_t)) {
                uint32_t version = 0;
                cache.read(reinterpret_cast<char*>(&version), sizeof(version));
                cached.resize(static_cast<size_t>(cacheSize) - sizeof(version));
                cache.read(reinterpret_cast<char*>(cached.data()), static_cast<std::streamsize>(cached.size()));
                if (!cache || version != kBakeVersion)
                    cached.clear();
            }
            if (cached.empty() && !world.baked.samples.empty())
                AddVradOccluders(world, instances);
            log::Info("Scene '{}': {} faces, {} displacements, {} skybox faces and {} skybox props, {} world triangles ({} alpha-tested), {} of {} static props, {} lights; "
                      "sun {} dir ({:.2f} {:.2f} {:.2f}) color ({:.2f} {:.2f} {:.2f}), sky ({:.2f} {:.2f} {:.2f})",
                level, world.faceCount, world.displacementCount, world.skyboxFaceCount, world.skyboxProps.size(), world.mesh.indices.size() / 3, worldTriangles.alphaTriangles.size(),
                worldProps,
                world.props.size(), world.lights.size(), world.sun.present ? "present" : "missing", world.sun.direction.x, world.sun.direction.y,
                world.sun.direction.z, world.sun.color.x, world.sun.color.y, world.sun.color.z, world.sun.ambient.x, world.sun.ambient.y,
                world.sun.ambient.z);
            if (!g_state.lighting->Sway().SetProps(g_state.scene, g_state.swayMaterials, swayingProps)) {
                for (const SwayingProp& prop : swayingProps)
                    instances.push_back(prop.instance);
            }
            g_state.scene.SetStaticInstances(std::move(instances));
            // The world's triangles in their BLAS order (PrepareMesh), with their faces' lightmaps.
            std::vector<BakedTriangle> bakedTriangles(world.mesh.triangleFaces.size());
            for (size_t t = 0; t < bakedTriangles.size(); ++t) {
                bakedTriangles[t].face = world.mesh.triangleFaces[t];
                for (size_t corner = 0; corner < 3; ++corner) {
                    const bsp::Float2& luxel = world.mesh.luxels[world.mesh.indices[t * 3 + corner]];
                    bakedTriangles[t].luxels[corner][0] = luxel.u;
                    bakedTriangles[t].luxels[corner][1] = luxel.v;
                }
            }
            if (g_state.scene.LoadBaked(world.baked, bakedTriangles, cached) && !g_state.scene.Baked().Ready())
                g_state.scene.Baked().cachePath = cachePath;
            if (g_state.opacityMicromaps) {
                const auto [triangles, micromaps] = g_state.scene.MicromapCounts();
                log::Info("Scene: {} alpha-tested or translucent triangles have opacity micromaps ({} micromaps)", triangles, micromaps);
            }
            return;
        }
        error = "building acceleration structures failed";
    }
    log::Info("Scene: cannot load '{}': {}", level, error);
    g_state.failedMap = level;
}

// The rope prism with a rope material's albedo, or -1 for clear materials (additive beams and lasers), which cast no
// shadow.
int RopeModelFor(const std::string& material)
{
    if (auto cached = g_state.ropeModels.find(material); cached != g_state.ropeModels.end())
        return cached->second;
    std::string path = material;
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!path.starts_with("materials/"))
        path = "materials/" + path;
    if (!path.ends_with(".vmt"))
        path += ".vmt";
    const std::optional<MaterialInfo>& info = MaterialFor(path);
    int& model = g_state.ropeModels[material];
    model = -1;
    if (info && info->clear)
        return model;
    const ropes::Prism prism = ropes::UnitPrism();
    const std::vector<uint32_t> albedo(prism.indices.size() / 3, PackAlbedo(info ? info->reflectivity : kDefaultReflectivity));
    model = g_state.scene.AddModel({prism.positions, prism.indices, albedo, static_cast<UINT>(albedo.size()), {}, {}, {}});
    return model;
}

// Never an entity index, which stays below 2^16.
int64_t RopeSegmentKey(int rope, size_t segment) { return static_cast<int64_t>(rope) << 16 | static_cast<int64_t>(segment + 1); }

// Keeps a dynamic instance's transform for the next main view and gives it the last one.
void TrackMotion(SceneInstance& instance, int64_t key)
{
    auto& current = g_state.currentTransforms[key];
    std::memcpy(current.data(), instance.transform, sizeof(current));
    if (auto previous = g_state.previousTransforms.find(key); previous != g_state.previousTransforms.end()) {
        std::memcpy(instance.previousTransform, previous->second.data(), sizeof(instance.previousTransform));
        instance.previousKnown = true;
    }
}

// Ropes are drawn from their simulated nodes; each segment of a rope's curve is an instance of its prism.
void CollectRopes()
{
    entities::CollectRopes(g_state.ropes);
    for (const entities::Rope& rope : g_state.ropes) {
        const int model = RopeModelFor(rope.material);
        if (model < 0)
            continue;
        const std::vector<bsp::Float3> curve = ropes::Curve(rope);
        const UINT mask = g_state.skyboxTest.InSkybox(curve.front()) ? kSkyboxMask : kDynamicMask;
        for (size_t s = 0; s + 1 < curve.size(); ++s) {
            SceneInstance instance;
            instance.model = model;
            instance.mask = mask;
            if (!ropes::SegmentTransform(curve[s], curve[s + 1], rope.width * 0.5f, instance.transform))
                continue;
            TrackMotion(instance, RopeSegmentKey(rope.index, s));
            g_state.dynamicInstances.push_back(instance);
        }
    }
    if (!g_state.ropes.empty() && !g_state.loggedRopes) {
        g_state.loggedRopes = true;
        log::Info("Scene: {} ropes cast shadows", g_state.ropes.size());
    }
}

// In first person the local player's weapons show their view models (c_ or v_), which are rigged to the camera, so the
// held one's world model is placed in the player's hand as Source merges it onto the holder's hand bone.
void AddHeldWeapon(const entities::ModelEntity& weapon, const studio::Matrix3x4& hand)
{
    const std::string worldModel = entities::WeaponWorldModel(weapon);
    if (worldModel.empty())
        return;
    const ModelEntry& model = ModelFor(worldModel);
    if (model.sceneModel < 0 || !model.rightHandPoseToBone)
        return;
    const studio::Matrix3x4 transform = studio::Multiply(hand, *model.rightHandPoseToBone);
    SceneInstance& instance = g_state.dynamicInstances.emplace_back();
    instance.model = model.sceneModel;
    instance.mask = kLocalPlayerMask;
    std::memcpy(instance.transform, transform.data(), sizeof(instance.transform));
    TrackMotion(instance, weapon.index);
}

void CollectEntities()
{
    g_state.dynamicInstances.clear();
    g_state.skinnedEntities.clear();
    g_state.currentTransforms.clear();
    entities::Collect(g_state.entities);
    const int localPlayer = engine::LocalPlayerIndex();
    const auto player = std::find_if(g_state.entities.begin(), g_state.entities.end(), [&](const entities::ModelEntity& e) { return e.index == localPlayer; });
    const void* playerRenderable = player != g_state.entities.end() ? player->renderable : nullptr;
    g_state.localPlayer.reset();
    if (player != g_state.entities.end())
        g_state.localPlayer = std::array<float, 3>{player->transform[0][3], player->transform[1][3], player->transform[2][3]};
    std::optional<studio::Matrix3x4> playerHand;
    if (player != g_state.entities.end()) {
        const ModelEntry& body = ModelFor(player->model);
        g_state.bones.resize(kMaxBones);
        if (body.rightHand >= 0 && body.rightHand < kMaxBones &&
            entities::SetupBones(player->renderable, reinterpret_cast<float (*)[3][4]>(g_state.bones.data()), kMaxBones))
            playerHand = g_state.bones[body.rightHand];
    }
    for (const entities::ModelEntity& entity : g_state.entities) {
        // What the local player holds goes with its body, so the viewmodel's rays never meet its weapon.
        const bool carried = entity.index == localPlayer || (entity.parent != nullptr && entity.parent == playerRenderable);
        if (IsFirstPersonModel(entity.model)) {
            if (carried && playerHand)
                AddHeldWeapon(entity, *playerHand);
            continue;
        }
        const ModelEntry& model = ModelFor(entity.model);
        if (model.sceneModel < 0)
            continue;
        const UINT mask = carried ? kLocalPlayerMask : InSkybox(entity, model.center) ? kSkyboxMask : kDynamicMask;
        if (model.skinned) {
            g_state.bones.resize(kMaxBones);
            if (!entities::SetupBones(entity.renderable, reinterpret_cast<float (*)[3][4]>(g_state.bones.data()), kMaxBones))
                continue;
            g_state.bones.resize(std::min<size_t>(model.skinned->poseToBone.size(), kMaxBones));
            g_state.skinnedEntities.push_back({entity.index, model.sceneModel, model.skinned, g_state.bones, mask});
            continue;
        }
        studio::Matrix3x4 transform;
        std::memcpy(transform.data(), entity.transform, sizeof(transform));
        if (model.rootPoseToBone) {
            g_state.bones.resize(kMaxBones);
            if (!entities::SetupBones(entity.renderable, reinterpret_cast<float (*)[3][4]>(g_state.bones.data()), kMaxBones))
                continue;
            transform = studio::Multiply(g_state.bones[0], *model.rootPoseToBone);
        }
        SceneInstance& instance = g_state.dynamicInstances.emplace_back();
        instance.model = model.sceneModel;
        instance.mask = mask;
        std::memcpy(instance.transform, transform.data(), sizeof(instance.transform));
        TrackMotion(instance, entity.index);
    }
    CollectRopes();
    std::swap(g_state.previousTransforms, g_state.currentTransforms);
}

std::optional<std::array<float, 3>> EyePosition(const gbuffer::Matrix4& m)
{
    // The eye is where clip x, y and w are all zero: solve rows 0, 1 and 3 with Cramer's rule.
    const auto& a = m[0];
    const auto& b = m[1];
    const auto& c = m[3];
    auto det = [](float a0, float a1, float a2, float b0, float b1, float b2, float c0, float c1, float c2) {
        return a0 * (b1 * c2 - b2 * c1) - a1 * (b0 * c2 - b2 * c0) + a2 * (b0 * c1 - b1 * c0);
    };
    const float d = det(a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]);
    if (std::fabs(d) < 1e-12f)
        return std::nullopt;
    return std::array<float, 3>{det(-a[3], a[1], a[2], -b[3], b[1], b[2], -c[3], c[1], c[2]) / d,
        det(a[0], -a[3], a[2], b[0], -b[3], b[2], c[0], -c[3], c[2]) / d, det(a[0], a[1], -a[3], b[0], b[1], -b[3], c[0], c[1], -c[3]) / d};
}


}

bool Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue, DescriptorHeap& descriptors, bool opacityMicromaps,
    const std::filesystem::path& dlssDirectory)
{
    if (g_state.lighting)
        return true;
    auto lighting = std::make_unique<Lighting>();
    if (!lighting->Initialize(device, queue, descriptors, opacityMicromaps, dlssDirectory))
        return false;
    g_state.device = device;
    g_state.queue = queue;
    g_state.descriptors = &descriptors;
    g_state.opacityMicromaps = opacityMicromaps;
    g_state.lighting = std::move(lighting);
    return true;
}

void Shutdown()
{
    if (!g_state.lighting)
        return;
    ClearScene();
    g_state.lighting.reset();
}

bool Ready() { return g_state.lighting && g_state.scene.Ready(); }

void WaitForGpu()
{
    if (g_state.lighting)
        g_state.lighting->WaitForGpu();
}

void OnPresent(UINT width, UINT height, bool consoleRequests)
{
    ++g_state.frame;
    g_state.width = width;
    g_state.height = height;
    client::TryInstall();
    engine::TryInitialize();
    light_queries::TryInstall();
    entities::TryInitialize();
    if (g_state.frame % kMapPollInterval == 0)
        SyncSettingsFile(GetOptions().gameDirectory / L"gmodpp_settings.ini");
    if (g_state.lighting && g_state.frame % kMapPollInterval == 0) {
        UpdateScene();
        RunConsoleRequests(consoleRequests);
    }
}

std::array<float, 2> ViewJitter(UINT width, UINT height)
{
    // A Halton(2, 3) sub-pixel pattern, in NDC units (y is up in NDC).
    g_state.pixelOffset[0] = g_state.pixelOffset[1] = 0.0f;
    UINT renderWidth = 0;
    UINT renderHeight = 0;
    if (g_state.lighting && g_state.lighting->RayReconstructionSize(width, height, renderWidth, renderHeight)) {
        // Ray Reconstruction wants many phases, more as the upscale grows (DLSS programming guide 3.7.1).
        const float ratio = static_cast<float>(width) / static_cast<float>(renderWidth);
        const uint32_t phases = std::max(kRayReconstructionPhases, static_cast<uint32_t>(std::ceil(8.0f * ratio * ratio)));
        const uint32_t sample = static_cast<uint32_t>(g_state.frame % phases) + 1;
        g_state.pixelOffset[0] = Halton(sample, 2) - 0.5f;
        g_state.pixelOffset[1] = Halton(sample, 3) - 0.5f;
        // DLSS takes the image shift itself, as NVIDIA's Donut samples pass it (PlanarView pixel offset).
        return {g_state.pixelOffset[0] * 2.0f / static_cast<float>(renderWidth), -g_state.pixelOffset[1] * 2.0f / static_cast<float>(renderHeight)};
    }
    if (GetSettings().antiAliasing == AntiAliasing::Temporal) {
        const uint32_t sample = static_cast<uint32_t>(g_state.frame % kJitterSamples) + 1;
        g_state.pixelOffset[0] = Halton(sample, 2) - 0.5f;
        g_state.pixelOffset[1] = Halton(sample, 3) - 0.5f;
        return {g_state.pixelOffset[0] * 2.0f / static_cast<float>(width), -g_state.pixelOffset[1] * 2.0f / static_cast<float>(height)};
    }
    return {};
}

void LightView(ViewInputs& inputs)
{
    switch (inputs.kind) {
    case gbuffer::ViewKind::Main:
        CollectEntities();
        engine::ActiveDynamicLights(g_state.dynamicLights);
        inputs.localPlayer = g_state.localPlayer;
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
    if (std::array<float, engine::kLightStyles> styles; engine::LightStyleValues(styles))
        g_state.scene.SetLightStyles(styles);
    inputs.frame = g_state.frame;
    inputs.jitterX = g_state.pixelOffset[0];
    inputs.jitterY = g_state.pixelOffset[1];
    const std::vector<SkinnedEntity>& skinned = inputs.kind == gbuffer::ViewKind::Viewmodel ? g_state.noSkinnedEntities : g_state.skinnedEntities;
    g_state.lighting->Run(inputs, g_state.scene, g_state.dynamicInstances, skinned);
    if (const std::optional<ProbeResult> probe = g_state.lighting->TakeProbe()) {
        std::string model = "none";
        for (const auto& [path, entry] : g_state.models) {
            if (entry.sceneModel == probe->model && probe->model >= 0)
                model = path;
        }
        if (probe->model == 0)
            model = "the world";
        log::Info("Probe: sun visibility {:.2f} at ({:.1f} {:.1f} {:.1f}); blocked by instance {} (model {} '{}', mask {:#x}), triangle {}, {:.1f} units away",
            probe->sunVisibility, probe->origin[0], probe->origin[1], probe->origin[2], static_cast<int>(probe->instance), probe->model, model, probe->mask,
            probe->triangle, probe->distance);
    }
}

std::optional<gbuffer::ForwardConstants> ForwardLighting(const ViewInputs& inputs)
{
    if (inputs.kind != gbuffer::ViewKind::Main || !g_state.lighting->VolumeReady())
        return std::nullopt;
    gbuffer::ForwardConstants forward = {};
    if (const std::optional<std::array<float, 3>> eye = EyePosition(inputs.viewProj))
        forward.eye = *eye;
    forward.tonemapScale = inputs.tonemapScale;
    const bsp::Float3& ambient = g_state.scene.Sun().ambient;
    const float sky = GetSettings().skyIntensity * g_state.scene.LightStyle(g_state.scene.Sun().ambientStyle);
    forward.fallback = {ambient.x * sky, ambient.y * sky, ambient.z * sky};
    forward.probeOrigin = g_state.lighting->VolumeOrigin();
    if (GetSettings().fog)
        forward.fog = inputs.fog;
    forward.lightOnly = GetSettings().debugView != DebugView::Final;
    forward.twinFrame = g_state.lighting->ForwardFrame();
    forward.twinScene = g_state.lighting->ForwardScene();
    return forward;
}

ID3D12Fence* Fence() { return g_state.lighting->Fence(); }

uint64_t FenceValue() { return g_state.lighting->FenceValue(); }

}
