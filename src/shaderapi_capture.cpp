#include "shaderapi_capture.h"

#include "engine_interfaces.h"
#include "log.h"
#include "shader_bytecode.h"
#include "shaderapi.h"
#include "shaderapi_device.h"
#include "shaderapi_meshes.h"
#include "shaderapi_textures.h"
#include "shader_twins.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace gmodpp::native::capture {

namespace {

using gbuffer::ShaderKind;
using gbuffer::ViewKind;

// Albedo and self-illumination are linear; FP16 keeps dark albedo precise. The light target has the frame's format, so
// additive passes write it as they would the frame.
enum TargetIndex { kAlbedo, kNormal, kEmissive, kFlags, kLight, kTargetCount };
constexpr DXGI_FORMAT kTargetFormats[kTargetCount] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_B8G8R8A8_UNORM, Backend::kFrameFormat};
constexpr size_t kMaxProjectedLights = 8;
// FrameConstants::lightWarpViews in src/lighting.h.
constexpr size_t kMaxLightWarps = 64;
// ShaderBlendFactor_t (shaderapi/ishaderdynamic.h): SHADER_BLEND_ONE, SHADER_BLEND_DST_COLOR, SHADER_BLEND_SRC_ALPHA,
// SHADER_BLEND_ONE_MINUS_SRC_ALPHA and SHADER_BLEND_SRC_COLOR.
constexpr int kBlendOne = 1;
constexpr int kBlendDestinationColor = 2;
constexpr int kBlendSourceAlpha = 4;
constexpr int kBlendOneMinusSourceAlpha = 5;
constexpr int kBlendSourceColor = 9;
// Main views captured before the coverage report is logged; the map's materials have been drawn by then.
constexpr uint64_t kCoverageViews = 600;

// How the capture handled a material shader's pixel shader file (Phase 3's coverage report).
enum class Path { Preview, Twin, Water, Unlit, Decal, Additive, Flashlight, ProjectedShadow, Forward, ForwardTwin, ForwardUnlit, Count };
constexpr const char* kPathNames[] = {"G-buffer (lighting preview)", "G-buffer (twin)", "G-buffer (water surface)", "game shader, unlit",
    "albedo decal", "additive light", "flashlight (ray traced)", "projected shadow (skipped)", "forward lit", "forward twin", "translucent, game shader"};
static_assert(std::size(kPathNames) == static_cast<size_t>(Path::Count));

struct Target {
    TrackedResource resource;
    D3D12_CPU_DESCRIPTOR_HANDLE view = {};
};

struct SkyCube {
    TrackedResource resource;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 6> faces = {};
    UINT cubeView = DescriptorHeap::kInvalid;
    UINT facesView = DescriptorHeap::kInvalid;
    // The map whose sky it holds.
    std::string level;
    bool failed = false;
};

struct PixelShader {
    gbuffer::PixelShaderClass shaderClass;
    std::string name;
    std::string forwardName;
};

// A projected light with the texture handle of its cookie (0 for none).
struct Projected {
    gbuffer::ProjectedLight light;
    std::intptr_t cookie = 0;
};

struct State {
    std::array<Target, kTargetCount> targets;
    UINT width = 0;
    UINT height = 0;
    // What lighting copies the main view's probe volume into, for the forward-lit draws (red, green, blue).
    std::array<TrackedResource, 3> probes;
    std::array<UINT, 3> probeViews = {DescriptorHeap::kInvalid, DescriptorHeap::kInvalid, DescriptorHeap::kInvalid};
    bool probesFailed = false;
    SkyCube sky;
    bool active = false;
    bool forwardActive = false;
    // G-buffer draws since Begin: a viewmodel capture without any is not lit (End).
    uint32_t gbufferDraws = 0;
    // The forward pass's draws take the last capture's jitter (the 3D skybox's translucent ones).
    bool forwardJitter = false;
    // The forward pass's draws read openAirProbes, one texel each that holds the light of open air (BeginForward).
    bool forwardOpenAir = false;
    std::array<TrackedResource, 3> openAirProbes;
    std::array<UINT, 3> openAirViews = {DescriptorHeap::kInvalid, DescriptorHeap::kInvalid, DescriptorHeap::kInvalid};
    // SetDetailSprites: the material's name and whether each material drawn since it was set is it.
    std::string detailSprites;
    std::unordered_map<void*, bool> detailSpriteMaterials;
    bool skipView = false;
    std::array<float, 2> jitter = {};
    gbuffer::Matrix4 viewProj = {};
    // The fog constants of the pass's last lit draw (End): the sky faces drawn after the terrain leave c29 white.
    bool litFogSeen = false;
    std::array<float, 4> litFogRange = {};
    std::array<float, 3> litFogColor = {};
    std::array<std::array<float, 4>, bytecode::kForwardUniforms> forwardConstants = {};
    uint32_t forwardDraws = 0;
    uint32_t forwardTwinDraws = 0;
    D3D12_GPU_VIRTUAL_ADDRESS twinFrame = 0;
    D3D12_GPU_VIRTUAL_ADDRESS twinScene = 0;
    D3D12_GPU_VIRTUAL_ADDRESS fogFrame = 0;
    uint32_t forwardLogged = 0;
    // Fog of the main view and of the 3D skybox, which has its own.
    std::array<gbuffer::Fog, 2> fog;
    std::optional<std::array<float, 3>> waterFogColor;
    // The water surfaces the game drew (the last draw of each, by fog colour) and the texture handle of each one's
    // normal map (Water's NormalSampler, s4).
    std::vector<gbuffer::WaterSurfaceDraw> waterDraws;
    std::vector<std::intptr_t> waterNormalHandles;
    // The waterDraws entry of the water pass being drawn.
    int lastWaterDraw = -1;
    bool loggedWaterMapping = false;
    std::vector<ID3D12Resource*> waterNormalMaps;
    gbuffer::SwayClock swayClock;
    std::vector<Projected> projected;
    std::vector<Projected> lastProjected;
    // The view's projected lights and their cookies, for ViewInputs.
    std::vector<gbuffer::ProjectedLight> lights;
    std::vector<ID3D12Resource*> cookies;
    // The light warp textures of the view's G-buffer twins, by slot - 1 (LightWarpSlot).
    std::vector<UINT> lightWarps;
    uint32_t flashlightDraws = 0;
    std::pair<uint32_t, size_t> loggedProjection = {0, 0};
    uint64_t sections = 0;
    bool loggedOtherTarget = false;
    bool loggedCounts = false;
    // Draws per material shader, pixel shader file and path, for the coverage report. Both names are static strings:
    // the IShader's name and the shader file's.
    std::map<std::tuple<const char*, const char*, Path>, uint64_t> coverage;
    uint64_t mainViews = 0;
    bool coverageLogged = false;
    // By the address of the game's combo, which never moves once loaded.
    std::unordered_map<const DWORD*, std::unique_ptr<PixelShader>> pixelShaders;
    std::unordered_map<const DWORD*, gbuffer::VertexShaderClass> vertexShaders;
    // What the 2D sky's draws show (SetSky), the sun the painted sky last drew, and each sky pixel shader's g_SunNormal
    // register (-1 for none), by its combo's address.
    std::optional<gbuffer::SkyDraw> skyDraw;
    std::optional<std::array<float, 3>> paintedSun;
    std::unordered_map<const DWORD*, int> sunNormalRegisters;
};

State g_state;

const Float4& PixelRegister(UINT reg) { return Dynamic().pixelFloats[std::min<size_t>(reg, kPixelFloatConstants - 1)]; }
const Float4& VertexRegister(UINT reg) { return Dynamic().vertexFloats[std::min<size_t>(reg, kVertexFloatConstants - 1)]; }

const PixelShader& PixelShaderFor(std::span<const DWORD> code)
{
    std::unique_ptr<PixelShader>& entry = g_state.pixelShaders[code.data()];
    if (!entry) {
        entry = std::make_unique<PixelShader>();
        entry->shaderClass = gbuffer::ClassifyPixelShader(code, kPixelFloatConstants);
        const auto id = reinterpret_cast<std::uintptr_t>(code.data());
        entry->name = std::format("gbuffer_{:x}", id);
        entry->forwardName = std::format("forward_{:x}", id);
    }
    return *entry;
}

const gbuffer::VertexShaderClass& VertexShaderFor(std::span<const DWORD> code)
{
    auto found = g_state.vertexShaders.find(code.data());
    if (found == g_state.vertexShaders.end())
        found = g_state.vertexShaders.emplace(code.data(), gbuffer::ClassifyVertexShader(code, kVertexFloatConstants)).first;
    return found->second;
}

bool CreateTargets()
{
    Backend& backend = GetBackend();
    const UINT width = backend.Width();
    const UINT height = backend.Height();
    if (g_state.targets[0].resource.resource && width == g_state.width && height == g_state.height)
        return true;
    Release();
    for (size_t i = 0; i < kTargetCount; ++i) {
        Target& target = g_state.targets[i];
        const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kTargetFormats[i];
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&target.resource.resource));
        if (FAILED(hr)) {
            log::Info("G-buffer: creating {}x{} targets failed ({:#010x})", width, height, static_cast<unsigned>(hr));
            Release();
            return false;
        }
        target.resource.state = D3D12_RESOURCE_STATE_COMMON;
        target.view = backend.RenderTargetViews().Allocate();
        D3D12_RENDER_TARGET_VIEW_DESC view = {};
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        view.Format = kTargetFormats[i];
        backend.Device()->CreateRenderTargetView(target.resource.resource.Get(), &view, target.view);
    }
    g_state.width = width;
    g_state.height = height;
    log::Info("G-buffer: {}x{} targets ready", width, height);
    return true;
}

// The textures the forward-lit draws read the probe volume from, in the format lighting fills them in.
void CreateProbeTextures()
{
    if (g_state.probes[0].resource || g_state.probesFailed)
        return;
    Backend& backend = GetBackend();
    for (size_t i = 0; i < g_state.probes.size(); ++i) {
        const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        desc.Width = gbuffer::kProbeGrid[0];
        desc.Height = gbuffer::kProbeGrid[1];
        desc.DepthOrArraySize = static_cast<UINT16>(gbuffer::kProbeGrid[2]);
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        if (FAILED(backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&g_state.probes[i].resource)))) {
            log::Info("G-buffer: creating the probe volume textures failed; translucent surfaces keep their baked light");
            g_state.probes = {};
            g_state.probesFailed = true;
            return;
        }
        g_state.probes[i].state = D3D12_RESOURCE_STATE_COMMON;
        g_state.probeViews[i] = backend.Descriptors().Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = desc.Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture3D.MipLevels = 1;
        backend.Device()->CreateShaderResourceView(g_state.probes[i].resource.Get(), &view, backend.Descriptors().Cpu(g_state.probeViews[i]));
    }
}

// One texel per colour channel in the probe textures' form, read alike wherever the forward snippet looks
// (ForwardConstants::openAirLight), copied in on the game's command list before the draws that read it.
bool UploadOpenAirLight(const std::array<std::array<float, 4>, 3>& light)
{
    Backend& backend = GetBackend();
    for (size_t i = 0; i < g_state.openAirProbes.size(); ++i) {
        if (!g_state.openAirProbes[i].resource) {
            const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            desc.Width = 1;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            desc.SampleDesc.Count = 1;
            if (FAILED(backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                    IID_PPV_ARGS(&g_state.openAirProbes[i].resource))))
                return false;
            g_state.openAirProbes[i].state = D3D12_RESOURCE_STATE_COMMON;
            g_state.openAirViews[i] = backend.Descriptors().Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
            view.Format = desc.Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture3D.MipLevels = 1;
            backend.Device()->CreateShaderResourceView(g_state.openAirProbes[i].resource.Get(), &view, backend.Descriptors().Cpu(g_state.openAirViews[i]));
        }
        const Backend::Upload upload = backend.AllocateUpload(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (upload.data == nullptr)
            return false;
        std::memcpy(upload.data, light[i].data(), sizeof(light[i]));
        backend.Transition(g_state.openAirProbes[i], D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to = {g_state.openAirProbes[i].resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        to.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION from = {upload.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        from.PlacedFootprint = {upload.offset, {DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
        backend.Commands()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    return true;
}

bool CreateSkyCube()
{
    SkyCube& sky = g_state.sky;
    if (sky.resource.resource || sky.failed)
        return !sky.failed;
    Backend& backend = GetBackend();
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kSkyCubeSize;
    desc.Height = kSkyCubeSize;
    desc.DepthOrArraySize = 6;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    sky.cubeView = backend.Descriptors().Allocate();
    sky.facesView = backend.Descriptors().Allocate();
    if (sky.cubeView == DescriptorHeap::kInvalid || sky.facesView == DescriptorHeap::kInvalid
        || FAILED(backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&sky.resource.resource)))) {
        log::Info("G-buffer: creating the sky cube failed; reflections and indirect light take the map's flat sky light");
        sky.resource = {};
        sky.failed = true;
        return false;
    }
    sky.resource.state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = desc.Format;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    view.TextureCube.MipLevels = 1;
    backend.Device()->CreateShaderResourceView(sky.resource.resource.Get(), &view, backend.Descriptors().Cpu(sky.cubeView));
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray = {};
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.ArraySize = 6;
    backend.Device()->CreateShaderResourceView(sky.resource.resource.Get(), &view, backend.Descriptors().Cpu(sky.facesView));
    for (UINT face = 0; face < 6; ++face) {
        sky.faces[face] = backend.RenderTargetViews().Allocate();
        D3D12_RENDER_TARGET_VIEW_DESC target = {};
        target.Format = desc.Format;
        target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        target.Texture2DArray.FirstArraySlice = face;
        target.Texture2DArray.ArraySize = 1;
        backend.Device()->CreateRenderTargetView(sky.resource.resource.Get(), &target, sky.faces[face]);
    }
    log::Info("G-buffer: {}x{} sky cube ready", kSkyCubeSize, kSkyCubeSize);
    return true;
}

// Rebuilds the projected light a flashlight pass is drawing from the constants and textures bound for it.
void CaptureProjectedLight(const gbuffer::FlashlightRegisters& ps, const gbuffer::FlashlightRegisters& vs, std::uint32_t srgbRead)
{
    ++g_state.flashlightDraws;
    if (ps.attenuation < 0 || ps.color < 0)
        return;
    const float* origin = ps.position >= 0 ? PixelRegister(static_cast<UINT>(ps.position)).v
        : vs.position >= 0                 ? VertexRegister(static_cast<UINT>(vs.position)).v
                                           : nullptr;
    if (origin == nullptr || (ps.worldToTexture < 0 && vs.worldToTexture < 0))
        return;
    Projected projected;
    gbuffer::ProjectedLight& light = projected.light;
    for (int i = 0; i < 4; ++i) {
        const float* row = ps.worldToTexture >= 0 ? PixelRegister(static_cast<UINT>(ps.worldToTexture + i)).v : VertexRegister(static_cast<UINT>(vs.worldToTexture + i)).v;
        std::copy_n(row, 4, light.worldToTexture[i].begin());
    }
    const float* attenuation = PixelRegister(static_cast<UINT>(ps.attenuation)).v;
    const float* color = PixelRegister(static_cast<UINT>(ps.color)).v;
    light.origin = {origin[0], origin[1], origin[2]};
    light.attenuation = {attenuation[0], attenuation[1], attenuation[2]};
    light.farZ = attenuation[3];
    light.color = {color[0], color[1], color[2]};
    if (ps.cookie >= 0 && ps.cookie < kSamplers) {
        projected.cookie = Dynamic().textures[static_cast<size_t>(ps.cookie)];
        light.cookieSrgb = ((srgbRead >> ps.cookie) & 1) != 0;
    }
    for (const Projected& existing : g_state.projected) {
        if (existing.light.origin == light.origin && existing.light.worldToTexture == light.worldToTexture)
            return;
    }
    if (g_state.projected.size() < kMaxProjectedLights)
        g_state.projected.push_back(projected);
}

// Shifting clip x/y by jitter * w moves the image by a constant NDC offset: row0 += jx * row3, row1 += jy * row3.
void AddJitter(std::vector<RegisterPatch>& patches)
{
    if (g_state.jitter[0] == 0.0f && g_state.jitter[1] == 0.0f)
        return;
    for (UINT base : {gbuffer::kModelViewProjection, gbuffer::kViewProjection}) {
        const float* row3 = VertexRegister(base + 3).v;
        for (UINT axis = 0; axis < 2; ++axis) {
            const float* row = VertexRegister(base + axis).v;
            RegisterPatch& patch = patches.emplace_back();
            patch.reg = base + axis;
            for (int c = 0; c < 4; ++c)
                patch.value[static_cast<size_t>(c)] = row[c] + g_state.jitter[axis] * row3[c];
        }
    }
}

void Count(const char* pixelFile, Path path)
{
    auto [entry, added] = g_state.coverage.try_emplace({BoundShaderName(), pixelFile != nullptr ? pixelFile : "none", path}, 0);
    ++entry->second;
    if (added && g_state.coverageLogged)
        log::Info("Coverage: {} / {}: {}", std::get<0>(entry->first), std::get<1>(entry->first), kPathNames[static_cast<size_t>(path)]);
}

// Once per session: every material shader drawn in the captured views, its pixel shader files and the path each took.
// New combinations are logged as they appear afterwards.
void LogCoverage()
{
    g_state.coverageLogged = true;
    std::map<std::tuple<std::string, std::string, Path>, uint64_t> sorted;
    for (const auto& [key, draws] : g_state.coverage)
        sorted[{std::get<0>(key), std::get<1>(key), std::get<2>(key)}] += draws;
    log::Info("Coverage report after {} main views (material shader / pixel shader file: path, draws):", g_state.mainViews);
    for (const auto& [key, draws] : sorted)
        log::Info("  {} / {}: {}, {}", std::get<0>(key), std::get<1>(key), kPathNames[static_cast<size_t>(std::get<2>(key))], draws);
}

}

bool Begin(std::array<float, 2> jitter)
{
    if (g_state.active)
        return true;
    if (!CreateTargets())
        return false;
    CreateProbeTextures();
    Backend& backend = GetBackend();
    constexpr float kZero[4] = {};
    for (size_t i = 0; i < kTargetCount; ++i) {
        backend.Transition(g_state.targets[i].resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
        backend.Commands()->ClearRenderTargetView(g_state.targets[i].view, kZero, 0, nullptr);
    }
    g_state.jitter = jitter;
    g_state.lightWarps.clear();
    g_state.waterFogColor.reset();
    g_state.litFogSeen = false;
    g_state.projected.clear();
    g_state.active = true;
    g_state.gbufferDraws = 0;
    ++g_state.sections;
    if (!g_state.loggedCounts) {
        g_state.loggedCounts = true;
        log::Info("G-buffer: {} preview shaders patched, {} model shaders replaced so far", gbuffer::ShaderStatistics().patched,
            gbuffer::ShaderStatistics().replaced);
    }
    return true;
}

bool Active() { return g_state.active; }

bool End(ViewKind kind, ViewInputs& inputs)
{
    if (!g_state.active)
        return false;
    g_state.active = false;
    // ARC9 copies the screen and changes the depth range in its viewmodel hooks, which end captures with nothing in
    // them; lit, each took a run, and the viewmodel's own no longer followed the main view's (Lighting::Run).
    if (kind == ViewKind::Viewmodel && g_state.gbufferDraws == 0) {
        g_state.flashlightDraws = 0;
        g_state.projected.clear();
        return false;
    }
    if (kind != ViewKind::Viewmodel) {
        // Every lit shader's vertex fog constant holds the view's range fog (cFogParams: end / range, 1,
        // 1 - max density, 1 / range) and pixel c29 its colour; Source writes that constant only when fog is on, so
        // the fog mode says whether it belongs to this view. They are taken at the pass's last lit draw: the 2D sky's
        // faces, drawn after the terrain, left c29 white, and the 3D skybox turned white when the view looked up
        // enough for them to come last (mm_trickery, 2026-11-03).
        const float* range = g_state.litFogSeen ? g_state.litFogRange.data() : VertexRegister(gbuffer::kVertexFogRegister).v;
        const float* color = g_state.litFogSeen ? g_state.litFogColor.data() : PixelRegister(gbuffer::kFogColorRegister).v;
        gbuffer::Fog& fog = g_state.fog[kind == ViewKind::Skybox ? 1 : 0];
        const std::optional<engine::FogState> current = engine::CurrentFog();
        fog.params = current && current->mode != 0 ? std::array<float, 4>{range[0] - 1.0f, 0.0f, 1.0f - range[2], range[3]} : std::array<float, 4>{};
        fog.color = {color[0], color[1], color[2]};
        // Under water the view's fog is the water's, blended linearly.
        if (current && current->mode != 0 && g_state.waterFogColor) {
            bool same = true;
            for (int c = 0; c < 3; ++c)
                same = same && std::fabs(std::pow(current->color[static_cast<size_t>(c)] / 255.0f, 2.2f) - (*g_state.waterFogColor)[static_cast<size_t>(c)]) < 0.01f;
            if (same)
                fog.params[1] = 1.0f;
        }
        fog.known = true;
    }
    if (kind == ViewKind::Main) {
        const std::pair<uint32_t, size_t> projection = {std::min<uint32_t>(g_state.flashlightDraws, 1), g_state.projected.size()};
        if (projection != g_state.loggedProjection) {
            g_state.loggedProjection = projection;
            log::Info("G-buffer: {} projected light(s) from {} flashlight draw(s)", g_state.projected.size(), g_state.flashlightDraws);
        }
        g_state.lastProjected = std::move(g_state.projected);
        if (++g_state.mainViews == kCoverageViews)
            LogCoverage();
    }
    g_state.flashlightDraws = 0;
    g_state.projected.clear();

    Backend& backend = GetBackend();
    auto common = [&](TrackedResource& resource) {
        backend.Transition(resource, D3D12_RESOURCE_STATE_COMMON);
        return resource.resource.Get();
    };
    inputs.color = common(backend.FrameBuffer());
    inputs.depth = common(backend.DepthBuffer());
    inputs.albedo = common(g_state.targets[kAlbedo].resource);
    inputs.normal = common(g_state.targets[kNormal].resource);
    inputs.emissive = common(g_state.targets[kEmissive].resource);
    inputs.flags = common(g_state.targets[kFlags].resource);
    inputs.light = common(g_state.targets[kLight].resource);
    // A cookie deleted since its light was captured leaves a round spot.
    g_state.lights.clear();
    g_state.cookies.clear();
    for (const Projected& projected : g_state.lastProjected) {
        g_state.lights.push_back(projected.light);
        Texture* texture = TextureFromHandle(projected.cookie);
        g_state.cookies.push_back(texture != nullptr && !texture->copies.empty() ? common(texture->Current().resource) : nullptr);
    }
    inputs.projected = g_state.lights;
    inputs.cookies = g_state.cookies;
    g_state.waterNormalMaps.clear();
    for (const std::intptr_t handle : g_state.waterNormalHandles) {
        Texture* texture = TextureFromHandle(handle);
        g_state.waterNormalMaps.push_back(texture != nullptr && !texture->copies.empty() ? common(texture->Current().resource) : nullptr);
    }
    inputs.waterDraws = g_state.waterDraws;
    inputs.waterNormalMaps = g_state.waterNormalMaps;
    inputs.lightWarps = g_state.lightWarps;
    if (kind == ViewKind::Main && g_state.probes[0].resource) {
        for (size_t i = 0; i < g_state.probes.size(); ++i)
            inputs.probes[i] = common(g_state.probes[i]);
    }
    if (g_state.sky.resource.resource && g_state.sky.level == engine::LevelName()) {
        inputs.skyCube = common(g_state.sky.resource);
        inputs.skyCubeView = g_state.sky.cubeView;
        inputs.skyFacesView = g_state.sky.facesView;
    }
    inputs.skyboxFog = g_state.fog[1];
    inputs.kind = kind;
    inputs.viewProj = g_state.viewProj;
    inputs.jitter = g_state.jitter;
    const float scale = PixelRegister(gbuffer::kLightScaleRegister).v[0];
    inputs.tonemapScale = scale > 0.0f ? scale : 1.0f;
    inputs.displayPeak = GetBackend().HudlessMapping().peak;
    inputs.fog = g_state.fog[kind == ViewKind::Skybox ? 1 : 0];
    inputs.sway = g_state.swayClock;
    inputs.paintedSun = g_state.paintedSun;
    return true;
}

void BeginForward(const gbuffer::ForwardConstants& constants, bool mainView)
{
    if (!g_state.probes[0].resource)
        return;
    using gbuffer::kProbeGrid;
    using gbuffer::kProbeSpacing;
    const float extent[3] = {kProbeSpacing * kProbeGrid[0], kProbeSpacing * kProbeGrid[1], kProbeSpacing * kProbeGrid[2]};
    auto& c = g_state.forwardConstants;
    c[1] = {0.0f, 0.0f, 0.0f, constants.lightOnly ? 1.0f : 0.0f};
    c[2] = {constants.fog.color[0] * constants.fogDaylight[0], constants.fog.color[1] * constants.fogDaylight[1], constants.fog.color[2] * constants.fogDaylight[2],
        constants.fog.known ? constants.litFogScale : 0.0f};
    c[3] = {1.0f / extent[0], 1.0f / extent[1], 1.0f / extent[2], kProbeSpacing};
    // The window ends at the centres of its outermost probes.
    c[0] = {0.0f, 2.0f / (extent[0] - kProbeSpacing), 2.0f / (extent[1] - kProbeSpacing), 2.0f / (extent[2] - kProbeSpacing)};
    c[4] = {(constants.probeOrigin[0] + kProbeGrid[0] * 0.5f) * kProbeSpacing, (constants.probeOrigin[1] + kProbeGrid[1] * 0.5f) * kProbeSpacing,
        (constants.probeOrigin[2] + kProbeGrid[2] * 0.5f) * kProbeSpacing, 0.0f};
    c[5] = {constants.eye[0], constants.eye[1], constants.eye[2], constants.tonemapScale};
    c[6] = constants.fog.known ? constants.fog.params : std::array<float, 4>{};
    g_state.twinFrame = constants.twinFrame;
    g_state.twinScene = constants.twinScene;
    g_state.fogFrame = mainView ? constants.fogFrame : 0;
    g_state.forwardJitter = constants.jittered;
    g_state.forwardOpenAir = constants.openAir && UploadOpenAirLight(constants.openAirLight);
    g_state.forwardActive = true;
    g_state.forwardDraws = 0;
    g_state.forwardTwinDraws = 0;
}

bool ForwardActive() { return g_state.forwardActive; }

void SkipView(bool skip) { g_state.skipView = skip; }

void EndForward()
{
    g_state.skipView = false;
    if (!g_state.forwardActive)
        return;
    g_state.forwardActive = false;
    g_state.forwardOpenAir = false;
    if (g_state.forwardLogged < 3) {
        ++g_state.forwardLogged;
        log::Info("G-buffer: {} forward-lit draws, {} of them by forward twins ({} preview shaders have a forward variant, {} could not take one)",
            g_state.forwardDraws, g_state.forwardTwinDraws, gbuffer::ShaderStatistics().forward, gbuffer::ShaderStatistics().forwardFailures);
    }
}

bool IsDetailSprites(void* material)
{
    if (g_state.detailSprites.empty())
        return false;
    auto [entry, added] = g_state.detailSpriteMaterials.try_emplace(material, false);
    if (added) {
        std::string name = engine::MaterialName(material);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return c == '\\' ? '/' : static_cast<char>(std::tolower(c)); });
        entry->second = name == g_state.detailSprites;
    }
    return entry->second;
}

void SetDetailSprites(std::string_view material)
{
    if (material == g_state.detailSprites)
        return;
    g_state.detailSprites = material;
    g_state.detailSpriteMaterials.clear();
}

int FixedLighting()
{
    // Unlit materials keep the game's own shaders where they are no G-buffer surface: their preview combos leave out
    // vertex colour (see engine_interfaces.cpp). During a capture those are the ones that blend: ARC9 darkens the screen
    // behind its customization menu with a 2D rect drawn in the viewmodel's pass, which came out white.
    if (g_state.active) {
        void* material = BoundMaterial();
        return material != nullptr && engine::IsUnlitMaterial(material) && engine::IsTranslucentMaterial(material) ? 0 : 2;
    }
    if (g_state.forwardActive) {
        void* material = BoundMaterial();
        return material != nullptr && engine::IsUnlitMaterial(material) && !IsDetailSprites(material) ? 0 : 2;
    }
    return -1;
}

void RecordWaterMapping(const MeshDraw& mesh)
{
    const int entry = std::exchange(g_state.lastWaterDraw, -1);
    if (entry < 0 || static_cast<size_t>(entry) >= g_state.waterDraws.size())
        return;
    if (mesh.vertexData == nullptr || mesh.indexData == nullptr || mesh.texCoord0Offset < 0 || mesh.primitives.empty() || mesh.primitives[0].second < 3) {
        if (!std::exchange(g_state.loggedWaterMapping, true))
            log::Info("G-buffer: a water surface's texture mapping cannot be read (vertices {}, indices {}, texture coordinate at {}, {} primitive lists)",
                mesh.vertexData != nullptr, mesh.indexData != nullptr, mesh.texCoord0Offset, mesh.primitives.size());
        return;
    }
    // Water surfaces are horizontal: their coordinates are an affine function of x and y, solved from one triangle.
    const UINT stride = mesh.streams[0].StrideInBytes;
    float x[3], y[3], u[3], v[3];
    for (int k = 0; k < 3; ++k) {
        const std::uint16_t index = mesh.indexData[mesh.primitives[0].first + k];
        if (static_cast<int>(index) >= mesh.vertexCount)
            return;
        const std::uint8_t* vertex = mesh.vertexData + static_cast<size_t>(index) * stride;
        const float* position = reinterpret_cast<const float*>(vertex);
        const float* uv = reinterpret_cast<const float*>(vertex + mesh.texCoord0Offset);
        x[k] = position[0];
        y[k] = position[1];
        u[k] = uv[0];
        v[k] = uv[1];
    }
    const float ax = x[1] - x[0], ay = y[1] - y[0], bx = x[2] - x[0], by = y[2] - y[0];
    const float det = ax * by - ay * bx;
    if (std::fabs(det) < 1e-6f)
        return;
    const auto solve = [&](const float (&c)[3], std::array<float, 4>& row) {
        const float da = c[1] - c[0], db = c[2] - c[0];
        row[0] = (da * by - db * ay) / det;
        row[1] = (db * ax - da * bx) / det;
        row[2] = 0.0f;
        row[3] = c[0] - row[0] * x[0] - row[1] * y[0];
    };
    gbuffer::WaterSurfaceDraw& draw = g_state.waterDraws[static_cast<size_t>(entry)];
    solve(u, draw.uvS);
    solve(v, draw.uvT);
    if (!draw.mapped && !std::exchange(g_state.loggedWaterMapping, true))
        log::Info("G-buffer: a water surface's texture coordinates are ({:.5f} {:.5f}) . xy + {:.3f}, ({:.5f} {:.5f}) . xy + {:.3f}", draw.uvS[0], draw.uvS[1],
            draw.uvS[3], draw.uvT[0], draw.uvT[1], draw.uvT[3]);
    draw.mapped = true;
}

namespace {

bool ScreenSpaceDraw()
{
    for (UINT row = 0; row < 4; ++row) {
        const float* v = VertexRegister(4 + row).v;
        for (UINT column = 0; column < 4; ++column) {
            if (v[column] != (row == column ? 1.0f : 0.0f))
                return false;
        }
    }
    return true;
}

} // namespace

Pass Prepare(std::span<const DWORD> vertexCode, std::span<const DWORD> pixelCode, const char* pixelFile, const ComboId& pixelCombo, bool depthTest,
    bool blend, int sourceBlend, int destinationBlend, bool srgbWrite, std::uint32_t srgbRead)
{
    Pass pass;
    if (g_state.skipView) {
        pass.route = Pass::Route::Skip;
        return pass;
    }
    if (!g_state.active && !g_state.forwardActive)
        return pass;
    // Swaying props are drawn in the views' opaque passes, which are captured.
    const gbuffer::VertexShaderClass& vertex = VertexShaderFor(vertexCode);
    if (vertex.swayRegister >= 0) {
        const float* params = VertexRegister(static_cast<UINT>(vertex.swayRegister)).v;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_state.swayClock = {params[1], {params[2], params[3]}, now.QuadPart, true};
    }
    const PixelShader* pixel = pixelCode.empty() ? nullptr : &PixelShaderFor(pixelCode);
    const gbuffer::PixelShaderClass empty;
    const gbuffer::PixelShaderClass& shaderClass = pixel != nullptr ? pixel->shaderClass : empty;
    const bool additive = blend && destinationBlend == kBlendOne;

    if (g_state.forwardActive) {
        // Flashlights and projected textures are ray traced here too (the forward constants' projectors), so the game's
        // own passes for them are skipped, as in captured views.
        if (shaderClass.kind == ShaderKind::Flashlight || shaderClass.kind == ShaderKind::ProjectedShadow) {
            Count(pixelFile, shaderClass.kind == ShaderKind::Flashlight ? Path::Flashlight : Path::ProjectedShadow);
            pass.route = Pass::Route::Skip;
            return pass;
        }
        // Screen overlays (the underwater view's Refract warp) are drawn in clip space, with an identity
        // model-view-projection: they are no surface in the world to light.
        if (ScreenSpaceDraw()) {
            Count(pixelFile, Path::ForwardUnlit);
            return pass;
        }
        if (g_state.forwardJitter)
            AddJitter(pass.vertexPatches);
        pass.fogFrame = g_state.fogFrame;
        if (pixelFile != nullptr && twins::IsParticleShader(pixelFile)) {
            pass.fogEpilogue = true;
            // The medium's light is added where the draw writes linear light: with sRGB writes (without them its output is
            // decoded after the shader, Variant::decodeOutput).
            if (blend && srgbWrite)
                pass.fogBlend = destinationBlend == kBlendOne ? 3.0f
                    : destinationBlend != kBlendOneMinusSourceAlpha ? 0.0f
                    : sourceBlend == kBlendSourceAlpha ? 1.0f
                    : sourceBlend == kBlendOne ? 2.0f : 0.0f;
        }
        // Forward twins trace their own light; additive draws add light and are shown as drawn.
        // Refract's twin needs the game shader's registers it was written for.
        const bool refract = pixelFile != nullptr && std::string_view(pixelFile).starts_with("refract_");
        const std::span<const DWORD> twin = !additive && g_state.twinFrame != 0 && g_state.twinScene != 0 && pixelFile != nullptr && pixelCombo.header != nullptr &&
                (!refract || shaderClass.refractGlass)
            ? twins::ForwardShader(pixelFile, *pixelCombo.header, pixelCombo.staticIndex, pixelCombo.dynamicIndex)
            : std::span<const DWORD>();
        if (!twin.empty()) {
            Count(pixelFile, Path::ForwardTwin);
            ++g_state.forwardDraws;
            ++g_state.forwardTwinDraws;
            pass.pixelCode = twin;
            pass.pixelName = pixel->forwardName.c_str();
            pass.forwardFrame = g_state.twinFrame;
            pass.forwardScene = g_state.twinScene;
            return pass;
        }
        const std::span<const DWORD> forward = shaderClass.forward;
        if (forward.empty() || !g_state.probes[0].resource) {
            Count(pixelFile, Path::ForwardUnlit);
            return pass;
        }
        Count(pixelFile, Path::Forward);
        ++g_state.forwardDraws;
        pass.pixelCode = forward;
        pass.pixelName = pixel->forwardName.c_str();
        pass.probes = true;
        pass.probeSamplers = shaderClass.probeSamplers;
        // Additive draws (glows, sprites) add light; they are shown as drawn.
        auto constants = g_state.forwardConstants;
        constants[0][0] = additive ? 1.0f : 0.0f;
        for (size_t i = 0; i < constants.size(); ++i)
            pass.pixelPatches.push_back({shaderClass.forwardUniforms[i], constants[i]});
        return pass;
    }

    AddJitter(pass.vertexPatches);
    // Preview shaders write unlit materials as surfaces to be lit. Those drawn without a depth test ($ignorez, as
    // 2D skybox faces often are) stay the colour they draw.
    const bool gameTarget = Dynamic().colorTargets[0] == kDefaultTarget;
    const bool water = shaderClass.kind == ShaderKind::Water && depthTest && gameTarget;
    if (water) {
        const float* waterFog = PixelRegister(gbuffer::kWaterFogColorRegister).v;
        g_state.waterFogColor = std::array<float, 3>{waterFog[0], waterFog[1], waterFog[2]};
        if (vertex.bumpTransformRegister >= 0) {
            gbuffer::WaterSurfaceDraw draw;
            draw.fog = *g_state.waterFogColor;
            for (UINT row = 0; row < 2; ++row)
                std::copy_n(VertexRegister(static_cast<UINT>(vertex.bumpTransformRegister) + row).v, 4, draw.bumpRows[row].begin());
            draw.time = gbuffer::WaterClock();
            const std::intptr_t normalMap = Dynamic().textures[gbuffer::kWaterNormalSampler];
            const auto same = std::find_if(g_state.waterDraws.begin(), g_state.waterDraws.end(), [&](const gbuffer::WaterSurfaceDraw& d) { return d.fog == draw.fog; });
            if (same != g_state.waterDraws.end()) {
                draw.uvS = same->uvS;
                draw.uvT = same->uvT;
                draw.mapped = same->mapped;
                *same = draw;
                g_state.waterNormalHandles[static_cast<size_t>(same - g_state.waterDraws.begin())] = normalMap;
                g_state.lastWaterDraw = static_cast<int>(same - g_state.waterDraws.begin());
                pass.waterSurface = true;
            } else if (g_state.waterDraws.size() < gbuffer::kMaxWaterDraws) {
                g_state.waterDraws.push_back(draw);
                g_state.waterNormalHandles.push_back(normalMap);
                g_state.lastWaterDraw = static_cast<int>(g_state.waterDraws.size()) - 1;
                pass.waterSurface = true;
            }
        }
    }
    // Opaque shaders with a twin but no preview combo (eyes, teeth) are drawn into the G-buffer by their twin.
    const bool twinOnly = shaderClass.kind == ShaderKind::Other && !blend && pixelFile != nullptr && twins::HasTwin(pixelFile);
    const bool gbuffer = (shaderClass.kind == ShaderKind::GBuffer || water || twinOnly) && depthTest && gameTarget;
    if (shaderClass.kind == ShaderKind::GBuffer && !gameTarget && !g_state.loggedOtherTarget) {
        g_state.loggedOtherTarget = true;
        log::Info("G-buffer: a G-buffer draw went to another render target during a capture; it is drawn there unlit");
    }
    // Multiplying decals (DecalModulate's mod2x blend) darken the surface under them, so they multiply its albedo.
    const bool modulate = !gbuffer && shaderClass.kind == ShaderKind::Other && blend && depthTest && gameTarget &&
        sourceBlend == kBlendDestinationColor && destinationBlend == kBlendSourceColor;
    pass.route = gbuffer ? Pass::Route::GBuffer : modulate ? Pass::Route::Albedo : Pass::Route::Game;
    if (gbuffer && shaderClass.kind == ShaderKind::GBuffer) {
        std::copy_n(VertexRegister(gbuffer::kVertexFogRegister).v, 4, g_state.litFogRange.begin());
        std::copy_n(PixelRegister(gbuffer::kFogColorRegister).v, 3, g_state.litFogColor.begin());
        g_state.litFogSeen = true;
    }
    if (shaderClass.kind == ShaderKind::ProjectedShadow) {
        Count(pixelFile, Path::ProjectedShadow);
        pass.route = Pass::Route::Skip;
        return pass;
    }
    // Additive passes (emissive glows, sprites) add light rather than surface colour, so they go to the light target,
    // which is added after lighting without being lit again.
    if (shaderClass.kind == ShaderKind::Other && additive && (sourceBlend == kBlendOne || sourceBlend == kBlendSourceAlpha)) {
        pass.route = pixel != nullptr ? Pass::Route::Light : Pass::Route::Skip;
        Count(pixelFile, Path::Additive);
        return pass;
    }
    // Flashlight passes are lit by ray tracing from the light they describe.
    if (shaderClass.kind == ShaderKind::Flashlight) {
        CaptureProjectedLight(shaderClass.flashlight, vertex.flashlight, srgbRead);
        Count(pixelFile, Path::Flashlight);
        pass.route = Pass::Route::Skip;
        return pass;
    }
    if (!gbuffer) {
        if (gameTarget && pixel != nullptr)
            Count(pixelFile, modulate ? Path::Decal : Path::Unlit);
        pass.clearsCoverage = gameTarget && pixel != nullptr && depthTest && !blend;
        pass.marksMirror = pass.clearsCoverage && pixelFile != nullptr && std::string_view(pixelFile).starts_with("lightmappedreflective_ps");
        return pass;
    }
    const std::span<const DWORD> twin = !water && pixelFile != nullptr && pixelCombo.header != nullptr
        ? twins::GBufferShader(pixelFile, *pixelCombo.header, pixelCombo.staticIndex, pixelCombo.dynamicIndex)
        : std::span<const DWORD>();
    if (!water && twin.empty() && shaderClass.patched.empty()) {
        // A model shader whose twin is missing (its .vcs changed) keeps the game's shader, unlit.
        Count(pixelFile, Path::Unlit);
        pass.route = Pass::Route::Game;
        pass.clearsCoverage = !blend;
        return pass;
    }
    Count(pixelFile, water ? Path::Water : !twin.empty() ? Path::Twin : Path::Preview);
    if (!twin.empty()) {
        pass.pixelCode = twin;
        pass.emissive = true;
        pass.lightWarpSampler = twins::LightWarpSampler(pixelFile, *pixelCombo.header, pixelCombo.staticIndex);
    } else if (!shaderClass.patched.empty()) {
        pass.pixelCode = shaderClass.patched;
    } else {
        pass.pixelCode = shaderClass.replacement;
    }
    pass.pixelName = pixel->name.c_str();
    for (UINT r = 0; r < 4; ++r)
        std::copy_n(VertexRegister(gbuffer::kViewProjection + r).v, 4, g_state.viewProj[r].begin());
    ++g_state.gbufferDraws;
    return pass;
}

void AddCoverageTarget(Targets& targets)
{
    Backend& backend = GetBackend();
    Target& target = g_state.targets[kFlags];
    backend.Transition(target.resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
    targets.colors[targets.count] = target.view;
    targets.colorFormats[targets.count] = kTargetFormats[kFlags];
    ++targets.count;
    backend.Commands()->OMSetRenderTargets(targets.count, targets.colors, FALSE, targets.depth.ptr != 0 ? &targets.depth : nullptr);
}

Targets BindTargets(const Pass& pass)
{
    Backend& backend = GetBackend();
    Targets targets;
    targets.width = backend.Width();
    targets.height = backend.Height();
    auto add = [&](TargetIndex index) {
        Target& target = g_state.targets[index];
        backend.Transition(target.resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
        targets.colors[targets.count] = target.view;
        targets.colorFormats[targets.count] = kTargetFormats[index];
        ++targets.count;
    };
    switch (pass.route) {
    case Pass::Route::GBuffer:
        add(kAlbedo);
        add(kNormal);
        add(kEmissive);
        add(kFlags);
        break;
    case Pass::Route::Albedo: add(kAlbedo); break;
    case Pass::Route::Light:
        add(kLight);
        targets.linear = true;
        break;
    default: break;
    }
    backend.Transition(backend.DepthBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    targets.depth = backend.DepthBufferView();
    targets.depthFormat = Backend::kDepthFormat;
    backend.Commands()->OMSetRenderTargets(targets.count, targets.colors, FALSE, &targets.depth);
    return targets;
}

float LightWarpSlot(UINT view)
{
    auto found = std::find(g_state.lightWarps.begin(), g_state.lightWarps.end(), view);
    if (found == g_state.lightWarps.end()) {
        if (g_state.lightWarps.size() >= kMaxLightWarps)
            return 0.0f;
        found = g_state.lightWarps.insert(g_state.lightWarps.end(), view);
    }
    return static_cast<float>(found - g_state.lightWarps.begin() + 1) / 255.0f;
}

UINT ProbeView(size_t index)
{
    TrackedResource& probes = g_state.forwardOpenAir ? g_state.openAirProbes[index] : g_state.probes[index];
    GetBackend().Transition(probes, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    return g_state.forwardOpenAir ? g_state.openAirViews[index] : g_state.probeViews[index];
}

// The engine loads the 2D skybox's faces as the materials "skybox/<sv_skyname><side>" (engine.dll 0x180102020, with
// "skybox/%s%s") and draws all six (face mask 0x3f, 0x180101470 called from the world list draw 0x1800fdc90) inside
// the views' world passes, which are captured.
bool SkyMaterial()
{
    constexpr std::string_view kPrefix = "skybox/";
    const std::string_view name = BoundMaterialName();
    return name.size() > kPrefix.size() && std::equal(kPrefix.begin(), kPrefix.end(), name.begin(),
        [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
}

bool DrawsSky() { return g_state.active && SkyMaterial(); }

// GMod's painted sky (g_sky_ps20b) reads the sun it draws from g_SunNormal (c5 in its constant table), env_skypaint's
// sun normal, which its Lua entity keeps at env_sun's m_vDirection when the map has one.
void RecordSkyShader(std::span<const DWORD> pixelCode)
{
    auto found = g_state.sunNormalRegisters.find(pixelCode.data());
    if (found == g_state.sunNormalRegisters.end()) {
        int reg = -1;
        for (const bytecode::Constant& constant : bytecode::ConstantTable(pixelCode)) {
            if (constant.name == "g_SunNormal" && constant.set == bytecode::RegisterSet::Float4)
                reg = static_cast<int>(constant.index);
        }
        found = g_state.sunNormalRegisters.emplace(pixelCode.data(), reg).first;
    }
    g_state.paintedSun.reset();
    if (found->second < 0)
        return;
    const float* normal = PixelRegister(static_cast<UINT>(found->second)).v;
    const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (std::isfinite(length) && length > 1e-3f)
        g_state.paintedSun = std::array<float, 3>{normal[0] / length, normal[1] / length, normal[2] / length};
}

void SetSky(std::optional<gbuffer::SkyDraw> sky) { g_state.skyDraw = std::move(sky); }

const gbuffer::SkyDraw* Sky() { return g_state.skyDraw && (g_state.skyDraw->sky || g_state.skyDraw->sunDisc) ? &*g_state.skyDraw : nullptr; }

bool IsSunSprite(const MeshDraw& mesh)
{
    const gbuffer::SkyDraw* sky = Sky();
    if (mesh.vertexCount != 4 || mesh.vertexData == nullptr || (mesh.vertexFormat & kVertexPosition) == 0 || sky == nullptr || !sky->sunDisc ||
        !sky->spriteDirection)
        return false;
    const std::optional<std::array<float, 3>> eye = ViewEye();
    if (!eye)
        return false;
    const UINT stride = mesh.streams[0].StrideInBytes;
    std::array<float, 3> centre = {};
    for (UINT v = 0; v < 4; ++v) {
        float corner[3];
        std::memcpy(corner, mesh.vertexData + v * stride, sizeof(corner));
        for (int c = 0; c < 3; ++c)
            centre[c] += corner[c] * 0.25f;
    }
    const std::array<float, 3> toCentre = {centre[0] - (*eye)[0], centre[1] - (*eye)[1], centre[2] - (*eye)[2]};
    const float distance = std::sqrt(toCentre[0] * toCentre[0] + toCentre[1] * toCentre[1] + toCentre[2] * toCentre[2]);
    const std::array<float, 3>& sun = *sky->spriteDirection;
    const float alignment = (toCentre[0] * sun[0] + toCentre[1] * sun[1] + toCentre[2] * sun[2]) / std::max(distance, 1e-3f);
    constexpr float kSpriteDistance = 100.0f;
    return std::fabs(distance - kSpriteDistance) < 1.0f && alignment > 0.99999f;
}

std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> SkyFaces()
{
    if (!CreateSkyCube())
        return {};
    SkyCube& sky = g_state.sky;
    Backend& backend = GetBackend();
    backend.Transition(sky.resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const std::string level = engine::LevelName();
    if (level != sky.level) {
        sky.level = level;
        constexpr float kUndrawn[4] = {-1.0f, -1.0f, -1.0f, 0.0f};
        for (const D3D12_CPU_DESCRIPTOR_HANDLE& face : sky.faces)
            backend.Commands()->ClearRenderTargetView(face, kUndrawn, 0, nullptr);
    }
    return sky.faces;
}

void Release()
{
    Backend& backend = GetBackend();
    for (Target& target : g_state.targets) {
        if (target.resource.resource)
            backend.Retire(std::move(target.resource.resource));
        if (target.view.ptr != 0)
            backend.RenderTargetViews().Free(target.view);
        target = {};
    }
    g_state.width = 0;
    g_state.height = 0;
    g_state.active = false;
    g_state.forwardActive = false;
    g_state.skipView = false;
}

}
