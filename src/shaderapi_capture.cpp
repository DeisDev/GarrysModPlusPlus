#include "shaderapi_capture.h"

#include "engine_interfaces.h"
#include "log.h"
#include "shaderapi.h"
#include "shaderapi_textures.h"
#include "shader_twins.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace gmodpp::native::capture {

namespace {

using gbuffer::ShaderKind;
using gbuffer::ViewKind;

// Albedo and self-illumination are linear; FP16 keeps dark albedo precise. The light target has the back buffer's
// format, so additive passes write it as they would the back buffer.
enum TargetIndex { kAlbedo, kNormal, kEmissive, kFlags, kLight, kTargetCount };
constexpr DXGI_FORMAT kTargetFormats[kTargetCount] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS};
constexpr size_t kMaxProjectedLights = 8;
// ShaderBlendFactor_t (shaderapi/ishaderdynamic.h): SHADER_BLEND_ONE, SHADER_BLEND_DST_COLOR, SHADER_BLEND_SRC_ALPHA and
// SHADER_BLEND_SRC_COLOR.
constexpr int kBlendOne = 1;
constexpr int kBlendDestinationColor = 2;
constexpr int kBlendSourceAlpha = 4;
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
    D3D12_CPU_DESCRIPTOR_HANDLE srgbView = {};
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
    bool active = false;
    bool forwardActive = false;
    std::array<float, 2> jitter = {};
    gbuffer::Matrix4 viewProj = {};
    std::array<std::array<float, 4>, bytecode::kForwardUniforms> forwardConstants = {};
    uint32_t forwardDraws = 0;
    uint32_t forwardTwinDraws = 0;
    D3D12_GPU_VIRTUAL_ADDRESS twinFrame = 0;
    D3D12_GPU_VIRTUAL_ADDRESS twinScene = 0;
    uint32_t forwardLogged = 0;
    // Fog of the main view and of the 3D skybox, which has its own.
    std::array<gbuffer::Fog, 2> fog;
    std::optional<std::array<float, 3>> waterFogColor;
    gbuffer::SwayClock swayClock;
    std::vector<Projected> projected;
    std::vector<Projected> lastProjected;
    // The view's projected lights and their cookies, for ViewInputs.
    std::vector<gbuffer::ProjectedLight> lights;
    std::vector<ID3D12Resource*> cookies;
    uint32_t flashlightDraws = 0;
    std::pair<uint32_t, size_t> loggedProjection = {0, 0};
    uint64_t sections = 0;
    uint32_t srgbDraws = 0;
    uint32_t linearDraws = 0;
    bool outputSrgb = true;
    bool loggedSrgb = false;
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
        view.Format = i == kLight ? DXGI_FORMAT_B8G8R8A8_UNORM : kTargetFormats[i];
        backend.Device()->CreateRenderTargetView(target.resource.resource.Get(), &view, target.view);
        if (i == kLight) {
            target.srgbView = backend.RenderTargetViews().Allocate();
            view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            backend.Device()->CreateRenderTargetView(target.resource.resource.Get(), &view, target.srgbView);
        }
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
    g_state.waterFogColor.reset();
    g_state.projected.clear();
    g_state.srgbDraws = 0;
    g_state.linearDraws = 0;
    g_state.active = true;
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
    if (kind != ViewKind::Viewmodel) {
        // Every lit shader's vertex fog constant holds the view's range fog (cFogParams: end / range, 1,
        // 1 - max density, 1 / range) and pixel c29 its colour; Source writes that constant only when fog is on, so
        // the fog mode says whether it belongs to this view.
        const float* range = VertexRegister(gbuffer::kVertexFogRegister).v;
        const float* color = PixelRegister(gbuffer::kFogColorRegister).v;
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
    if (g_state.srgbDraws + g_state.linearDraws != 0) {
        const bool srgb = g_state.srgbDraws >= g_state.linearDraws;
        if (srgb != g_state.outputSrgb || !g_state.loggedSrgb) {
            g_state.loggedSrgb = true;
            log::Info("G-buffer: the game draws lit surfaces with sRGB {} ({} sRGB draws, {} linear draws)", srgb ? "on" : "off", g_state.srgbDraws,
                g_state.linearDraws);
            g_state.outputSrgb = srgb;
        }
    }

    Backend& backend = GetBackend();
    auto common = [&](TrackedResource& resource) {
        backend.Transition(resource, D3D12_RESOURCE_STATE_COMMON);
        return resource.resource.Get();
    };
    inputs.color = common(backend.BackBuffer());
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
    if (kind == ViewKind::Main && g_state.probes[0].resource) {
        for (size_t i = 0; i < g_state.probes.size(); ++i)
            inputs.probes[i] = common(g_state.probes[i]);
    }
    inputs.kind = kind;
    inputs.viewProj = g_state.viewProj;
    inputs.jitter = g_state.jitter;
    const float scale = PixelRegister(gbuffer::kLightScaleRegister).v[0];
    inputs.tonemapScale = scale > 0.0f ? scale : 1.0f;
    inputs.outputSrgb = g_state.outputSrgb;
    inputs.fog = g_state.fog[kind == ViewKind::Skybox ? 1 : 0];
    inputs.sway = g_state.swayClock;
    return true;
}

void BeginForward(const gbuffer::ForwardConstants& constants)
{
    if (!g_state.probes[0].resource)
        return;
    using gbuffer::kProbeGrid;
    using gbuffer::kProbeSpacing;
    const float extent[3] = {kProbeSpacing * kProbeGrid[0], kProbeSpacing * kProbeGrid[1], kProbeSpacing * kProbeGrid[2]};
    auto& c = g_state.forwardConstants;
    c[1] = {constants.fallback[0], constants.fallback[1], constants.fallback[2], constants.lightOnly ? 1.0f : 0.0f};
    c[2] = {constants.fog.color[0], constants.fog.color[1], constants.fog.color[2], 0.0f};
    c[3] = {1.0f / extent[0], 1.0f / extent[1], 1.0f / extent[2], 0.0f};
    // The window ends at the centres of its outermost probes.
    c[0] = {0.0f, 2.0f / (extent[0] - kProbeSpacing), 2.0f / (extent[1] - kProbeSpacing), 2.0f / (extent[2] - kProbeSpacing)};
    c[4] = {(constants.probeOrigin[0] + kProbeGrid[0] * 0.5f) * kProbeSpacing, (constants.probeOrigin[1] + kProbeGrid[1] * 0.5f) * kProbeSpacing,
        (constants.probeOrigin[2] + kProbeGrid[2] * 0.5f) * kProbeSpacing, 0.0f};
    c[5] = {constants.eye[0], constants.eye[1], constants.eye[2], constants.tonemapScale};
    c[6] = constants.fog.known ? constants.fog.params : std::array<float, 4>{};
    g_state.twinFrame = constants.twinFrame;
    g_state.twinScene = constants.twinScene;
    g_state.forwardActive = true;
    g_state.forwardDraws = 0;
    g_state.forwardTwinDraws = 0;
}

bool ForwardActive() { return g_state.forwardActive; }

void EndForward()
{
    if (!g_state.forwardActive)
        return;
    g_state.forwardActive = false;
    if (g_state.forwardLogged < 3) {
        ++g_state.forwardLogged;
        log::Info("G-buffer: {} forward-lit draws, {} of them by forward twins ({} preview shaders have a forward variant, {} could not take one)",
            g_state.forwardDraws, g_state.forwardTwinDraws, gbuffer::ShaderStatistics().forward, gbuffer::ShaderStatistics().forwardFailures);
    }
}

int FixedLighting()
{
    if (g_state.active)
        return 2;
    // Unlit materials keep the game's own shaders during forward lighting: their preview combos leave out vertex
    // colour (see engine_interfaces.cpp).
    if (g_state.forwardActive) {
        void* material = BoundMaterial();
        return material != nullptr && engine::IsUnlitMaterial(material) ? 0 : 2;
    }
    return -1;
}

Pass Prepare(std::span<const DWORD> vertexCode, std::span<const DWORD> pixelCode, const char* pixelFile, const ComboId& pixelCombo, bool depthTest,
    bool blend, int sourceBlend, int destinationBlend, bool srgbWrite, std::uint32_t srgbRead)
{
    Pass pass;
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
        // Forward twins trace their own light; additive draws add light and are shown as drawn.
        const std::span<const DWORD> twin = !additive && g_state.twinFrame != 0 && g_state.twinScene != 0 && pixelFile != nullptr && pixelCombo.header != nullptr
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
    }
    // Opaque shaders with a twin but no preview combo (eyes, teeth) are drawn into the G-buffer by their twin.
    const bool twinOnly = shaderClass.kind == ShaderKind::Other && !blend && pixelFile != nullptr && twins::HasTwin(pixelFile);
    const bool gbuffer = (shaderClass.kind == ShaderKind::GBuffer || water || twinOnly) && depthTest && gameTarget;
    if (shaderClass.kind == ShaderKind::GBuffer && !gbuffer && !g_state.loggedOtherTarget) {
        g_state.loggedOtherTarget = true;
        log::Info("G-buffer: a G-buffer draw went to another render target during a capture; it is drawn there unlit");
    }
    // Multiplying decals (DecalModulate's mod2x blend) darken the surface under them, so they multiply its albedo.
    const bool modulate = !gbuffer && shaderClass.kind == ShaderKind::Other && blend && depthTest && gameTarget &&
        sourceBlend == kBlendDestinationColor && destinationBlend == kBlendSourceColor;
    pass.route = gbuffer ? Pass::Route::GBuffer : modulate ? Pass::Route::Albedo : Pass::Route::Game;
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
    } else if (!shaderClass.patched.empty()) {
        pass.pixelCode = shaderClass.patched;
    } else {
        pass.pixelCode = shaderClass.replacement;
    }
    pass.pixelName = pixel->name.c_str();
    // The game's sRGB state for its own lit surfaces says how the back buffer is encoded.
    ++(srgbWrite ? g_state.srgbDraws : g_state.linearDraws);
    for (UINT r = 0; r < 4; ++r)
        std::copy_n(VertexRegister(gbuffer::kViewProjection + r).v, 4, g_state.viewProj[r].begin());
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

Targets BindTargets(const Pass& pass, bool srgb)
{
    Backend& backend = GetBackend();
    Targets targets;
    targets.width = backend.Width();
    targets.height = backend.Height();
    auto add = [&](TargetIndex index, bool srgbView) {
        Target& target = g_state.targets[index];
        backend.Transition(target.resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
        targets.colors[targets.count] = srgbView ? target.srgbView : target.view;
        targets.colorFormats[targets.count] = index == kLight ? (srgbView ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM) : kTargetFormats[index];
        ++targets.count;
    };
    switch (pass.route) {
    case Pass::Route::GBuffer:
        add(kAlbedo, false);
        add(kNormal, false);
        add(kEmissive, false);
        add(kFlags, false);
        break;
    case Pass::Route::Albedo: add(kAlbedo, false); break;
    case Pass::Route::Light: add(kLight, srgb); break;
    default: break;
    }
    backend.Transition(backend.DepthBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    targets.depth = backend.DepthBufferView();
    targets.depthFormat = Backend::kDepthFormat;
    backend.Commands()->OMSetRenderTargets(targets.count, targets.colors, FALSE, &targets.depth);
    return targets;
}

UINT ProbeView(size_t index)
{
    GetBackend().Transition(g_state.probes[index], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    return g_state.probeViews[index];
}

void Release()
{
    Backend& backend = GetBackend();
    for (Target& target : g_state.targets) {
        if (target.resource.resource)
            backend.Retire(std::move(target.resource.resource));
        if (target.view.ptr != 0)
            backend.RenderTargetViews().Free(target.view);
        if (target.srgbView.ptr != 0)
            backend.RenderTargetViews().Free(target.srgbView);
        target = {};
    }
    g_state.width = 0;
    g_state.height = 0;
    g_state.active = false;
    g_state.forwardActive = false;
}

}
