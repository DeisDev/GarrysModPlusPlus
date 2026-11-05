#include "world.h"

#include "bsp.h"
#include "client_hooks.h"
#include "command_line.h"
#include "engine_interfaces.h"
#include "effect_lights.h"
#include "entities.h"
#include "glow_patches.h"
#include "light_queries.h"
#include "loading_screen.h"
#include "log.h"
#include "options.h"
#include "ropes.h"
#include "scene.h"
#include "settings.h"
#include "skinned_casters.h"
#include "studio_model.h"
#include "swaying_props.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <execution>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
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
// Humanoid characters' skeletons have this bone, wherever the model lives (HL2's NPCs are in models/ itself).
constexpr char kHeadBone[] = "ValveBiped.Bip01_Head1";
constexpr bsp::Float3 kDefaultReflectivity = {0.5f, 0.5f, 0.5f};
constexpr float kDefaultAlphaReference = 0.5f;
// Alpha and mask textures are read from a mip no larger than this; shadows and reflections need no more detail.
constexpr uint32_t kSceneTextureSize = 256;
// Base textures and blend masks, which reflected and refracted hits show (TexturedAlbedo in shaders/scene.hlsli), are
// read whole: through water up close, the 256-texel mip showed as blur and blocks beside the game's own texture.
constexpr uint32_t kFullTextureSize = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
// Never PackAlpha values (they always have a texture view below 0xFFFFFF) nor PackTranslucent ones (bit 31 set).
// 0xFFFFFFFF is PackTranslucent's untextured glass at full opacity (Refract without a tint texture).
constexpr uint32_t kOpaqueTriangle = 0x00FFFFFF;
constexpr uint32_t kDroppedTriangle = 0x7FFFFFFF;

constexpr uint32_t kJitterSamples = 8;

// Self-illumination masks are read on the CPU at this size to find where models glow; a patch's light needs no more.
constexpr uint32_t kGlowMaskSize = 64;
// Triangles whose mask covers less than this share of them glow too faintly to light anything.
constexpr float kGlowCoverage = 0.05f;
// How many of the view's glowing patches are lit exactly (the rest are sampled with the other lights).
constexpr size_t kExactGlowLights = 4;
// Dynamic light keys (FrameLight::key) carry what made the light in their top bits (engine and particle lights':
// src/effect_lights.cpp).
constexpr uint64_t kGlowLightKey = 2ull << 62;

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
    // A $staticprop model's bone is its entity's unscaled transform (C_BaseAnimating::SetupBones, client.dll+0x1a4e80):
    // GMod scales that bone by the model scale only as it draws it (InternalDrawModel, 0x1a0270), for single-bone
    // models that are $staticprop or set Entity:SetLegacyTransform (REA 2026-11-03).
    bool staticProp = false;
    // Its kRightHandBone, if it has one: a holder's hand, or where a weapon merged onto that hand is held.
    int rightHand = -1;
    std::optional<studio::Matrix3x4> rightHandPoseToBone;
    // Its glowing patches, which light the scene from its moving instances (FrameLight::Kind::Glow), and for each texture
    // (glow::Patch::material) its material's name and the $selfillumtint the patches were made with, which proxies may
    // change as the game draws (ModelEntry::glowPatches' tinted ones follow it).
    std::shared_ptr<const std::vector<glow::Patch>> glowPatches;
    struct GlowMaterial {
        std::string name;
        bsp::Float3 tint;
    };
    std::vector<GlowMaterial> glowMaterials;
};

struct MaterialInfo {
    // Translucent surfaces (glass, foliage) let part of the light through, coloured by their texture; alpha-tested ones
    // (foliage, fences) are cut by their alpha. Refract glass passes light tinted by $refracttint and its
    // $refracttinttexture. Clear ones block nothing: additive light effects (beams, glows) only add light.
    bool translucent = false;
    bool clear = false;
    bool refract = false;
    // Glass: Refract materials, and translucent ones that reflect ($envmap). Light through it loses what its two faces
    // reflect (Scene::kTintGlass).
    bool glass = false;
    // $refracttint in linear light, as the Refract shader sets it (SetPixelShaderConstantGammaToLinear, c1; confirmed in
    // GMod's stdshader_dx9.dll at 0x180017210, called with the refract tint from 0x180044070, whose $REFRACTTINT
    // defaults to [1 1 1] in the parameter table at 0x1800be7c0), and its tint texture (sampler 5, read as sRGB).
    bsp::Float3 refractTint = {1.0f, 1.0f, 1.0f};
    std::string refractTintTexture;
    bool alphaTest = false;
    float alphaReference = kDefaultAlphaReference;
    // $alpha.
    float opacity = 1.0f;
    std::string baseTexture;
    bsp::Float3 reflectivity = kDefaultReflectivity;
    // The material's name, as the material system knows it ("models/weapons/w_physics").
    std::string name;
    // $selfillum: its mask's texture and channel (the base texture's alpha, or $selfillummask's red, as VertexLitGeneric
    // reads them) and $selfillumtint.
    bool selfIllum = false;
    std::string selfIllumMask;
    uint32_t selfIllumChannel = 0;
    bsp::Float3 selfIllumTint = {1.0f, 1.0f, 1.0f};
    // $selfillumtint as the VMT gives it (selfIllumTint may hold more: $selfillumfresnel's mean).
    bsp::Float3 selfIllumVmtTint = {1.0f, 1.0f, 1.0f};
    // VertexLitGeneric's $emissiveblend layer, an additive pass of $emissiveblendbasetexture times $emissiveblendtexture
    // (scrolled by a flow map) times $emissiveblendtint and $emissiveblendstrength (0-1).
    bool emissiveBlend = false;
    std::string emissiveBlendBase;
    std::string emissiveBlendTexture;
    bsp::Float3 emissiveBlendTint = {1.0f, 1.0f, 1.0f};
    float emissiveBlendStrength = 0.0f;
    // Reflections and self-illumination, for Scene::AddMaterial.
    uint32_t sceneMaterial = Scene::kNoMaterial;
    // Index of its $treesway parameters in State::swayMaterials.
    uint32_t swayMaterial = SwayingModel::kNoSway;
};

// What Scene::AddModel needs beyond positions, indices and albedo; alpha-tested triangles are moved last.
struct TriangleData {
    UINT opaqueTriangles = 0;
    std::vector<uint32_t> alphaTriangles;
    // TriangleTintFor of each of alphaTriangles.
    std::vector<uint32_t> alphaTints;
    // Scene material of each triangle.
    std::vector<uint32_t> materials;
};

// A self-illuminated triangle of a model, in the model's space: its corners, its index in the model's BLAS order and
// the luminance of its emission (albedo times $selfillumtint; its mask is read where it is sampled).
struct ModelEmitter {
    bsp::Float3 corners[3];
    uint32_t primitive;
    float emission;
};

struct SceneTexture {
    UINT view = DescriptorHeap::kInvalid;
    bool alpha = false;
};

// One channel of a self-illumination mask at kGlowMaskSize, as the GPU reads it; empty when it cannot be read.
struct MaskImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> values;
};

// A glowing patch of a moving model placed for the main view (glow::Place): its light, how much it matters to the view
// (its power over its squared distance from the eye), and its instance: an index in State::dynamicInstances (Rigid) or
// State::skinnedEntities (Skinned), or none (the view model, which is not in the scene).
struct GlowCandidate {
    enum class Owner { Rigid, Skinned, None };
    FrameLight light;
    float score = 0.0f;
    size_t owner = 0;
    Owner kind = Owner::None;
};

struct State {
    ID3D12Device5* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    DescriptorHeap* descriptors = nullptr;
    bool opacityMicromaps = false;
    uint64_t frame = 0;
    // The main view's size, whose centre probe.request reports by default.
    UINT width = 0;
    UINT height = 0;
    std::unique_ptr<Lighting> lighting;
    Scene scene;
    bool sceneInitialized = false;
    // PrepareLevel is running; the frames it presents must not load the scene again.
    bool preparing = false;
    // Loads the scene of the map being joined while the game loads it (BeginLevelLoad); only it touches the scene
    // until it is joined.
    std::jthread loader;
    std::atomic<float> loaderDone = 0.0f;
    std::atomic<bool> loaderFinished = false;
    std::chrono::steady_clock::time_point loaderStart;
    std::chrono::steady_clock::time_point loaderEnd;
    std::string failedMap;
    std::unordered_map<std::string, ModelEntry> models;
    std::unordered_map<std::string, std::optional<MaterialInfo>> materials;
    std::vector<SwayMaterial> swayMaterials;
    // Textures uploaded for alpha tests and material masks, by VTF path.
    std::unordered_map<std::string, SceneTexture> textures;
    // Textures at full size (kFullTextureSize), by path.
    std::unordered_map<std::string, UINT> fullTextures;
    std::vector<std::string> mapMaterials;
    // bsp::WorldGeometry::lightMaterials of the map.
    std::vector<bool> lightMaterials;
    // A copy of each scene material (AddSceneMaterial), and the light-texture copy of self-illuminated ones.
    std::vector<GpuMaterial> sceneMaterials;
    std::unordered_map<uint32_t, uint32_t> lightTextureMaterials;
    // The self-illuminated triangles of each scene model (ModelEmitters), for the emitters of its static instances.
    std::unordered_map<int, std::vector<ModelEmitter>> modelEmitters;
    // Self-illumination masks read on the CPU for glowing patches (MaskImageFor), by texture and channel.
    std::unordered_map<std::string, MaskImage> maskImages;
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
    // The main view's glowing patches of moving models (CollectEntities), and its dynamic lights in Scene's runs
    // (Scene::SetDynamicLights).
    std::vector<GlowCandidate> glowCandidates;
    // The held weapon's world model in the player's hand (AddHeldWeapon), whose glow lights the scene unless the view
    // model's does.
    struct HeldGlow {
        const ModelEntry* model;
        studio::Matrix3x4 transform;
        int index;
        size_t instance;
    };
    std::optional<HeldGlow> heldWeaponGlow;
    // TintChange's reading of each material's $selfillumtint, and the frame it was read in.
    std::unordered_map<std::string, std::pair<uint64_t, std::array<float, 3>>> tintChanges;
    std::vector<FrameLight> frameLights;
    // Where the map's point and spot lights are, whose halo sprites effect lights may leave out.
    std::vector<bsp::Float3> mapLightOrigins;
    size_t frameExactFirst = 0;
    size_t frameFlashFirst = 0;
    // The local player's origin from the last main view's entities.
    std::optional<std::array<float, 3>> localPlayer;
    // env_sun as the last 3D skybox or main view found it, for every view's sunlight.
    std::optional<gbuffer::EnvSun> envSun;
    std::vector<bsp::Mesh> brushModels;
    // Brush entity models with water (func_water_analog) by model number: their surfaces' scene model and body of water.
    struct BrushWaterModel {
        int sceneModel = -1;
        uint32_t body = 0;
    };
    std::unordered_map<uint32_t, BrushWaterModel> brushWater;
    std::vector<BrushWaterPlacement> brushWaterPlacements;
    bool loggedTurnedWater = false;
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
    const std::filesystem::path probe = ModDirectory() / L"probe.request";
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
    const std::filesystem::path path = ModDirectory() / L"console.txt";
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

UINT AddSceneTexture(const studio::TextureImage& image)
{
    constexpr DXGI_FORMAT kFormats[] = {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_R8_UNORM};
    return g_state.scene.AddTexture(kFormats[static_cast<int>(image.format)], image.width, image.height, image.mips);
}

SceneTexture LoadSceneTexture(const std::string& vtfPath, uint32_t maxSize)
{
    SceneTexture texture;
    const std::vector<std::byte> file = ReadOptional(vtfPath);
    std::string error;
    const std::optional<studio::TextureImage> image = studio::ReadTexture(file, maxSize, error);
    if (image) {
        texture.view = AddSceneTexture(*image);
        texture.alpha = image->alpha;
        if (texture.view == DescriptorHeap::kInvalid)
            error = "creating the texture failed";
    }
    if (texture.view == DescriptorHeap::kInvalid && ++g_state.modelErrors <= 10)
        log::Info("Scene: texture '{}' unusable ({}); alpha tests, masks and reflected detail that need it are skipped", vtfPath,
            file.empty() ? "file not found" : error);
    return texture;
}

const SceneTexture& TextureFor(const std::string& vtfPath)
{
    const auto [entry, added] = g_state.textures.try_emplace(vtfPath);
    if (added)
        entry->second = LoadSceneTexture(vtfPath, kSceneTextureSize);
    return entry->second;
}

UINT FullTextureFor(const std::string& vtfPath)
{
    const auto [entry, added] = g_state.fullTextures.try_emplace(vtfPath, DescriptorHeap::kInvalid);
    if (added)
        entry->second = LoadSceneTexture(vtfPath, kFullTextureSize).view;
    return entry->second;
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

// Adds a material to the scene, keeping a copy for emitters and light-texture copies.
uint32_t AddSceneMaterial(const GpuMaterial& material)
{
    const uint32_t index = g_state.scene.AddMaterial(material);
    if (index != Scene::kNoMaterial) {
        if (g_state.sceneMaterials.size() <= index)
            g_state.sceneMaterials.resize(index + 1);
        g_state.sceneMaterials[index] = material;
    }
    return index;
}

// The scene material of a face whose texture VRAD made emit_surface lights of (bsp::WorldGeometry::lightMaterials):
// a self-illuminated one is copied with Scene::kMaterialLightTexture, so its glow lights nothing a second time.
uint32_t LightTextureMaterial(uint32_t material)
{
    if (material >= g_state.sceneMaterials.size() || g_state.sceneMaterials[material].selfIllumMask == Scene::kMaskOff)
        return material;
    if (const auto found = g_state.lightTextureMaterials.find(material); found != g_state.lightTextureMaterials.end())
        return found->second;
    GpuMaterial copy = g_state.sceneMaterials[material];
    copy.flags |= Scene::kMaterialLightTexture;
    const uint32_t index = AddSceneMaterial(copy);
    g_state.lightTextureMaterials[material] = index;
    return index;
}

// A model's self-illuminated triangles that light the scene (not light textures), from its geometry in BLAS order.
std::vector<ModelEmitter> ModelEmitters(std::span<const bsp::Float3> positions, std::span<const uint32_t> indices,
    std::span<const uint32_t> triangleAlbedo, std::span<const uint32_t> triangleMaterials)
{
    std::vector<ModelEmitter> emitters;
    for (size_t t = 0; t < triangleMaterials.size() && t < triangleAlbedo.size(); ++t) {
        const uint32_t material = triangleMaterials[t];
        if (material >= g_state.sceneMaterials.size())
            continue;
        const GpuMaterial& gpu = g_state.sceneMaterials[material];
        if (gpu.selfIllumMask == Scene::kMaskOff || (gpu.flags & Scene::kMaterialLightTexture) != 0)
            continue;
        const uint32_t albedo = triangleAlbedo[t];
        const float rgb[3] = {static_cast<float>(albedo & 0xFF) / 255.0f, static_cast<float>((albedo >> 8) & 0xFF) / 255.0f,
            static_cast<float>((albedo >> 16) & 0xFF) / 255.0f};
        const float emission = 0.2126f * rgb[0] * gpu.selfIllumTint[0] + 0.7152f * rgb[1] * gpu.selfIllumTint[1] + 0.0722f * rgb[2] * gpu.selfIllumTint[2];
        if (emission <= 0.0f)
            continue;
        emitters.push_back({{positions[indices[t * 3]], positions[indices[t * 3 + 1]], positions[indices[t * 3 + 2]]}, static_cast<uint32_t>(t), emission});
    }
    return emitters;
}

// A self-illumination mask's channel (MaterialInfo::selfIllumMask), read once.
const MaskImage& MaskImageFor(const std::string& vtfPath, uint32_t channel)
{
    const auto [entry, added] = g_state.maskImages.try_emplace(std::format("{}#{}", vtfPath, channel));
    if (!added || vtfPath.empty())
        return entry->second;
    std::string error;
    const std::vector<std::byte> file = ReadOptional(vtfPath);
    if (const std::optional<studio::TextureImage> image = studio::ReadTexture(file, kGlowMaskSize, error)) {
        entry->second.values = studio::DecodeChannel(*image, 0, channel);
        entry->second.width = image->width;
        entry->second.height = image->height;
    }
    return entry->second;
}

// The mean of a mask over a triangle with these texture coordinates (0-1), from seven points spread over it; 0 for a
// mask that cannot be read, which glows nowhere on the GPU either.
float MaskCoverage(const MaskImage& mask, const bsp::Float2 (&uv)[3])
{
    if (mask.values.empty() || mask.width == 0 || mask.height == 0)
        return 0.0f;
    constexpr float kPoints[7][2] = {{1.0f / 3.0f, 1.0f / 3.0f}, {0.6f, 0.2f}, {0.2f, 0.6f}, {0.2f, 0.2f}, {0.1f, 0.45f}, {0.45f, 0.1f}, {0.45f, 0.45f}};
    float sum = 0.0f;
    for (const auto& point : kPoints) {
        const float b0 = 1.0f - point[0] - point[1];
        float u = uv[0].u * b0 + uv[1].u * point[0] + uv[2].u * point[1];
        float v = uv[0].v * b0 + uv[1].v * point[0] + uv[2].v * point[1];
        u -= std::floor(u);
        v -= std::floor(v);
        const uint32_t x = std::min(static_cast<uint32_t>(u * static_cast<float>(mask.width)), mask.width - 1);
        const uint32_t y = std::min(static_cast<uint32_t>(v * static_cast<float>(mask.height)), mask.height - 1);
        sum += static_cast<float>(mask.values[static_cast<size_t>(y) * mask.width + x]) / 255.0f;
    }
    return sum / static_cast<float>(std::size(kPoints));
}

float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// A texture's mean linear colour (read as sRGB) at kGlowMaskSize.
bsp::Float3 TextureMean(const std::string& vtfPath)
{
    float mean[3] = {};
    for (uint32_t c = 0; c < 3; ++c) {
        const MaskImage& image = MaskImageFor(vtfPath, c);
        double sum = 0.0;
        for (const uint8_t value : image.values)
            sum += SrgbToLinear(static_cast<float>(value) / 255.0f);
        mean[c] = image.values.empty() ? 0.0f : static_cast<float>(sum / static_cast<double>(image.values.size()));
    }
    return {mean[0], mean[1], mean[2]};
}

// A model's glowing patches (glow::BuildPatches, grouped by texture: glow::Patch::material) from its triangles in BLAS
// order. $selfillum glows as HitEmission has it, its albedo times $selfillumtint times its mask averaged over the
// triangle; those patches follow $selfillumtint as it changes (glow::Patch::tinted). An $emissiveblend layer glows by its
// base texture's colour over the triangle times the emissive texture's mean (its flow scrolls it over the model), its
// tint and strength.
std::shared_ptr<const std::vector<glow::Patch>> GlowPatches(const studio::Geometry& geometry, std::span<const uint32_t> triangleAlbedo,
    std::span<const MaterialInfo* const> textureInfo, bool skinned)
{
    const size_t triangles = std::min(geometry.triangleTextures.size(), triangleAlbedo.size());
    std::vector<bsp::Float3> selfIllum(triangles);
    std::vector<bsp::Float3> blend(triangles);
    std::vector<uint32_t> textures(triangles);
    bool glowing[2] = {};
    for (size_t t = 0; t < triangles && !geometry.uvs.empty(); ++t) {
        const uint16_t texture = geometry.triangleTextures[t];
        textures[t] = texture;
        const MaterialInfo* info = texture < textureInfo.size() ? textureInfo[texture] : nullptr;
        if (info == nullptr)
            continue;
        const bsp::Float2 uv[3] = {geometry.uvs[geometry.indices[t * 3]], geometry.uvs[geometry.indices[t * 3 + 1]], geometry.uvs[geometry.indices[t * 3 + 2]]};
        if (info->selfIllum) {
            const float coverage = MaskCoverage(MaskImageFor(info->selfIllumMask, info->selfIllumChannel), uv);
            if (coverage >= kGlowCoverage) {
                const uint32_t albedo = triangleAlbedo[t];
                selfIllum[t] = {static_cast<float>(albedo & 0xFF) / 255.0f * info->selfIllumTint.x * coverage,
                    static_cast<float>((albedo >> 8) & 0xFF) / 255.0f * info->selfIllumTint.y * coverage,
                    static_cast<float>((albedo >> 16) & 0xFF) / 255.0f * info->selfIllumTint.z * coverage};
                glowing[0] = true;
            }
        }
        if (info->emissiveBlend) {
            const bsp::Float3 emissive = TextureMean(info->emissiveBlendTexture);
            float base[3];
            for (uint32_t c = 0; c < 3; ++c)
                base[c] = SrgbToLinear(MaskCoverage(MaskImageFor(info->emissiveBlendBase, c), uv));
            const float strength = info->emissiveBlendStrength;
            blend[t] = {base[0] * emissive.x * info->emissiveBlendTint.x * strength, base[1] * emissive.y * info->emissiveBlendTint.y * strength,
                base[2] * emissive.z * info->emissiveBlendTint.z * strength};
            glowing[1] = glowing[1] || 0.2126f * blend[t].x + 0.7152f * blend[t].y + 0.0722f * blend[t].z > 0.0f;
        }
    }
    if (!glowing[0] && !glowing[1])
        return nullptr;
    const std::span<const studio::VertexWeights> weights = skinned ? std::span<const studio::VertexWeights>(geometry.weights) : std::span<const studio::VertexWeights>();
    std::vector<glow::Patch> patches;
    if (glowing[0]) {
        patches = glow::BuildPatches(geometry.positions, geometry.indices, selfIllum, textures, weights);
        for (glow::Patch& patch : patches)
            patch.tinted = true;
    }
    if (glowing[1]) {
        for (glow::Patch& patch : glow::BuildPatches(geometry.positions, geometry.indices, blend, textures, weights)) {
            patch.tinted = false;
            patches.push_back(patch);
        }
    }
    return patches.empty() ? nullptr : std::make_shared<const std::vector<glow::Patch>>(std::move(patches));
}

// The scene material for a VMT's env_cubemap reflection and self-illumination (as LightmappedGeneric and
// VertexLitGeneric compute their masks) and whether it is unlit, or Scene::kNoMaterial when it has none of them.
uint32_t SceneMaterial(std::string_view vmt, std::string_view shader, const MaterialInfo& info, bool unlit)
{
    const std::string& baseTexture = info.baseTexture;
    const bsp::Float3& reflectivity = info.reflectivity;
    auto enabled = [&](std::string_view key) {
        const std::optional<std::string> value = studio::MaterialValue(vmt, key);
        return value && !value->empty() && value->front() != '0';
    };
    const std::optional<std::string> envmap = studio::MaterialValue(vmt, "$envmap");
    const bool reflective = envmap && !envmap->empty();
    const bool selfIllum = info.selfIllum;
    const UINT baseView = baseTexture.empty() ? DescriptorHeap::kInvalid : FullTextureFor(baseTexture);
    if (!reflective && !selfIllum && !unlit && baseView == DescriptorHeap::kInvalid)
        return Scene::kNoMaterial;
    auto mask = [](const std::string& texture, uint32_t channel, bool invert) {
        const UINT view = texture.empty() ? DescriptorHeap::kInvalid : TextureFor(texture).view;
        return view != DescriptorHeap::kInvalid ? Scene::PackMask(view, channel, invert) : Scene::kMaskOff;
    };
    GpuMaterial material = {};
    material.flags = unlit ? Scene::kMaterialUnlit : 0u;
    material.baseTexture = baseView;
    material.reflectivity = 0.2126f * reflectivity.x + 0.7152f * reflectivity.y + 0.0722f * reflectivity.z;
    material.baseTexture2 = DescriptorHeap::kInvalid;
    material.blendModulate = DescriptorHeap::kInvalid;
    if (shader == "worldvertextransition" && baseView != DescriptorHeap::kInvalid) {
        const std::string baseTexture2 = studio::MaterialTexture(vmt, "$basetexture2");
        const std::string blendModulate = studio::MaterialTexture(vmt, "$blendmodulatetexture");
        material.baseTexture2 = baseTexture2.empty() ? DescriptorHeap::kInvalid : FullTextureFor(baseTexture2);
        material.blendModulate = blendModulate.empty() ? DescriptorHeap::kInvalid : FullTextureFor(blendModulate);
    }
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
        material.selfIllumMask = mask(info.selfIllumMask, info.selfIllumChannel, false);
        material.selfIllumTint[0] = info.selfIllumTint.x;
        material.selfIllumTint[1] = info.selfIllumTint.y;
        material.selfIllumTint[2] = info.selfIllumTint.z;
    }
    if (material.envmapMask == Scene::kMaskOff && material.selfIllumMask == Scene::kMaskOff && !unlit && baseView == DescriptorHeap::kInvalid)
        return Scene::kNoMaterial;
    return AddSceneMaterial(material);
}

// A VMT's text, with the material a "patch" material includes appended, and its shader's name in lower case; nothing when
// the file is missing.
std::optional<std::string> MaterialText(const std::string& vmtPath, std::string& shader)
{
    std::vector<std::byte> vmt = ReadOptional(vmtPath);
    if (vmt.empty())
        return std::nullopt;
    std::string text(reinterpret_cast<const char*>(vmt.data()), vmt.size());
    shader = ShaderName(text);
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
    return text;
}

// Source's GammaToLinear for colour constants up to 1 (src/mathlib/color_conversion.cpp): 1 from 0.95 up.
float GammaToLinear(float c) { return c > 1.0f ? c : c >= 0.95f ? 1.0f : std::pow(std::max(c, 0.0f), 2.2f); }
bsp::Float3 GammaToLinear(const bsp::Float3& c) { return {GammaToLinear(c.x), GammaToLinear(c.y), GammaToLinear(c.z)}; }

// The first of a texture's candidate VMTs that exists, with what the scene needs from it.
const std::optional<MaterialInfo>& MaterialFor(const std::string& vmtPath)
{
    auto cached = g_state.materials.find(vmtPath);
    if (cached != g_state.materials.end())
        return cached->second;
    std::optional<MaterialInfo>& entry = g_state.materials[vmtPath];
    std::string shader;
    const std::optional<std::string> materialText = MaterialText(vmtPath, shader);
    if (!materialText)
        return entry;
    const std::string& text = *materialText;
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
    entry->clear = enabled("$additive");
    if (shader == "refract") {
        entry->refract = true;
        entry->refractTint = GammaToLinear(MaterialColor(text, "$refracttint", {1.0f, 1.0f, 1.0f}));
        entry->refractTintTexture = studio::MaterialTexture(text, "$refracttinttexture");
    }
    entry->glass = entry->refract || (entry->translucent && !entry->clear && studio::MaterialValue(text, "$envmap").has_value());
    entry->alphaTest = enabled("$alphatest");
    if (std::optional<std::string> reference = studio::MaterialValue(text, "$alphatestreference")) {
        float value = 0.0f;
        if (std::from_chars(reference->data(), reference->data() + reference->size(), value).ec == std::errc() && value > 0.0f)
            entry->alphaReference = value;
    }
    entry->name = vmtPath.substr(vmtPath.starts_with("materials/") ? 10 : 0);
    if (entry->name.ends_with(".vmt"))
        entry->name.resize(entry->name.size() - 4);
    entry->baseTexture = studio::MaterialTexture(text, "$basetexture");
    bool baseAlpha = false;
    if (!entry->baseTexture.empty()) {
        std::vector<std::byte> header = ReadOptional(entry->baseTexture, studio::kVtfReflectivityHeaderSize);
        if (std::optional<bsp::Float3> reflectivity = studio::TextureReflectivity(header))
            entry->reflectivity = *reflectivity;
        baseAlpha = studio::TextureHasAlpha(header);
    }
    entry->selfIllum = enabled("$selfillum");
    if (entry->selfIllum) {
        const std::string selfIllumMask = studio::MaterialTexture(text, "$selfillummask");
        const std::string envmapMask = studio::MaterialTexture(text, "$envmapmask");
        // VertexLitGeneric's init (stdshader_dx9.dll+0x613f0) drops $selfillum when the base texture has no alpha and there
        // is neither $selfillumfresnel nor $selfillummask; without a bump map its draw (+0x59a00) takes the mask from
        // $envmapmask's alpha with $selfillum_envmapmask_alpha (SELFILLUM_ENVMAPMASK_ALPHA). $phong materials go to the
        // skin shader instead (+0x4d990), which was not checked. REA 2026-11-04.
        if (shader == "vertexlitgeneric" && !enabled("$phong")) {
            entry->selfIllum = baseAlpha || !selfIllumMask.empty() || enabled("$selfillumfresnel");
            if (entry->selfIllum && enabled("$selfillum_envmapmask_alpha") && !envmapMask.empty() && studio::MaterialTexture(text, "$bumpmap").empty()) {
                entry->selfIllumMask = envmapMask;
                entry->selfIllumChannel = 3;
            }
        }
        if (entry->selfIllumMask.empty()) {
            entry->selfIllumMask = selfIllumMask.empty() ? entry->baseTexture : selfIllumMask;
            entry->selfIllumChannel = selfIllumMask.empty() ? 3u : 0u;
        }
        entry->selfIllumTint = MaterialColor(text, "$selfillumtint", {1.0f, 1.0f, 1.0f});
        entry->selfIllumVmtTint = entry->selfIllumTint;
        // $selfillumfresnel (bumped VertexLitGeneric without $detail, whose shader alone has the SELFILLUMFRESNEL combo)
        // scales the glow by the angle it is seen at: the draw (stdshader_dx9.dll+0x59a00) sets c11 to (1 - min / max,
        // min / max, exp, max) from $selfillumfresnelminmaxexp (default 0 1 1; REA 2026-11-04), which the SDK's
        // vertexlit_and_unlit_generic_bump_ps2x.fxc turns into max * saturate(min / max + (1 - min / max) cos^exp) =
        // min + (max - min) cos^exp of the view's angle to the vertex normal. The light the glow gives others is its mean
        // over the directions it leaves in, weighted by their cosine: min + (max - min) * 2 / (exp + 2).
        if (shader == "vertexlitgeneric" && enabled("$selfillumfresnel") && studio::MaterialTexture(text, "$detail").empty() &&
            !studio::MaterialTexture(text, "$bumpmap").empty()) {
            const bsp::Float3 fresnel = MaterialColor(text, "$selfillumfresnelminmaxexp", {0.0f, 1.0f, 1.0f});
            const float mean = fresnel.x + (fresnel.y - fresnel.x) * 2.0f / (std::max(fresnel.z, 0.0f) + 2.0f);
            entry->selfIllumTint = {entry->selfIllumTint.x * mean, entry->selfIllumTint.y * mean, entry->selfIllumTint.z * mean};
        }
    }
    // VertexLitGeneric draws the layer after its own passes when $emissiveblendenabled is set and $emissiveblendstrength
    // is above 0 (OnDrawElements, stdshader_dx9.dll+0x591d0; the pass +0x223d0 adds s0 $emissiveblendbasetexture times
    // s2 $emissiveblendtexture, its coordinates moved by s1 $emissiveblendflowtexture and c1 $emissiveblendscrollvector,
    // times c2 $emissiveblendtint and c0 $emissiveblendstrength clamped to 0-1, base and emissive textures read as sRGB;
    // the SDK's emissive_scroll_blended_pass_ps2x.fxc. REA 2026-11-05).
    if (shader == "vertexlitgeneric" && enabled("$emissiveblendenabled")) {
        entry->emissiveBlendStrength = std::clamp(MaterialFloat(text, "$emissiveblendstrength", 0.0f), 0.0f, 1.0f);
        entry->emissiveBlendBase = studio::MaterialTexture(text, "$emissiveblendbasetexture");
        entry->emissiveBlendTexture = studio::MaterialTexture(text, "$emissiveblendtexture");
        entry->emissiveBlendTint = MaterialColor(text, "$emissiveblendtint", {1.0f, 1.0f, 1.0f});
        entry->emissiveBlend = entry->emissiveBlendStrength > 0.0f && !entry->emissiveBlendBase.empty() && !entry->emissiveBlendTexture.empty();
    }
    entry->sceneMaterial = SceneMaterial(text, shader, *entry, IsUnlitShader(shader));
    if (const std::optional<SwayMaterial> sway = MaterialSway(text)) {
        entry->swayMaterial = static_cast<uint32_t>(g_state.swayMaterials.size());
        g_state.swayMaterials.push_back(*sway);
    }
    return entry;
}

// The first of a model texture's candidate VMTs that exists, or nullptr.
const MaterialInfo* TextureMaterial(const std::vector<std::string>& vmtPaths)
{
    for (const std::string& vmtPath : vmtPaths) {
        if (const std::optional<MaterialInfo>& material = MaterialFor(vmtPath))
            return &*material;
    }
    return nullptr;
}

// The alpha-test or translucency entry of a material's triangles, kOpaqueTriangle, or kDroppedTriangle for
// clear materials.
uint32_t TriangleAlphaFor(const MaterialInfo& material)
{
    if (material.clear)
        return kDroppedTriangle;
    if (material.refract) {
        const SceneTexture* tint = material.refractTintTexture.empty() ? nullptr : &TextureFor(material.refractTintTexture);
        return Scene::PackTranslucent(tint != nullptr ? tint->view : DescriptorHeap::kInvalid, 1.0f);
    }
    if (!material.alphaTest && !material.translucent)
        return kOpaqueTriangle;
    const SceneTexture* texture = material.baseTexture.empty() ? nullptr : &TextureFor(material.baseTexture);
    if (material.alphaTest) {
        const bool alpha = texture != nullptr && texture->view != DescriptorHeap::kInvalid && texture->alpha;
        return alpha ? Scene::PackAlpha(texture->view, material.alphaReference) : kOpaqueTriangle;
    }
    // A texture without alpha reads as opaque, leaving $alpha; its colour filters the light either way.
    return Scene::PackTranslucent(texture != nullptr ? texture->view : DescriptorHeap::kInvalid, material.opacity);
}

// How a translucent triangle colours the light through it (Scene::PackTint): by Refract's tint, or for other translucent
// surfaces by their texture's colour.
uint32_t TriangleTintFor(const MaterialInfo& material)
{
    const uint32_t glass = material.glass ? Scene::kTintGlass : 0;
    return material.refract ? Scene::PackTint(material.refractTint, Scene::kTintRefract | glass) : Scene::PackTint({1.0f, 1.0f, 1.0f}, Scene::kTintFilter | glass);
}

// Moves the triangles whose `triangleAlpha` is not kOpaqueTriangle behind the others and removes those marked
// kDroppedTriangle, reordering `indices` and every per-triangle vector in `perTriangle` alike.
template <class... PerTriangle>
TriangleData PartitionTriangles(std::vector<uint32_t>& indices, const std::vector<uint32_t>& triangleAlpha, const std::vector<uint32_t>& triangleTint,
    PerTriangle&... perTriangle)
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
    for (auto it = firstAlpha; it != order.end(); ++it) {
        partition.alphaTriangles.push_back(triangleAlpha[*it]);
        partition.alphaTints.push_back(triangleTint[*it]);
    }
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

// A VMT texture transform ("center .5 .5 scale 1 1 rotate 0 translate 0 0") as the rows of its 2x3 matrix over
// (u, v, 1): scaled and rotated about the centre, then translated, as the SDK's TextureTransform proxy composes the same
// parts (src/game/client/matrixproxy.cpp).
std::array<float, 6> TextureTransform(std::string_view text)
{
    float center[2] = {0.5f, 0.5f};
    float scale[2] = {1.0f, 1.0f};
    float rotate = 0.0f;
    float translate[2] = {0.0f, 0.0f};
    std::vector<std::string_view> words;
    for (size_t i = 0; i < text.size();) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        const size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        if (i > start)
            words.push_back(text.substr(start, i - start));
    }
    const auto number = [&](size_t index, float& value) {
        if (index < words.size())
            std::from_chars(words[index].data(), words[index].data() + words[index].size(), value);
    };
    for (size_t w = 0; w < words.size(); ++w) {
        if (words[w] == "center") {
            number(w + 1, center[0]);
            number(w + 2, center[1]);
        } else if (words[w] == "scale") {
            number(w + 1, scale[0]);
            number(w + 2, scale[1]);
        } else if (words[w] == "rotate") {
            number(w + 1, rotate);
        } else if (words[w] == "translate") {
            number(w + 1, translate[0]);
            number(w + 2, translate[1]);
        }
    }
    const float radians = rotate * 3.14159265358979f / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float a[2][2] = {{c * scale[0], -s * scale[1]}, {s * scale[0], c * scale[1]}};
    return {a[0][0], a[0][1], center[0] + translate[0] - a[0][0] * center[0] - a[0][1] * center[1],
        a[1][0], a[1][1], center[1] + translate[1] - a[1][0] * center[0] - a[1][1] * center[1]};
}

// The keys and values of each block named `proxy` in a VMT's Proxies, in lower case.
std::vector<std::unordered_map<std::string, std::string>> MaterialProxies(std::string_view vmt, std::string_view proxy)
{
    std::vector<std::string> tokens;
    for (size_t i = 0; i < vmt.size();) {
        const char c = vmt[i];
        if (c == '/' && i + 1 < vmt.size() && vmt[i + 1] == '/') {
            i = vmt.find('\n', i);
            if (i == std::string_view::npos)
                break;
        } else if (c == '"') {
            const size_t end = vmt.find('"', i + 1);
            if (end == std::string_view::npos)
                break;
            tokens.emplace_back(vmt.substr(i + 1, end - i - 1));
            i = end + 1;
        } else if (c == '{' || c == '}') {
            tokens.emplace_back(1, c);
            ++i;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else {
            size_t end = i;
            while (end < vmt.size() && !std::isspace(static_cast<unsigned char>(vmt[end])) && vmt[end] != '"' && vmt[end] != '{' && vmt[end] != '}')
                ++end;
            tokens.emplace_back(vmt.substr(i, end - i));
            i = end;
        }
    }
    for (std::string& token : tokens)
        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<std::unordered_map<std::string, std::string>> blocks;
    for (size_t t = 0; t + 1 < tokens.size(); ++t) {
        if (tokens[t] != proxy || tokens[t + 1] != "{")
            continue;
        std::unordered_map<std::string, std::string>& block = blocks.emplace_back();
        for (t += 2; t + 1 < tokens.size() && tokens[t] != "}"; t += 2)
            block[tokens[t]] = tokens[t + 1];
    }
    return blocks;
}

float ProxyFloat(const std::unordered_map<std::string, std::string>& block, const std::string& key, float fallback)
{
    const auto value = block.find(key);
    float result = fallback;
    if (value == block.end() || std::from_chars(value->second.data(), value->second.data() + value->second.size(), result).ec != std::errc())
        return fallback;
    return result;
}

// The most frames of a water normal map that are loaded.
constexpr uint32_t kMaxWaterFrames = 64;

// A body of water for the scene, from its surface material (the Water shader's parameters, water.cpp in the SDK) and
// its normal map's frames, which go to `frameViews`. `normalMaps` caches the frames of each material.
GpuWaterBody WaterBodyFor(const bsp::WaterBody& body, std::vector<uint32_t>& frameViews, std::unordered_map<std::string, std::pair<uint32_t, uint32_t>>& normalMaps)
{
    GpuWaterBody gpu = {};
    gpu.surfaceZ = body.surfaceZ;
    std::copy_n(body.textureS, 4, gpu.textureS);
    std::copy_n(body.textureT, 4, gpu.textureT);
    gpu.mins[0] = body.mins.x - 1.0f;
    gpu.mins[1] = body.mins.y - 1.0f;
    gpu.mins[2] = body.minZ;
    gpu.maxs[0] = body.maxs.x + 1.0f;
    gpu.maxs[1] = body.maxs.y + 1.0f;
    gpu.maxs[2] = body.surfaceZ;
    std::string text;
    if (body.material < g_state.mapMaterials.size()) {
        std::string path = "materials/" + g_state.mapMaterials[body.material] + ".vmt";
        std::replace(path.begin(), path.end(), '\\', '/');
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string shader;
        text = MaterialText(path, shader).value_or(std::string());
    }
    // Water_DX9's defaults: a red fog colour, fog from 0 to 0 units, white tints, dev/water_normal.
    const bsp::Float3 fog = GammaToLinear(MaterialColor(text, "$fogcolor", {1.0f, 0.0f, 0.0f}));
    const float fogStart = MaterialFloat(text, "$fogstart", 0.0f);
    const float fogEnd = MaterialFloat(text, "$fogend", 0.0f);
    // The water's mean free path matches the mean visibility of Source's linear fog: as far as the surface beyond stays
    // visible on average, (start + end) / 2. Light that crosses it keeps the fog colour's balance: colours the fog
    // lacks are absorbed faster, by the log of how much it lacks them (at most e^-4 more), scaled so that the extinction
    // averaged by luminance is 1 / that path.
    const float meanFreePath = std::max(0.5f * (fogStart + fogEnd), 8.0f);
    const float fogColor[3] = {fog.x, fog.y, fog.z};
    const float brightest = std::max({fog.x, fog.y, fog.z, 1e-4f});
    constexpr float kLuminance[3] = {0.2126f, 0.7152f, 0.0722f};
    float k[3];
    float average = 0.0f;
    for (int c = 0; c < 3; ++c) {
        k[c] = 1.0f + std::min(std::log(brightest / std::max(fogColor[c], 1e-4f)), 4.0f);
        average += kLuminance[c] * k[c];
    }
    for (int c = 0; c < 3; ++c) {
        gpu.extinction[c] = k[c] / (average * meanFreePath);
        gpu.fogColor[c] = fogColor[c];
    }
    const bsp::Float3 reflectTint = GammaToLinear(MaterialColor(text, "$reflecttint", {1.0f, 1.0f, 1.0f}));
    gpu.reflectTint = (reflectTint.x + reflectTint.y + reflectTint.z) / 3.0f;
    const bsp::Float3 refractTint = GammaToLinear(MaterialColor(text, "$refracttint", {1.0f, 1.0f, 1.0f}));
    gpu.refractTint[0] = refractTint.x;
    gpu.refractTint[1] = refractTint.y;
    gpu.refractTint[2] = refractTint.z;
    const std::optional<std::string> bumpTransform = studio::MaterialValue(text, "$bumptransform");
    const std::array<float, 6> bump = TextureTransform(bumpTransform.value_or(std::string()));
    gpu.bumpS[0] = bump[0];
    gpu.bumpS[1] = bump[1];
    gpu.bumpS[3] = bump[2];
    gpu.bumpT[0] = bump[3];
    gpu.bumpT[1] = bump[4];
    gpu.bumpT[3] = bump[5];
    // A TextureScroll proxy on $bumptransform replaces it with its scale and an offset that moves at its rate along its
    // angle (texturescrollmaterialproxy.cpp in the SDK); the offset per second goes in the rows' z.
    for (const auto& scroll : MaterialProxies(text, "texturescroll")) {
        const auto var = scroll.find("texturescrollvar");
        if (var == scroll.end() || var->second != "$bumptransform")
            continue;
        const float rate = ProxyFloat(scroll, "texturescrollrate", 1.0f);
        const float angle = ProxyFloat(scroll, "texturescrollangle", 0.0f) * 3.14159265358979f / 180.0f;
        const float scale = ProxyFloat(scroll, "texturescale", 1.0f);
        const float rows[2][4] = {{scale, 0.0f, rate * std::cos(angle), 0.0f}, {0.0f, scale, rate * std::sin(angle), 0.0f}};
        std::copy_n(rows[0], 4, gpu.bumpS);
        std::copy_n(rows[1], 4, gpu.bumpT);
    }

    std::string normalMap = studio::MaterialTexture(text, "$normalmap");
    if (normalMap.empty())
        normalMap = "materials/dev/water_normal.vtf";
    // The AnimatedTexture proxy steps $bumpframe through the normal map's frames (baseanimatedtextureproxy.cpp in the
    // SDK: 15 frames a second by default); without it the material's $bumpframe stays.
    bool animated = false;
    for (const auto& proxy : MaterialProxies(text, "animatedtexture")) {
        const auto var = proxy.find("animatedtexturevar");
        if (var != proxy.end() && var->second == "$normalmap") {
            animated = true;
            gpu.frameRate = ProxyFloat(proxy, "animatedtextureframerate", 15.0f);
        }
    }
    const uint32_t fixedFrame = static_cast<uint32_t>(std::max(MaterialFloat(text, "$bumpframe", 0.0f), 0.0f));
    const std::string key = animated ? normalMap : std::format("{}#{}", normalMap, fixedFrame);
    auto cached = normalMaps.find(key);
    if (cached == normalMaps.end()) {
        const std::vector<std::byte> file = ReadOptional(normalMap);
        std::string error;
        std::optional<studio::TextureImage> first = studio::ReadTexture(file, kSceneTextureSize, error);
        const uint32_t frames = !first ? 0 : animated ? std::min(first->frames, kMaxWaterFrames) : 1;
        const uint32_t firstView = static_cast<uint32_t>(frameViews.size());
        for (uint32_t f = 0; f < frames; ++f) {
            const std::optional<studio::TextureImage> image = studio::ReadTexture(file, kSceneTextureSize, error, animated ? f : std::min(fixedFrame, first->frames - 1));
            const UINT view = image ? AddSceneTexture(*image) : DescriptorHeap::kInvalid;
            if (view == DescriptorHeap::kInvalid) {
                frameViews.resize(firstView);
                break;
            }
            frameViews.push_back(view);
        }
        if (frameViews.size() == firstView)
            log::Info("Scene: water normal map '{}' unusable ({}); its caustics and reflected ripples are flat", normalMap, file.empty() ? "file not found" : error);
        cached = normalMaps.emplace(key, std::pair{firstView, static_cast<uint32_t>(frameViews.size()) - firstView}).first;
    }
    gpu.framesFirst = cached->second.first;
    gpu.frameCount = cached->second.second;
    return gpu;
}

// Partitions a BSP mesh by its materials' alpha tests and describes it for Scene::AddModel.
ModelGeometry PrepareMesh(bsp::Mesh& mesh, TriangleData& triangles)
{
    std::vector<uint32_t> triangleAlpha(mesh.triangleMaterials.size(), kOpaqueTriangle);
    std::vector<uint32_t> triangleTint(mesh.triangleMaterials.size(), 0);
    for (size_t t = 0; t < triangleAlpha.size(); ++t) {
        const std::optional<MaterialInfo>* material = MapMaterial(mesh.triangleMaterials[t]);
        if (material != nullptr && *material) {
            triangleAlpha[t] = TriangleAlphaFor(**material);
            triangleTint[t] = TriangleTintFor(**material);
        }
    }
    triangles = PartitionTriangles(mesh.indices, triangleAlpha, triangleTint, mesh.triangleAlbedo, mesh.triangleMaterials, mesh.triangleFaces);
    triangles.materials.resize(mesh.triangleMaterials.size(), Scene::kNoMaterial);
    for (size_t t = 0; t < triangles.materials.size(); ++t) {
        const std::optional<MaterialInfo>* material = MapMaterial(mesh.triangleMaterials[t]);
        if (material != nullptr && *material) {
            const uint32_t texdata = mesh.triangleMaterials[t];
            const bool lightTexture = texdata < g_state.lightMaterials.size() && g_state.lightMaterials[texdata];
            triangles.materials[t] = lightTexture ? LightTextureMaterial((*material)->sceneMaterial) : (*material)->sceneMaterial;
        }
    }
    return {mesh.positions, mesh.indices, mesh.triangleAlbedo, triangles.opaqueTriangles, mesh.uvs, triangles.alphaTriangles, triangles.alphaTints,
        triangles.materials, mesh.blends};
}

uint32_t PackAlbedo(const bsp::Float3& c)
{
    auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return channel(c.x) | channel(c.y) << 8 | channel(c.z) << 16 | 0xFF000000u;
}

// Loads a model once per skin family: a BLAS in its bind pose for rigid use, plus skinning data for multi-bone models.
// Brush entity models ("*N") come from the map.
const ModelEntry& ModelFor(const std::string& mdlPath, int skin = 0)
{
    const std::string key = skin != 0 ? std::format("{}#{}", mdlPath, skin) : mdlPath;
    auto cached = g_state.models.find(key);
    if (cached != g_state.models.end())
        return cached->second;
    ModelEntry& entry = g_state.models[key];
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
        std::vector<uint32_t> textureTint(texturePaths.size(), 0);
        std::vector<uint32_t> textureMaterial(texturePaths.size(), Scene::kNoMaterial);
        std::vector<uint32_t> textureSway(texturePaths.size(), SwayingModel::kNoSway);
        std::vector<const MaterialInfo*> textureInfo(texturePaths.size(), nullptr);
        for (size_t t = 0; t < texturePaths.size(); ++t) {
            const MaterialInfo* material = TextureMaterial(texturePaths[t]);
            textureInfo[t] = material;
            if (material == nullptr)
                continue;
            skip[t] = material->clear;
            textureAlbedo[t] = PackAlbedo(material->reflectivity);
            textureAlpha[t] = TriangleAlphaFor(*material);
            textureTint[t] = TriangleTintFor(*material);
            textureMaterial[t] = material->sceneMaterial;
            textureSway[t] = material->swayMaterial;
        }
        if (studio::BuildGeometry(mdl, vvd, vtx, skip, geometry, error, skin) && !geometry.indices.empty()) {
            std::vector<uint32_t> triangleAlbedo(geometry.triangleTextures.size());
            std::vector<uint32_t> triangleAlpha(geometry.triangleTextures.size());
            std::vector<uint32_t> triangleTint(geometry.triangleTextures.size());
            for (size_t i = 0; i < triangleAlbedo.size(); ++i) {
                const uint16_t texture = geometry.triangleTextures[i];
                triangleAlbedo[i] = texture < textureAlbedo.size() ? textureAlbedo[texture] : PackAlbedo(kDefaultReflectivity);
                triangleAlpha[i] = texture < textureAlpha.size() ? textureAlpha[texture] : kOpaqueTriangle;
                triangleTint[i] = texture < textureTint.size() ? textureTint[texture] : 0;
            }
            TriangleData triangles = PartitionTriangles(geometry.indices, triangleAlpha, triangleTint, triangleAlbedo, geometry.triangleTextures);
            for (uint16_t texture : geometry.triangleTextures)
                triangles.materials.push_back(texture < textureMaterial.size() ? textureMaterial[texture] : Scene::kNoMaterial);
            entry.sceneModel = g_state.scene.AddModel({geometry.positions, geometry.indices, triangleAlbedo, triangles.opaqueTriangles, geometry.uvs,
                triangles.alphaTriangles, triangles.alphaTints, triangles.materials});
            if (entry.sceneModel >= 0) {
                if (std::vector<ModelEmitter> emitters = ModelEmitters(geometry.positions, geometry.indices, triangleAlbedo, triangles.materials); !emitters.empty())
                    g_state.modelEmitters[entry.sceneModel] = std::move(emitters);
                entry.glowPatches = GlowPatches(geometry, triangleAlbedo, textureInfo, studio::BoneCount(mdl) > 1);
                if (entry.glowPatches) {
                    for (const MaterialInfo* info : textureInfo)
                        entry.glowMaterials.push_back({info != nullptr ? info->name : std::string(), info != nullptr ? info->selfIllumVmtTint : bsp::Float3{}});
                }
            }
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
                if (poseToBone.size() == 1) {
                    entry.rootPoseToBone = poseToBone[0];
                    entry.staticProp = studio::IsStaticProp(mdl);
                }
                entry.rightHand = studio::BoneIndex(mdl, kRightHandBone);
                // Characters: players, humans and other character folders, or a humanoid skeleton. Their $halflambert
                // materials are lit as skin (subsurface scattering, shaders/deferred_lighting.hlsl).
                if (mdlPath.starts_with("models/player/") || mdlPath.starts_with("models/humans/") || mdlPath.starts_with("models/characters/") ||
                    studio::BoneIndex(mdl, kHeadBone) >= 0)
                    g_state.scene.MarkCharacter(entry.sceneModel);
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

// Ray tests of SealedLights: rays from a light stop at triangles within kShellReach, and when all but kSealedEscape of
// kSealRays directions stop, the light is sealed. A light inside a hollow building prop reaches walls further away.
// A light its fixture encloses closely (kCloseShellShare of the directions stop at the prop within kCloseShell) is
// sealed too, open sides or not: the bulbs of atmospherichouse's chandeliers sit in its ceiling cap, which threw
// magnified, stepped shadows of its arms onto the ceiling an inch away (2026-11-03).
constexpr int kSealRays = 128;
constexpr float kShellReach = 64.0f;
constexpr float kSealedEscape = 0.1f;
constexpr float kCloseShell = 12.0f;
constexpr float kCloseShellShare = 0.15f;
// Props whose bounds come this close to a light may seal it.
constexpr float kShellMargin = 1.0f;

bsp::Float3 Subtract(const bsp::Float3& a, const bsp::Float3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Dot(const bsp::Float3& a, const bsp::Float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
bsp::Float3 Cross(const bsp::Float3& a, const bsp::Float3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// Where a ray meets a triangle from either side (Moller-Trumbore), or tMax when it misses before it.
float RayTriangle(const bsp::Float3& origin, const bsp::Float3& direction, const bsp::Float3* corner, float tMax)
{
    const bsp::Float3 edge1 = Subtract(corner[1], corner[0]);
    const bsp::Float3 edge2 = Subtract(corner[2], corner[0]);
    const bsp::Float3 p = Cross(direction, edge2);
    const float determinant = Dot(edge1, p);
    if (std::fabs(determinant) < 1e-8f)
        return tMax;
    const float inverse = 1.0f / determinant;
    const bsp::Float3 s = Subtract(origin, corner[0]);
    const float u = Dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f)
        return tMax;
    const bsp::Float3 q = Cross(s, edge1);
    const float v = Dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f)
        return tMax;
    const float t = Dot(edge2, q) * inverse;
    return t > 0.01f && t < tMax ? t : tMax;
}

// A model's opaque triangles in model space, three positions each: those TriangleAlphaFor leaves opaque, of LOD0 and
// the skin family.
std::vector<bsp::Float3> OpaqueModelTriangles(const std::string& mdlPath, int skin)
{
    std::vector<bsp::Float3> triangles;
    const std::string base = mdlPath.size() > 4 ? mdlPath.substr(0, mdlPath.size() - 4) : mdlPath;
    const std::vector<std::byte> mdl = ReadOptional(mdlPath);
    std::vector<std::vector<std::string>> texturePaths;
    std::string error;
    if (mdl.empty() || !studio::ReadMaterialPaths(mdl, texturePaths, error))
        return triangles;
    std::vector<bool> skip(texturePaths.size(), false);
    for (size_t t = 0; t < texturePaths.size(); ++t) {
        const MaterialInfo* material = TextureMaterial(texturePaths[t]);
        skip[t] = material != nullptr && TriangleAlphaFor(*material) != kOpaqueTriangle;
    }
    studio::Geometry geometry;
    if (!studio::BuildGeometry(mdl, ReadOptional(base + ".vvd"), ReadOptional(base + ".dx90.vtx"), skip, geometry, error, skin))
        return triangles;
    triangles.reserve(geometry.indices.size());
    for (uint32_t index : geometry.indices)
        triangles.push_back(geometry.positions[index]);
    return triangles;
}

// Map lights sealed inside static props, as mappers placed lights in lamp models that VRAD's prop hulls then blocked.
// Rays leave each light whose prop bounds hold it towards the opaque triangles of those props and of the world close
// by. A sealed light gets kLightSealed, and the props its rays stopped at become lamp shells (`shells`, by prop),
// which its shadow rays pass as if the shell were a diffuser; to other rays they stay solid. It takes the shell's size:
// the mean distance to it, but not past the nearest world surface. Lamps with openings let their light out only
// through them. Returns each light's shape.
std::vector<MapLightShape> SealedLights(const bsp::WorldGeometry& world, const ModelGeometry& worldGeometry, std::vector<bool>& shells)
{
    std::vector<MapLightShape> shapes(world.lights.size());
    shells.assign(world.props.size(), false);
    const auto transform = [](const float m[3][4], const bsp::Float3& p) {
        return bsp::Float3{m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3], m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
    };
    struct ModelBounds {
        bool read = false;
        bool bounded = false;
        bsp::Float3 mins = {};
        bsp::Float3 maxs = {};
    };
    std::vector<ModelBounds> modelBounds(world.propModels.size());
    std::vector<std::pair<bsp::Float3, bsp::Float3>> propBoxes(world.props.size());
    for (size_t p = 0; p < world.props.size(); ++p) {
        const bsp::StaticProp& prop = world.props[p];
        ModelBounds& bounds = modelBounds[prop.model];
        if (!bounds.read) {
            bounds.read = true;
            bounds.bounded = studio::ReadBounds(ReadOptional(world.propModels[prop.model]), bounds.mins, bounds.maxs);
        }
        auto& [low, high] = propBoxes[p];
        low = {INFINITY, INFINITY, INFINITY};
        high = {-INFINITY, -INFINITY, -INFINITY};
        for (int corner = 0; corner < 8 && bounds.bounded; ++corner) {
            const bsp::Float3 c = transform(prop.transform, {corner & 1 ? bounds.maxs.x : bounds.mins.x, corner & 2 ? bounds.maxs.y : bounds.mins.y,
                corner & 4 ? bounds.maxs.z : bounds.mins.z});
            low = {std::min(low.x, c.x - kShellMargin), std::min(low.y, c.y - kShellMargin), std::min(low.z, c.z - kShellMargin)};
            high = {std::max(high.x, c.x + kShellMargin), std::max(high.y, c.y + kShellMargin), std::max(high.z, c.z + kShellMargin)};
        }
    }

    struct Candidate {
        uint32_t light;
        std::vector<uint32_t> props;
        std::vector<uint32_t> shells;
        float radius = kBulbRadius;
    };
    std::vector<Candidate> candidates;
    for (uint32_t i = 0; i < world.lights.size(); ++i) {
        if (world.lights[i].type == bsp::LightType::Surface)
            continue;
        Candidate candidate{i};
        for (uint32_t p = 0; p < world.props.size(); ++p) {
            if (bsp::InBounds(world.lights[i].origin, propBoxes[p].first, propBoxes[p].second))
                candidate.props.push_back(p);
        }
        if (!candidate.props.empty())
            candidates.push_back(std::move(candidate));
    }
    // Read once, here: materials and files go through the shared state.
    std::unordered_map<uint64_t, std::vector<bsp::Float3>> modelTriangles;
    const auto modelKey = [&](uint32_t p) { return static_cast<uint64_t>(world.props[p].model) | static_cast<uint64_t>(world.props[p].skin) << 32; };
    for (const Candidate& candidate : candidates) {
        for (uint32_t p : candidate.props) {
            if (!modelTriangles.contains(modelKey(p)))
                modelTriangles[modelKey(p)] = OpaqueModelTriangles(world.propModels[world.props[p].model], world.props[p].skin);
        }
    }

    std::for_each(std::execution::par, candidates.begin(), candidates.end(), [&](Candidate& candidate) {
        const bsp::Float3 origin = world.lights[candidate.light].origin;
        const bsp::Float3 low = {origin.x - kShellReach, origin.y - kShellReach, origin.z - kShellReach};
        const bsp::Float3 high = {origin.x + kShellReach, origin.y + kShellReach, origin.z + kShellReach};
        // World-space corners of each triangle in reach, and the index in candidate.props of its prop (-1: the world).
        std::vector<bsp::Float3> corners;
        std::vector<int32_t> owners;
        for (size_t n = 0; n < candidate.props.size(); ++n) {
            const bsp::StaticProp& prop = world.props[candidate.props[n]];
            for (const bsp::Float3& p : modelTriangles.at(modelKey(candidate.props[n])))
                corners.push_back(transform(prop.transform, p));
            owners.resize(corners.size() / 3, static_cast<int32_t>(n));
        }
        for (size_t t = 0; t < worldGeometry.opaqueTriangles; ++t) {
            const bsp::Float3* triangle[3];
            bsp::Float3 tLow = {INFINITY, INFINITY, INFINITY};
            bsp::Float3 tHigh = {-INFINITY, -INFINITY, -INFINITY};
            for (int k = 0; k < 3; ++k) {
                triangle[k] = &worldGeometry.positions[worldGeometry.indices[t * 3 + k]];
                tLow = {std::min(tLow.x, triangle[k]->x), std::min(tLow.y, triangle[k]->y), std::min(tLow.z, triangle[k]->z)};
                tHigh = {std::max(tHigh.x, triangle[k]->x), std::max(tHigh.y, triangle[k]->y), std::max(tHigh.z, triangle[k]->z)};
            }
            if (tHigh.x < low.x || tLow.x > high.x || tHigh.y < low.y || tLow.y > high.y || tHigh.z < low.z || tLow.z > high.z)
                continue;
            for (int k = 0; k < 3; ++k)
                corners.push_back(*triangle[k]);
            owners.push_back(-1);
        }
        std::vector<uint32_t> hits(candidate.props.size(), 0);
        int escaped = 0;
        int closeHits = 0;
        float shellDistance = 0.0f;
        float nearestWorld = kShellReach;
        for (int r = 0; r < kSealRays; ++r) {
            // Directions spread evenly over the sphere (a Fibonacci lattice).
            const float z = 1.0f - (2.0f * static_cast<float>(r) + 1.0f) / kSealRays;
            const float ring = std::sqrt(std::max(1.0f - z * z, 0.0f));
            const float angle = 2.39996323f * static_cast<float>(r);
            const bsp::Float3 direction = {ring * std::cos(angle), ring * std::sin(angle), z};
            float nearest = kShellReach;
            int32_t owner = -2;
            for (size_t t = 0; t < owners.size(); ++t) {
                const float hit = RayTriangle(origin, direction, &corners[t * 3], nearest);
                if (hit < nearest) {
                    nearest = hit;
                    owner = owners[t];
                }
            }
            if (owner == -2) {
                ++escaped;
            } else if (owner >= 0) {
                ++hits[owner];
                shellDistance += nearest;
                if (nearest <= kCloseShell)
                    ++closeHits;
            } else {
                nearestWorld = std::min(nearestWorld, nearest);
            }
        }
        static const bool developer = HasArgument(L"-gmodpp_dev");
        if (developer) {
            log::Info("Scene: light {} at ({:.0f} {:.0f} {:.0f}) in {} prop(s): {} of {} rays escape, {} stop at a prop within {} units", candidate.light,
                origin.x, origin.y, origin.z, candidate.props.size(), escaped, kSealRays, closeHits, kCloseShell);
        }
        if (escaped > kSealedEscape * kSealRays && closeHits < kCloseShellShare * kSealRays)
            return;
        uint32_t shellHits = 0;
        for (size_t n = 0; n < hits.size(); ++n) {
            if (hits[n] != 0)
                candidate.shells.push_back(candidate.props[n]);
            shellHits += hits[n];
        }
        if (shellHits != 0)
            candidate.radius = std::clamp(shellDistance / static_cast<float>(shellHits), std::min(kBulbRadius, nearestWorld), nearestWorld);
    });

    size_t sealed = 0;
    for (const Candidate& candidate : candidates) {
        if (candidate.shells.empty())
            continue;
        shapes[candidate.light] = {kLightSealed, candidate.radius};
        ++sealed;
        for (uint32_t p : candidate.shells)
            shells[p] = true;
    }
    log::Info("Scene: {} of {} lights in static props are sealed in {} lamp shells, which let their light out; the rest shine through their "
              "props' openings",
        sealed, candidates.size(), std::count(shells.begin(), shells.end(), true));
    return shapes;
}

// The scene's emitters (Scene::LoadMap): the self-illuminated triangles of the world and of static props outside the
// 3D skybox, in world space, with the cumulative share of their emission times their area. Instances with some get
// kInstanceEmitter, and `worldEmits` says whether the world has some; the world is instance 0 and static instance i is
// i + 1 (Scene::RecordFrame).
std::vector<GpuEmitter> CollectEmitters(const std::vector<ModelEmitter>& worldEmitters, std::vector<SceneInstance>& instances, bool& worldEmits,
    float& totalPower)
{
    std::vector<GpuEmitter> emitters;
    std::vector<double> power;
    const auto add = [&](const std::vector<ModelEmitter>& model, uint32_t instance, const float (*m)[4]) {
        for (const ModelEmitter& emitter : model) {
            bsp::Float3 p[3];
            for (int k = 0; k < 3; ++k) {
                const bsp::Float3& c = emitter.corners[k];
                p[k] = m == nullptr ? c
                                    : bsp::Float3{m[0][0] * c.x + m[0][1] * c.y + m[0][2] * c.z + m[0][3], m[1][0] * c.x + m[1][1] * c.y + m[1][2] * c.z + m[1][3],
                                          m[2][0] * c.x + m[2][1] * c.y + m[2][2] * c.z + m[2][3]};
            }
            const bsp::Float3 n = Cross(Subtract(p[1], p[0]), Subtract(p[2], p[0]));
            const double area = 0.5 * std::sqrt(static_cast<double>(Dot(n, n)));
            if (area <= 0.0)
                continue;
            emitters.push_back({{p[0].x, p[0].y, p[0].z}, instance, {p[1].x, p[1].y, p[1].z}, emitter.primitive, {p[2].x, p[2].y, p[2].z}, 0.0f});
            power.push_back(area * emitter.emission);
        }
    };
    add(worldEmitters, 0, nullptr);
    worldEmits = !emitters.empty();
    for (size_t i = 0; i < instances.size(); ++i) {
        if (instances[i].mask == kSkyboxMask)
            continue;
        const auto found = g_state.modelEmitters.find(instances[i].model);
        if (found == g_state.modelEmitters.end())
            continue;
        const size_t before = emitters.size();
        add(found->second, static_cast<uint32_t>(i + 1), instances[i].transform);
        if (emitters.size() != before)
            instances[i].flags |= kInstanceEmitter;
    }
    double total = 0.0;
    for (double p : power)
        total += p;
    double cumulative = 0.0;
    for (size_t e = 0; e < emitters.size(); ++e) {
        cumulative += power[e];
        emitters[e].cdf = static_cast<float>(cumulative / total);
    }
    if (!emitters.empty())
        emitters.back().cdf = 1.0f;
    totalPower = static_cast<float>(total);
    return emitters;
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

// Identifies a map by its contents, for the baked light cache.
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
    g_state.brushWater.clear();
    g_state.brushWaterPlacements.clear();
    g_state.skyboxTest = {};
    g_state.mapMaterials.clear();
    g_state.textures.clear();
    g_state.fullTextures.clear();
    // Scene material indices and texture views are reset with the scene.
    g_state.materials.clear();
    g_state.sceneMaterials.clear();
    g_state.lightTextureMaterials.clear();
    g_state.modelEmitters.clear();
    g_state.maskImages.clear();
    g_state.tintChanges.clear();
    engine::ForgetMaterials();
    g_state.glowCandidates.clear();
    g_state.frameLights.clear();
    g_state.mapLightOrigins.clear();
    g_state.swayMaterials.clear();
    g_state.ropeModels.clear();
    g_state.loggedRopes = false;
}

// The map whose scene must be loaded, or nothing when the scene is up to date. Clears the scene when no map is loaded
// or real-time lighting is off.
std::optional<std::string> PendingLevel()
{
    if (!engine::Ready())
        return std::nullopt;
    if (!g_state.sceneInitialized) {
        g_state.sceneInitialized = g_state.scene.Initialize(g_state.device, g_state.queue, *g_state.descriptors, g_state.opacityMicromaps);
        if (!g_state.sceneInitialized)
            return std::nullopt;
    }
    std::string level = engine::LevelName();
    if (level.empty()) {
        if (g_state.scene.Ready()) {
            ClearScene();
            log::Info("Scene cleared");
        }
        return std::nullopt;
    }
    if (level == g_state.scene.Name() || level == g_state.failedMap)
        return std::nullopt;
    // The map's own settings apply from its first frame, the bake included.
    config::Sync(level);
    // With real-time lighting off the game lights itself; the map's scene loads once it is turned on.
    if (!GetSettings().realtimeLighting) {
        if (g_state.scene.Ready()) {
            ClearScene();
            log::Info("Scene cleared");
        }
        return std::nullopt;
    }
    return level;
}

// Replaces the scene with `level`'s, reporting the share done to `report`. Runs on the main thread, or on the loader
// thread while the game loads the map and draws nothing else.
void LoadScene(const std::string& level, const std::function<void(float)>& report)
{
    std::vector<std::byte> file;
    bsp::WorldGeometry world;
    std::string error;
    if (!engine::ReadGameFile(level, file)) {
        error = "the game filesystem could not read it";
    } else if (bsp::LoadWorld(file, world, error)) {
        report(0.05f);
        ClearScene();
        g_state.mapMaterials = std::move(world.materials);
        g_state.lightMaterials = world.lightMaterials;
        TriangleData worldTriangles;
        const ModelGeometry worldGeometry = PrepareMesh(world.mesh, worldTriangles);
        std::vector<bool> shells;
        const std::vector<MapLightShape> lightShapes = SealedLights(world, worldGeometry, shells);
        report(0.2f);
        std::vector<SceneInstance> instances;
        std::vector<SwayingProp> swayingProps;
        // `propShells` marks the props that are lamp shells.
        const auto addProps = [&](const std::vector<bsp::StaticProp>& props, UINT mask, const std::vector<bool>& propShells) {
            for (size_t p = 0; p < props.size(); ++p) {
                const bsp::StaticProp& prop = props[p];
                const ModelEntry& model = ModelFor(world.propModels[prop.model], prop.skin);
                SceneInstance instance;
                instance.model = model.sceneModel;
                if (mask == 0 && p < propShells.size() && propShells[p])
                    instance.mask = kLampShellMask;
                else
                    instance.mask = mask == 0 && prop.noShadow ? kShadowlessPropMask : mask;
                instance.flags = kInstanceStudio | (prop.ignoreNormals ? kInstanceIgnoreNormals : 0);
                std::memcpy(instance.transform, prop.transform, sizeof(instance.transform));
                // The 3D skybox's scaled-down props sway too little to matter.
                if (model.swaying && mask != kSkyboxMask)
                    swayingProps.push_back({model.swaying, instance});
                else
                    instances.push_back(instance);
            }
        };
        addProps(world.props, 0, shells);
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
        addProps(world.skyboxProps, kSkyboxMask, {});
        report(0.4f);
        std::erase_if(instances, [](const SceneInstance& instance) { return instance.model < 0; });
        bool worldEmits = false;
        float emitterPower = 0.0f;
        const std::vector<GpuEmitter> emitters = CollectEmitters(
            ModelEmitters(world.mesh.positions, world.mesh.indices, world.mesh.triangleAlbedo, worldTriangles.materials), instances, worldEmits, emitterPower);
        if (g_state.scene.LoadMap(level, world, lightShapes, worldGeometry, emitters, worldEmits, emitterPower)) {
            report(0.75f);
            for (const bsp::Light& light : world.lights) {
                if (light.type != bsp::LightType::Surface)
                    g_state.mapLightOrigins.push_back(light.origin);
            }
            if (!world.water.indices.empty() || !world.brushWater.empty()) {
                std::vector<GpuWaterBody> bodies;
                std::vector<uint32_t> frameViews;
                std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> normalMaps;
                for (const bsp::WaterBody& body : world.waterBodies)
                    bodies.push_back(WaterBodyFor(body, frameViews, normalMaps));
                const UINT brushFirst = static_cast<UINT>(bodies.size());
                const auto surfaceGeometry = [](const bsp::Mesh& mesh, std::vector<uint32_t>& albedo) {
                    albedo.assign(mesh.indices.size() / 3, PackAlbedo({1.0f, 1.0f, 1.0f}));
                    return ModelGeometry{mesh.positions, mesh.indices, albedo, static_cast<UINT>(albedo.size()), {}, {}, {}, {}};
                };
                for (const bsp::WorldGeometry::BrushWater& water : world.brushWater) {
                    std::vector<uint32_t> albedo;
                    const int model = g_state.scene.AddModel(surfaceGeometry(water.surface, albedo));
                    if (model < 0)
                        continue;
                    g_state.brushWater[water.model] = {model, static_cast<uint32_t>(bodies.size())};
                    bodies.push_back(WaterBodyFor(water.body, frameViews, normalMaps));
                }
                std::vector<uint32_t> albedo;
                SceneInstance water;
                water.model = world.water.indices.empty() ? -1 : g_state.scene.AddModel(surfaceGeometry(world.water, albedo));
                water.mask = kWaterMask;
                water.flags = kInstanceWater;
                const float identity[3][4] = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
                std::memcpy(water.transform, identity, sizeof(water.transform));
                const std::vector<uint32_t> triangleBodies = world.water.triangleMaterials.empty() ? std::vector<uint32_t>{SceneInstance::kNoWaterBody}
                                                                                                   : world.water.triangleMaterials;
                if (g_state.scene.SetWater(bodies, frameViews, triangleBodies, world.visibility, brushFirst)) {
                    if (water.model >= 0)
                        instances.push_back(water);
                    log::Info("Scene: {} bodies of water ({} in brush entities), {} surface triangles, {} normal map frames", bodies.size(),
                        bodies.size() - brushFirst, albedo.size(), frameViews.size());
                    for (size_t b = 0; b < bodies.size() && b < 16; ++b) {
                        const GpuWaterBody& body = bodies[b];
                        if (body.mins[0] <= body.maxs[0])
                            log::Info("Scene: water {} at z {:.0f} over ({:.0f} {:.0f})-({:.0f} {:.0f}), fog ({:.3f} {:.3f} {:.3f}), extinction ({:.5f} {:.5f} {:.5f}), {} frames at {:.0f}/s, "
                                      "texture axes ({:.5f} {:.5f} {:.5f} {:.3f}) ({:.5f} {:.5f} {:.5f} {:.3f})",
                                b, body.surfaceZ, body.mins[0], body.mins[1], body.maxs[0], body.maxs[1], body.fogColor[0], body.fogColor[1], body.fogColor[2],
                                body.extinction[0], body.extinction[1], body.extinction[2], body.frameCount, body.frameRate, body.textureS[0], body.textureS[1],
                                body.textureS[2], body.textureS[3], body.textureT[0], body.textureT[1], body.textureT[2], body.textureT[3]);
                    }
                }
            }
            g_state.brushModels = std::move(world.brushModels);
            g_state.skyboxTest.visibility.nodes = std::move(world.visibility.nodes);
            g_state.skyboxTest.visibility.leaves = std::move(world.visibility.leaves);
            g_state.skyboxTest.skyArea = world.skyArea;
            g_state.skyboxTest.skyMins = world.skyMins;
            g_state.skyboxTest.skyMaxs = world.skyMaxs;
            log::Info("Scene: {} of {} textures are VRAD light textures; {} of those glow ($selfillum) and light nothing again",
                std::count(g_state.lightMaterials.begin(), g_state.lightMaterials.end(), true), g_state.lightMaterials.size(),
                g_state.lightTextureMaterials.size());
            // Light baked before for this map, by the map's contents (gmodplusplus\cache\baked).
            const std::filesystem::path cachePath = ModDirectory() / L"cache" / L"baked" / std::format(L"{:016x}.bin", FileHash(file));
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
            report(0.9f);
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

bool LoaderBusy() { return g_state.loader.joinable() && !g_state.loaderFinished; }

void JoinLoader()
{
    if (g_state.loader.joinable())
        g_state.loader.join();
}

// Loads the scene of a map the client is in; maps being joined are loaded during their loading screen
// (BeginLevelLoad, PrepareLevel).
void UpdateScene()
{
    if (LoaderBusy())
        return;
    JoinLoader();
    if (const std::optional<std::string> level = PendingLevel(); level && engine::IsInGame())
        LoadScene(*level, [](float) {});
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
    model = g_state.scene.AddModel({prism.positions, prism.indices, albedo, static_cast<UINT>(albedo.size()), {}, {}, {}, {}});
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
// How a material's $selfillumtint now compares with the one its patches were made with, per colour channel: proxies
// (GMod's PlayerWeaponColor on the physics gun) set it as the game draws, so this is what the last entity drawn with it
// set. Read once a frame per material.
std::array<float, 3> TintChange(const ModelEntry& model, uint32_t texture)
{
    constexpr std::array<float, 3> kSame = {1.0f, 1.0f, 1.0f};
    if (texture >= model.glowMaterials.size() || model.glowMaterials[texture].name.empty())
        return kSame;
    const ModelEntry::GlowMaterial& material = model.glowMaterials[texture];
    auto [entry, added] = g_state.tintChanges.try_emplace(material.name);
    if (added || entry->second.first != g_state.frame) {
        entry->second.first = g_state.frame;
        entry->second.second = kSame;
        if (const std::optional<std::array<float, 3>> now = engine::MaterialVector(material.name, "$selfillumtint")) {
            const float made[3] = {material.tint.x, material.tint.y, material.tint.z};
            for (int c = 0; c < 3; ++c)
                entry->second.second[c] = made[c] > 0.0f ? std::max((*now)[c], 0.0f) / made[c] : 1.0f;
        }
    }
    return entry->second.second;
}

// The glowing patches of a moving model, as candidates for the main view's lights (GlowCandidate): placed by `transform`,
// or for a skinned model (`boneToWorld` not empty) by each patch's bones. `owner` and `kind` name its instance.
void AddGlowCandidates(const ModelEntry& model, int entityIndex, const studio::Matrix3x4* transform, std::span<const studio::Matrix3x4> boneToWorld,
    size_t owner, GlowCandidate::Owner kind, uint32_t flags)
{
    const Settings& settings = GetSettings();
    if (!model.glowPatches || settings.emissiveLighting != EmissiveLighting::Full || settings.emissiveIntensity <= 0.0f)
        return;
    const std::vector<glow::Patch>& patches = *model.glowPatches;
    for (size_t p = 0; p < patches.size(); ++p) {
        studio::Matrix3x4 placement = {};
        if (!boneToWorld.empty() && model.skinned) {
            // The bones' transforms blended by their shares, as shaders/skin.hlsl blends a vertex's.
            bool placed = true;
            for (size_t b = 0; b < glow::Patch::kBones && placed; ++b) {
                const uint32_t bone = patches[p].bones[b];
                const float weight = patches[p].boneWeights[b];
                if (weight <= 0.0f)
                    continue;
                placed = bone < boneToWorld.size() && bone < model.skinned->poseToBone.size();
                if (!placed)
                    break;
                const studio::Matrix3x4 matrix = studio::Multiply(boneToWorld[bone], model.skinned->poseToBone[bone]);
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 4; ++c)
                        placement[r][c] += weight * matrix[r][c];
                }
            }
            if (!placed)
                continue;
        } else if (transform != nullptr) {
            placement = *transform;
        } else {
            continue;
        }
        const std::array<float, 3> tint = patches[p].tinted ? TintChange(model, patches[p].material) : std::array<float, 3>{1.0f, 1.0f, 1.0f};
        if (tint[0] + tint[1] + tint[2] <= 0.0f)
            continue;
        GlowCandidate& candidate = g_state.glowCandidates.emplace_back();
        const uint64_t key = kGlowLightKey | static_cast<uint64_t>(static_cast<uint32_t>(entityIndex)) << 20 | p;
        candidate.light = glow::Place(patches[p], placement, settings.emissiveIntensity, key, flags);
        for (int c = 0; c < 3; ++c)
            candidate.light.color[c] *= tint[c];
        candidate.score = glow::Power(patches[p], placement);
        candidate.owner = owner;
        candidate.kind = kind;
    }
}

// The view model the local player sees in first person (a view model entity, not carried by the player): its glowing
// patches light the scene from where it is drawn. Returns whether it has any.
bool AddViewModelGlow(const entities::ModelEntity& entity)
{
    const ModelEntry& model = ModelFor(entity.model, entity.skin);
    if (!model.glowPatches)
        return false;
    if (model.skinned) {
        g_state.bones.resize(kMaxBones);
        if (!entities::SetupBones(entity.renderable, reinterpret_cast<float (*)[3][4]>(g_state.bones.data()), kMaxBones))
            return false;
        AddGlowCandidates(model, entity.index, nullptr, g_state.bones, 0, GlowCandidate::Owner::None, kLightOnLocalPlayer);
    } else {
        studio::Matrix3x4 transform;
        std::memcpy(transform.data(), entity.transform, sizeof(transform));
        AddGlowCandidates(model, entity.index, &transform, {}, 0, GlowCandidate::Owner::None, kLightOnLocalPlayer);
    }
    return true;
}

void AddHeldWeapon(const entities::ModelEntity& weapon, const studio::Matrix3x4& hand)
{
    const std::string worldModel = entities::WeaponWorldModel(weapon);
    if (worldModel.empty())
        return;
    const ModelEntry& model = ModelFor(worldModel, weapon.skin);
    if (model.sceneModel < 0 || !model.rightHandPoseToBone)
        return;
    const studio::Matrix3x4 transform = studio::Multiply(hand, *model.rightHandPoseToBone);
    SceneInstance& instance = g_state.dynamicInstances.emplace_back();
    instance.model = model.sceneModel;
    instance.mask = kLocalPlayerMask;
    instance.flags = kInstanceStudio;
    std::memcpy(instance.transform, transform.data(), sizeof(instance.transform));
    TrackMotion(instance, weapon.index);
    // Its glow lights the scene unless the view model's does (CollectEntities).
    g_state.heldWeaponGlow = State::HeldGlow{&model, transform, weapon.index, g_state.dynamicInstances.size() - 1};
}

// A brush entity's water surface (func_water_analog) where its entity is: an instance carrying its body of water, which
// Scene::PlaceBrushWater moves with it. Water surfaces are level, so only entities that are moved, not turned, carry it.
void AddBrushWater(const entities::ModelEntity& entity)
{
    if (!entity.model.starts_with('*'))
        return;
    uint32_t number = 0;
    const auto [end, ec] = std::from_chars(entity.model.data() + 1, entity.model.data() + entity.model.size(), number);
    const auto found = ec == std::errc() ? g_state.brushWater.find(number) : g_state.brushWater.end();
    if (found == g_state.brushWater.end())
        return;
    const auto& t = entity.transform;
    if (std::fabs(t[0][0] - 1.0f) > 1e-3f || std::fabs(t[1][1] - 1.0f) > 1e-3f || std::fabs(t[2][2] - 1.0f) > 1e-3f) {
        if (!std::exchange(g_state.loggedTurnedWater, true))
            log::Info("Scene: brush entity water '{}' is turned; it is left out", entity.model);
        return;
    }
    SceneInstance& instance = g_state.dynamicInstances.emplace_back();
    instance.model = found->second.sceneModel;
    instance.mask = kWaterMask;
    instance.flags = kInstanceWater;
    instance.waterBody = found->second.body;
    std::memcpy(instance.transform, entity.transform, sizeof(instance.transform));
    g_state.brushWaterPlacements.push_back({found->second.body, {t[0][3], t[1][3], t[2][3]}});
}

void CollectEntities()
{
    g_state.dynamicInstances.clear();
    g_state.skinnedEntities.clear();
    g_state.glowCandidates.clear();
    g_state.heldWeaponGlow.reset();
    bool viewModelGlows = false;
    g_state.currentTransforms.clear();
    g_state.brushWaterPlacements.clear();
    entities::Collect(g_state.entities);
    const int localPlayer = engine::LocalPlayerIndex();
    const auto player = std::find_if(g_state.entities.begin(), g_state.entities.end(), [&](const entities::ModelEntity& e) { return e.index == localPlayer; });
    const void* playerRenderable = player != g_state.entities.end() ? player->renderable : nullptr;
    g_state.localPlayer.reset();
    if (player != g_state.entities.end())
        g_state.localPlayer = std::array<float, 3>{player->transform[0][3], player->transform[1][3], player->transform[2][3]};
    std::optional<studio::Matrix3x4> playerHand;
    if (player != g_state.entities.end()) {
        const ModelEntry& body = ModelFor(player->model, player->skin);
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
            else if (!carried)
                viewModelGlows = AddViewModelGlow(entity) || viewModelGlows;
            continue;
        }
        AddBrushWater(entity);
        const ModelEntry& model = ModelFor(entity.model, entity.skin);
        if (model.sceneModel < 0)
            continue;
        const UINT mask = carried ? kLocalPlayerMask : InSkybox(entity, model.center) ? kSkyboxMask : kDynamicMask;
        const uint32_t flags = entity.model.starts_with('*') ? 0u : kInstanceStudio;
        // The 3D skybox's models are scaled down; their glow lights nothing in the map.
        const bool glows = mask != kSkyboxMask;
        const uint32_t lightFlags = carried ? kLightOnLocalPlayer : 0u;
        if (model.skinned) {
            g_state.bones.resize(kMaxBones);
            if (!entities::SetupBones(entity.renderable, reinterpret_cast<float (*)[3][4]>(g_state.bones.data()), kMaxBones))
                continue;
            g_state.bones.resize(std::min<size_t>(model.skinned->poseToBone.size(), kMaxBones));
            g_state.skinnedEntities.push_back({entity.index, model.sceneModel, model.skinned, g_state.bones, mask, flags});
            if (glows)
                AddGlowCandidates(model, entity.index, nullptr, g_state.skinnedEntities.back().boneToWorld, g_state.skinnedEntities.size() - 1,
                    GlowCandidate::Owner::Skinned, lightFlags);
            continue;
        }
        studio::Matrix3x4 transform;
        std::memcpy(transform.data(), entity.transform, sizeof(transform));
        if (model.rootPoseToBone) {
            g_state.bones.resize(kMaxBones);
            if (!entities::SetupBones(entity.renderable, reinterpret_cast<float (*)[3][4]>(g_state.bones.data()), kMaxBones))
                continue;
            studio::Matrix3x4 bone = g_state.bones[0];
            if (model.staticProp || entity.legacyTransform) {
                // A scale of zero or below draws the model collapsed or mirrored, which no instance stands for.
                if (!std::isfinite(entity.modelScale) || entity.modelScale <= 0.0f)
                    continue;
                for (auto& row : bone) {
                    for (int c = 0; c < 3; ++c)
                        row[c] *= entity.modelScale;
                }
            }
            transform = studio::Multiply(bone, *model.rootPoseToBone);
        }
        SceneInstance& instance = g_state.dynamicInstances.emplace_back();
        instance.model = model.sceneModel;
        instance.mask = mask;
        instance.flags = flags;
        std::memcpy(instance.transform, transform.data(), sizeof(instance.transform));
        TrackMotion(instance, entity.index);
        if (glows)
            AddGlowCandidates(model, entity.index, &transform, {}, g_state.dynamicInstances.size() - 1, GlowCandidate::Owner::Rigid, lightFlags);
    }
    if (g_state.heldWeaponGlow && !viewModelGlows) {
        const State::HeldGlow& held = *g_state.heldWeaponGlow;
        AddGlowCandidates(*held.model, held.index, &held.transform, {}, held.instance, GlowCandidate::Owner::Rigid, kLightOnLocalPlayer);
    }
    CollectRopes();
    g_state.scene.PlaceBrushWater(g_state.brushWaterPlacements);
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

// The main view's dynamic lights in Scene's runs (Scene::SetDynamicLights): the engine's dlights and the glowing patches
// it samples, then the patches that matter most to the view, lit exactly, then the flashes of effects. Patches are
// ranked by their power over their squared distance from the eye; a model whose patches all made it takes no glow at
// indirect rays' first hits (kInstanceGlowLights), as its light already arrives directly.
void BuildFrameLights(const std::array<float, 3>& eye)
{
    std::vector<FrameLight>& lights = g_state.frameLights;
    lights.clear();
    if (GetSettings().dynamicLights) {
        for (const engine::DynamicLight& light : g_state.dynamicLights) {
            if (light.radius > 0.0f && (light.flags & (engine::kDlightNoWorld | engine::kDlightNoModel)) != (engine::kDlightNoWorld | engine::kDlightNoModel))
                lights.push_back(effect_lights::FromEngineLight(light, 1.0f));
        }
    }
    std::vector<GlowCandidate>& candidates = g_state.glowCandidates;
    for (GlowCandidate& candidate : candidates) {
        const float d[3] = {candidate.light.origin[0] - eye[0], candidate.light.origin[1] - eye[1], candidate.light.origin[2] - eye[2]};
        candidate.score /= std::max(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], candidate.light.size * candidate.light.size);
    }
    const size_t kept = std::min(candidates.size(), static_cast<size_t>(Scene::kMaxGlowLights));
    std::partial_sort(candidates.begin(), candidates.begin() + kept, candidates.end(),
        [](const GlowCandidate& a, const GlowCandidate& b) { return a.score > b.score; });
    const size_t exact = std::min(kept, kExactGlowLights);
    for (size_t c = exact; c < kept; ++c)
        lights.push_back(candidates[c].light);
    g_state.frameExactFirst = lights.size();
    for (size_t c = 0; c < exact; ++c)
        lights.push_back(candidates[c].light);
    g_state.frameFlashFirst = lights.size();
    effect_lights::Collect({eye, g_state.dynamicLights, g_state.mapLightOrigins, g_state.localPlayer}, Scene::kMaxFlashLights, lights);

    std::vector<std::pair<uint32_t, uint32_t>> rigid(g_state.dynamicInstances.size());
    std::vector<std::pair<uint32_t, uint32_t>> skinned(g_state.skinnedEntities.size());
    for (size_t c = 0; c < candidates.size(); ++c) {
        if (candidates[c].kind == GlowCandidate::Owner::None)
            continue;
        auto& [total, lit] = candidates[c].kind == GlowCandidate::Owner::Skinned ? skinned[candidates[c].owner] : rigid[candidates[c].owner];
        ++total;
        lit += c < kept ? 1u : 0u;
    }
    for (size_t i = 0; i < rigid.size(); ++i) {
        if (rigid[i].first != 0 && rigid[i].first == rigid[i].second)
            g_state.dynamicInstances[i].flags |= kInstanceGlowLights;
    }
    for (size_t i = 0; i < skinned.size(); ++i) {
        if (skinned[i].first != 0 && skinned[i].first == skinned[i].second)
            g_state.skinnedEntities[i].flags |= kInstanceGlowLights;
    }
}


}

bool Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue, DescriptorHeap& descriptors, const RaytracingSupport& support,
    const std::filesystem::path& dlssDirectory)
{
    if (g_state.lighting)
        return true;
    auto lighting = std::make_unique<Lighting>();
    if (!lighting->Initialize(device, queue, descriptors, support, dlssDirectory))
        return false;
    g_state.device = device;
    g_state.queue = queue;
    g_state.descriptors = &descriptors;
    g_state.opacityMicromaps = support.opacityMicromaps;
    g_state.lighting = std::move(lighting);
    return true;
}

void Shutdown()
{
    if (!g_state.lighting)
        return;
    JoinLoader();
    ClearScene();
    g_state.lighting.reset();
}

bool Ready() { return g_state.lighting && g_state.lighting->Usable() && g_state.scene.Ready(); }

std::optional<std::pair<size_t, size_t>> LightingPipelineProgress()
{
    if (!g_state.lighting || !g_state.lighting->PreparingPipelines())
        return std::nullopt;
    return g_state.lighting->PipelineProgress();
}

bool LevelHasWater() { return Ready() && !g_state.scene.WaterBodies().empty(); }

float SkyboxScale() { return Ready() ? g_state.scene.Sun().skyCameraScale : 0.0f; }

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
    loading::TryInstall();
    engine::TryInitialize();
    engine::CapUnfocusedFrames(!GetSettings().backgroundFullSpeed);
    // Addons that make their own lights (Better Lights) read gmodpp_lights to step aside while the mod lights effects.
    if (g_state.frame % kMapPollInterval == 0) {
        const Settings& settings = GetSettings();
        engine::PublishLightsActive(g_state.lighting && g_state.lighting->Usable() && settings.realtimeLighting && settings.effectLights);
    }
    light_queries::TryInstall();
    entities::TryInitialize();
    if (g_state.frame % kMapPollInterval == 0)
        config::Sync(engine::LevelName());
    if (g_state.lighting && g_state.frame % kMapPollInterval == 0 && !g_state.preparing && !loading::Active()) {
        UpdateScene();
        RunConsoleRequests(consoleRequests);
    }
}

void BeginLevelLoad()
{
    if (!g_state.lighting || g_state.preparing || g_state.loader.joinable())
        return;
    engine::TryInitialize();
    const std::optional<std::string> level = PendingLevel();
    if (!level)
        return;
    g_state.loaderDone = 0.0f;
    g_state.loaderFinished = false;
    g_state.loaderStart = std::chrono::steady_clock::now();
    g_state.loader = std::jthread([level = *level] {
        LoadScene(level, [](float done) { g_state.loaderDone = done; });
        g_state.loaderEnd = std::chrono::steady_clock::now();
        g_state.loaderFinished = true;
    });
}

void PrepareLevel(const LoadProgress& progress)
{
    if (!g_state.lighting || g_state.preparing)
        return;
    engine::TryInitialize();
    g_state.preparing = true;
    config::Sync(engine::LevelName());
    // The scene's share of the bar; with baked indirect light, the bake of a map not cached yet fills the rest.
    const bool bake = GetSettings().indirectLighting == IndirectLighting::Baked;
    const float sceneShare = bake ? 0.7f : 1.0f;
    const auto start = std::chrono::steady_clock::now();
    const bool background = g_state.loader.joinable();
    while (LoaderBusy()) {
        progress(g_state.loaderDone * sceneShare, LoadStage::Scene);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    JoinLoader();
    if (const std::optional<std::string> level = PendingLevel())
        LoadScene(*level, [&](float done) { progress(done * sceneShare, LoadStage::Scene); });
    // The lighting's pipeline states have been created in the background since the game started (Lighting::Initialize);
    // the first map loaded soon after a launch waits for the rest here instead of in its first frame. Without a scene the
    // map is not lit, and nothing waits.
    bool waitedForPipelines = false;
    while (g_state.scene.Ready() && g_state.lighting->PreparingPipelines()) {
        waitedForPipelines = true;
        const auto [created, total] = g_state.lighting->PipelineProgress();
        progress(total != 0 ? static_cast<float>(created) / static_cast<float>(total) : 0.0f, LoadStage::Pipelines);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    const auto loaded = std::chrono::steady_clock::now();
    // After the pipeline states' bar the bake fills a bar of its own, rather than going back to the scene's share.
    const float bakeStart = waitedForPipelines ? 0.0f : sceneShare;
    if (bake && g_state.scene.Ready())
        g_state.lighting->Bake(g_state.scene, [&](float done) { progress(bakeStart + done * (1.0f - bakeStart), LoadStage::Bake); });
    const auto ms = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
    log::Info("Loading screen: '{}' ready when the loading screen closes (scene loaded {}, {:.0f} ms waited for it; bake {:.0f} ms)", g_state.scene.Name(),
        background ? std::format("in the background in {:.0f} ms", ms(g_state.loaderStart, g_state.loaderEnd)) : std::string("then"), ms(start, loaded),
        ms(loaded, std::chrono::steady_clock::now()));
    g_state.preparing = false;
}

std::array<float, 2> ViewJitter(UINT width, UINT height)
{
    // A Halton(2, 3) sub-pixel pattern, in NDC units (y is up in NDC).
    g_state.pixelOffset[0] = g_state.pixelOffset[1] = 0.0f;
    UINT renderWidth = 0;
    UINT renderHeight = 0;
    if (g_state.lighting && g_state.lighting->RenderSize(width, height, renderWidth, renderHeight) != Lighting::Upscaler::None) {
        const uint32_t sample = static_cast<uint32_t>(g_state.frame % Lighting::JitterPhases(width, renderWidth)) + 1;
        g_state.pixelOffset[0] = Halton(sample, 2) - 0.5f;
        g_state.pixelOffset[1] = Halton(sample, 3) - 0.5f;
        // DLSS and FSR take the image shift themselves, as NVIDIA's Donut samples pass it (PlanarView pixel offset).
        return {g_state.pixelOffset[0] * 2.0f / static_cast<float>(renderWidth), -g_state.pixelOffset[1] * 2.0f / static_cast<float>(renderHeight)};
    }
    if ((g_state.lighting ? g_state.lighting->AntiAliasingMode() : GetSettings().antiAliasing) == AntiAliasing::Temporal) {
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
        BuildFrameLights(EyePosition(inputs.viewProj).value_or(std::array<float, 3>{}));
        inputs.localPlayer = g_state.localPlayer;
        g_state.envSun = entities::FindSun();
        break;
    case gbuffer::ViewKind::Skybox:
        g_state.dynamicInstances.clear();
        g_state.skinnedEntities.clear();
        g_state.dynamicLights.clear();
        g_state.frameLights.clear();
        g_state.frameExactFirst = g_state.frameFlashFirst = 0;
        g_state.envSun = entities::FindSun();
        break;
    case gbuffer::ViewKind::Viewmodel:
        // Same scene as the main view; characters are not skinned a second time.
        break;
    }
    g_state.scene.SetDynamicLights(g_state.frameLights, g_state.frameExactFirst, g_state.frameFlashFirst, inputs.kind == gbuffer::ViewKind::Main);
    if (std::array<float, engine::kLightStyles> styles; engine::LightStyleValues(styles))
        g_state.scene.SetLightStyles(styles);
    inputs.envSun = g_state.envSun;
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
        if (probe->water)
            log::Info("Probe: water slope drawn ({:.3f} {:.3f}, eye angle {:.3f}), sampled by shaders ({:.3f} {:.3f}, eye angle by the texture axes {:.3f})", probe->drawnWaterNormal[0],
                probe->drawnWaterNormal[1], probe->drawnWaterNormal[2], probe->rebuiltWaterNormal[0], probe->rebuiltWaterNormal[1],
                probe->rebuiltWaterNormal[2]);
    }
}

std::optional<PostView> CurrentPostView()
{
    if (!g_state.lighting)
        return std::nullopt;
    const std::optional<PostView>& post = g_state.lighting->LastPostView();
    if (!post || post->frame != g_state.frame)
        return std::nullopt;
    return post;
}

std::optional<gbuffer::SkyDraw> SkyDraw()
{
    return g_state.lighting ? std::optional(g_state.lighting->SkyDraw()) : std::nullopt;
}

std::optional<gbuffer::ForwardConstants> ForwardLighting(const ViewInputs& inputs)
{
    if (inputs.kind != gbuffer::ViewKind::Main || !g_state.lighting->VolumeReady())
        return std::nullopt;
    gbuffer::ForwardConstants forward = {};
    if (const std::optional<std::array<float, 3>> eye = EyePosition(inputs.viewProj))
        forward.eye = *eye;
    forward.tonemapScale = inputs.tonemapScale;
    forward.probeOrigin = g_state.lighting->VolumeOrigin();
    forward.fogDaylight = g_state.lighting->FogDaylight();
    if (GetSettings().fog) {
        forward.fog = inputs.fog;
        forward.litFogScale = g_state.lighting->LitFogScale();
    }
    forward.lightOnly = IsLightingDebugView(GetSettings().debugView);
    forward.twinFrame = g_state.lighting->ForwardFrame();
    forward.twinScene = g_state.lighting->ForwardScene();
    forward.fogFrame = g_state.lighting->ForwardFrame();
    return forward;
}

ID3D12Fence* Fence() { return g_state.lighting->Fence(); }

uint64_t FenceValue() { return g_state.lighting->FenceValue(); }

}
