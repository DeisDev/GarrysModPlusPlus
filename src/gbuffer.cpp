#include "gbuffer.h"

#include "engine_interfaces.h"
#include "gbuffer_shaders.h"
#include "log.h"
#include "settings.h"
#include "shader_bytecode.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp::gbuffer {

namespace {

// Albedo and self-illumination are linear; FP16 keeps dark albedo precise. sRGB writes stay off for
// G-buffer draws, which these formats could not take anyway.
constexpr D3DFORMAT kAlbedoFormat = D3DFMT_A16B16G16R16F;
constexpr D3DFORMAT kEmissiveFormat = D3DFMT_A16B16G16R16F;
constexpr D3DFORMAT kNormalFormat = D3DFMT_A16B16G16R16F;
constexpr D3DFORMAT kFlagsFormat = D3DFMT_A8R8G8B8;
// Same format as the back buffer, so flashlight passes write it exactly as they would the back buffer.
constexpr D3DFORMAT kLightFormat = D3DFMT_A8R8G8B8;
constexpr DWORD kDefaultColorWrite = 0x0F;
constexpr DWORD kColorWriteRgb = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;
constexpr UINT kTrackedRegisters = 32;
constexpr UINT kTrackedVertexRegisters = 256;
constexpr size_t kMaxProjectedLights = 8;
// The render targets a capture draw writes: the game's, the G-buffer, or the albedo alone (decals that multiply it).
enum class Targets { Game, GBuffer, Albedo };

struct ShaderInfo {
    ShaderKind kind = ShaderKind::Other;
    FlashlightRegisters flashlight;
    // Its replacement writes self-illumination to the emissive target.
    bool emissive = false;
    // The variant lit by the probe volume after the capture, the samplers it reads the volume from and the
    // registers of its constants (shaders/forward_light.hlsli).
    IDirect3DPixelShader9* forward = nullptr;
    std::array<UINT, 3> probeSamplers = {};
    std::array<UINT, bytecode::kForwardUniforms> forwardUniforms = {};
    // The skin detail blend mode its replacement applies (shaders/gbuffer_models.hlsl), or -1.
    int detailMode = -1;
};

constexpr UINT kForwardConstantCount = bytecode::kForwardUniforms;
// Sampler states a forward draw changes on its probe samplers, which are put back afterwards.
constexpr D3DSAMPLERSTATETYPE kProbeSamplerStates[] = {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
    D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE};
constexpr DWORD kProbeSamplerValues[] = {D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, FALSE};
// Direct3D 9's initial values of those states.
constexpr DWORD kDefaultSamplerValues[] = {D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, FALSE};
constexpr size_t kTrackedSamplerStates = std::size(kProbeSamplerStates);
constexpr size_t kSrgbTextureState = 6;
static_assert(kProbeSamplerStates[kSrgbTextureState] == D3DSAMP_SRGBTEXTURE);

// A render state as the device has it and as the game last set it.
struct TrackedState {
    DWORD device;
    DWORD game;
};

struct State {
    ComPtr<IDirect3DTexture9> albedo;
    ComPtr<IDirect3DSurface9> albedoSurface;
    ComPtr<IDirect3DTexture9> emissive;
    ComPtr<IDirect3DSurface9> emissiveSurface;
    ComPtr<IDirect3DTexture9> normal;
    ComPtr<IDirect3DTexture9> flags;
    ComPtr<IDirect3DSurface9> normalSurface;
    ComPtr<IDirect3DSurface9> flagsSurface;
    ComPtr<IDirect3DTexture9> light;
    ComPtr<IDirect3DSurface9> lightSurface;
    // The game's render target when the capture began, and the one it has set since.
    ComPtr<IDirect3DSurface9> colorTarget;
    IDirect3DSurface9* gameTarget = nullptr;
    Targets boundTargets = Targets::Game;
    bool loggedOtherTarget = false;
    // Set while this module calls the device, so the hooks do not take its calls for the game's.
    bool ownCall = false;
    D3DVIEWPORT9 viewport = {};
    // Our shaders drawn in place of the game's (PixelShaderClass::replacement and forwardReplacement), by their code.
    std::unordered_map<const DWORD*, ComPtr<IDirect3DPixelShader9>> replacements;
    // g_WaterFogColor of the last water surface drawn in this capture.
    std::optional<std::array<float, 3>> waterFogColor;
    // Forward variants of the patched preview shaders, by the shader they replace.
    std::unordered_map<IDirect3DPixelShader9*, ComPtr<IDirect3DPixelShader9>> forwardVariants;
    std::array<ComPtr<IDirect3DVolumeTexture9>, 3> probeTextures;
    bool forwardActive = false;
    std::array<std::array<float, 4>, kForwardConstantCount> forwardConstants = {};
    // The game's sampler states on each stage (kProbeSamplerStates).
    std::array<std::array<DWORD, kTrackedSamplerStates>, kTextureStages> samplerStates = {};
    // Forward variants the device rejected.
    uint32_t forwardFailures = 0;
    uint32_t forwardDraws = 0;
    uint32_t forwardLogged = 0;
    std::unordered_map<IDirect3DPixelShader9*, ShaderInfo> shaders;
    std::unordered_map<IDirect3DVertexShader9*, VertexShaderClass> vertexShaders;
    FlashlightRegisters boundVertex;
    // The bound vertex shader's sway register, or -1.
    int boundSwayRegister = -1;
    SwayClock swayClock;
    std::array<std::array<float, 4>, kTrackedVertexRegisters> vertexConstants = {};
    std::array<IDirect3DBaseTexture9*, kTextureStages> textures = {};
    std::vector<ProjectedLight> projected;
    std::vector<ComPtr<IDirect3DBaseTexture9>> projectedCookies;
    std::vector<ProjectedLight> lastProjected;
    std::vector<ComPtr<IDirect3DBaseTexture9>> lastCookies;
    uint32_t flashlightDraws = 0;
    float jitter[2] = {};
    Matrix4 viewProj = {};
    uint64_t sections = 0;
    std::pair<uint32_t, size_t> loggedProjection = {0, 0};
    std::unordered_map<IDirect3DPixelShader9*, IDirect3DPixelShader9*> substitutes;
    IDirect3DPixelShader9* requested = nullptr;
    ShaderInfo bound;
    std::array<std::array<float, 4>, kTrackedRegisters> constants = {};
    // Fog of the main view and of the 3D skybox, which has its own.
    std::array<Fog, 2> fog;
    bool depthTest = true;
    bool blending = false;
    DWORD sourceBlend = D3DBLEND_ONE;
    DWORD destinationBlend = D3DBLEND_ZERO;
    TrackedState srgbWrite = {FALSE, FALSE};
    TrackedState emissiveWrites = {kDefaultColorWrite, kDefaultColorWrite};
    bool active = false;
    uint32_t srgbDraws = 0;
    uint32_t linearDraws = 0;
    bool outputSrgb = true;
    bool loggedSrgb = false;
    bool loggedCounts = false;
};

State g_state;

// Rebuilds the projected light a flashlight pass is drawing from the constants and textures bound for it.
void CaptureProjectedLight()
{
    const FlashlightRegisters& ps = g_state.bound.flashlight;
    const FlashlightRegisters& vs = g_state.boundVertex;
    ++g_state.flashlightDraws;
    if (ps.attenuation < 0 || ps.color < 0)
        return;
    ProjectedLight light;
    ComPtr<IDirect3DBaseTexture9> cookie;
    const float* origin = ps.position >= 0 ? g_state.constants[ps.position].data()
        : vs.position >= 0                 ? g_state.vertexConstants[vs.position].data()
                                           : nullptr;
    if (origin == nullptr)
        return;
    if (ps.worldToTexture >= 0) {
        for (int i = 0; i < 4; ++i)
            light.worldToTexture[i] = g_state.constants[ps.worldToTexture + i];
    } else if (vs.worldToTexture >= 0) {
        for (int i = 0; i < 4; ++i)
            light.worldToTexture[i] = g_state.vertexConstants[vs.worldToTexture + i];
    } else {
        return;
    }
    const auto& attenuation = g_state.constants[ps.attenuation];
    const auto& color = g_state.constants[ps.color];
    light.origin = {origin[0], origin[1], origin[2]};
    light.attenuation = {attenuation[0], attenuation[1], attenuation[2]};
    light.farZ = attenuation[3];
    light.color = {color[0], color[1], color[2]};
    if (ps.cookie >= 0) {
        cookie = g_state.textures[ps.cookie];
        light.cookieSrgb = g_state.samplerStates[ps.cookie][kSrgbTextureState] != FALSE;
    }
    for (const ProjectedLight& existing : g_state.projected) {
        if (existing.origin == light.origin && existing.worldToTexture == light.worldToTexture)
            return;
    }
    if (g_state.projected.size() < kMaxProjectedLights) {
        if (g_state.sections % 600 == 0) {
            const auto& m = light.worldToTexture;
            log::Info("G-buffer: projected light from {} constants: origin ({:.1f} {:.1f} {:.1f}) far {:.1f} attenuation ({:.2f} {:.2f} {:.2f}) color "
                      "({:.3f} {:.3f} {:.3f}) cookie sRGB {} rows ({:.3f} {:.3f} {:.3f} {:.1f}) ({:.3f} {:.3f} {:.3f} {:.1f}) ({:.3f} {:.3f} {:.3f} {:.1f}) "
                      "({:.3f} {:.3f} {:.3f} {:.1f})",
                ps.worldToTexture >= 0 ? "pixel" : "vertex", light.origin[0], light.origin[1], light.origin[2], light.farZ, light.attenuation[0],
                light.attenuation[1], light.attenuation[2], light.color[0], light.color[1], light.color[2], light.cookieSrgb, m[0][0], m[0][1], m[0][2], m[0][3],
                m[1][0], m[1][1], m[1][2], m[1][3], m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
        }
        g_state.projected.push_back(light);
        g_state.projectedCookies.push_back(std::move(cookie));
    }
}

// The device's shader for one of ours, created on first use; null when there is none or the device rejects it.
IDirect3DPixelShader9* Replacement(IDirect3DDevice9* device, std::span<const DWORD> code, CreatePixelShaderFn create)
{
    if (code.empty())
        return nullptr;
    ComPtr<IDirect3DPixelShader9>& slot = g_state.replacements[code.data()];
    if (!slot && FAILED(create(device, code.data(), &slot)))
        log::Info("G-buffer: creating a replacement pixel shader failed");
    return slot.Get();
}

void Register(IDirect3DPixelShader9* shader, ShaderKind kind, IDirect3DPixelShader9* substitute,
    const FlashlightRegisters& flashlight = {}, bool emissive = false, IDirect3DPixelShader9* forward = nullptr, std::array<UINT, 3> probeSamplers = {},
    std::array<UINT, kForwardConstantCount> forwardUniforms = {}, int detailMode = -1)
{
    // Shader addresses are reused after the game releases one, so every creation overwrites the old entry.
    g_state.shaders[shader] = {kind, flashlight, emissive, forward, probeSamplers, forwardUniforms, detailMode};
    if (forward == nullptr || forward != g_state.forwardVariants[shader].Get())
        g_state.forwardVariants.erase(shader);
    if (substitute != nullptr)
        g_state.substitutes[shader] = substitute;
    else
        g_state.substitutes.erase(shader);
}

}

bool Initialize(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& params)
{
    Release();
    D3DCAPS9 caps = {};
    device->GetDeviceCaps(&caps);
    const bool independentDepths = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS) != 0;
    const bool blending = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING) != 0;
    // The stock preview shaders write positions to the emissive target; only its write mask keeps them out.
    const bool writeMasks = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_INDEPENDENTWRITEMASKS) != 0;
    if (caps.NumSimultaneousRTs < 4 || !independentDepths || !writeMasks) {
        log::Info("G-buffer disabled: the device supports {} render targets (independent bit depths: {}, independent write masks: {})",
            caps.NumSimultaneousRTs, independentDepths, writeMasks);
        return false;
    }
    auto createTarget = [&](D3DFORMAT format, ComPtr<IDirect3DTexture9>& texture, ComPtr<IDirect3DSurface9>& surface) {
        HRESULT hr = device->CreateTexture(params.BackBufferWidth, params.BackBufferHeight, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT,
            &texture, nullptr);
        return SUCCEEDED(hr) ? texture->GetSurfaceLevel(0, &surface) : hr;
    };
    HRESULT hr = createTarget(kAlbedoFormat, g_state.albedo, g_state.albedoSurface);
    if (SUCCEEDED(hr))
        hr = createTarget(kEmissiveFormat, g_state.emissive, g_state.emissiveSurface);
    if (SUCCEEDED(hr))
        hr = device->CreateTexture(params.BackBufferWidth, params.BackBufferHeight, 1, D3DUSAGE_RENDERTARGET, kNormalFormat, D3DPOOL_DEFAULT,
            &g_state.normal, nullptr);
    if (SUCCEEDED(hr))
        hr = device->CreateTexture(params.BackBufferWidth, params.BackBufferHeight, 1, D3DUSAGE_RENDERTARGET, kFlagsFormat, D3DPOOL_DEFAULT,
            &g_state.flags, nullptr);
    if (SUCCEEDED(hr))
        hr = device->CreateTexture(params.BackBufferWidth, params.BackBufferHeight, 1, D3DUSAGE_RENDERTARGET, kLightFormat, D3DPOOL_DEFAULT,
            &g_state.light, nullptr);
    if (SUCCEEDED(hr))
        hr = g_state.light->GetSurfaceLevel(0, &g_state.lightSurface);
    if (SUCCEEDED(hr))
        hr = g_state.normal->GetSurfaceLevel(0, &g_state.normalSurface);
    if (SUCCEEDED(hr))
        hr = g_state.flags->GetSurfaceLevel(0, &g_state.flagsSurface);
    if (FAILED(hr)) {
        log::Info("G-buffer disabled: creating render targets failed ({:#010x})", static_cast<unsigned>(hr));
        Release();
        return false;
    }
    log::Info("G-buffer: {}x{} targets ready (MRT blending: {})", params.BackBufferWidth, params.BackBufferHeight, blending);
    // A reset puts every sampler state back to its default.
    for (auto& stage : g_state.samplerStates)
        std::copy(std::begin(kDefaultSamplerValues), std::end(kDefaultSamplerValues), stage.begin());
    for (ComPtr<IDirect3DVolumeTexture9>& texture : g_state.probeTextures) {
        if (FAILED(device->CreateVolumeTexture(kProbeGrid[0], kProbeGrid[1], kProbeGrid[2], 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &texture, nullptr))) {
            log::Info("G-buffer: creating the probe volume textures failed; translucent surfaces keep their baked light");
            for (ComPtr<IDirect3DVolumeTexture9>& created : g_state.probeTextures)
                created.Reset();
            break;
        }
    }
    return true;
}

void Release()
{
    g_state.albedoSurface.Reset();
    g_state.albedo.Reset();
    g_state.emissiveSurface.Reset();
    g_state.emissive.Reset();
    g_state.normalSurface.Reset();
    g_state.flagsSurface.Reset();
    g_state.normal.Reset();
    g_state.flags.Reset();
    g_state.lightSurface.Reset();
    g_state.light.Reset();
    g_state.colorTarget.Reset();
    for (ComPtr<IDirect3DVolumeTexture9>& texture : g_state.probeTextures)
        texture.Reset();
    g_state.active = false;
    g_state.forwardActive = false;
}

HRESULT CreatePixelShader(IDirect3DDevice9* device, const DWORD* code, IDirect3DPixelShader9** shader, CreatePixelShaderFn create)
{
    const size_t tokens = code != nullptr ? bytecode::TokenCount(code) : 0;
    if (tokens == 0)
        return create(device, code, shader);
    const PixelShaderClass shaderClass = ClassifyPixelShader({code, tokens}, kTrackedRegisters);
    if (!shaderClass.patched.empty()) {
        HRESULT hr = create(device, shaderClass.patched.data(), shader);
        if (SUCCEEDED(hr)) {
            ComPtr<IDirect3DPixelShader9> forward;
            if (!shaderClass.forward.empty() && FAILED(create(device, shaderClass.forward.data(), &forward))) {
                forward.Reset();
                if (++g_state.forwardFailures <= 5)
                    log::Info("G-buffer: a preview shader gets no forward-lit variant: the device rejected the patched shader");
            }
            Register(*shader, ShaderKind::GBuffer, nullptr, {}, false, forward.Get(), shaderClass.probeSamplers, shaderClass.forwardUniforms);
            if (forward)
                g_state.forwardVariants[*shader] = std::move(forward);
        }
        return hr;
    }

    HRESULT hr = create(device, code, shader);
    if (FAILED(hr))
        return hr;
    IDirect3DPixelShader9* substitute = Replacement(device, shaderClass.replacement, create);
    IDirect3DPixelShader9* forward = Replacement(device, shaderClass.forwardReplacement, create);
    // Without its replacement a shader is drawn as the game's.
    const bool replaced = shaderClass.kind == ShaderKind::GBuffer || shaderClass.kind == ShaderKind::Water;
    const ShaderKind kind = replaced && substitute == nullptr ? ShaderKind::Other : shaderClass.kind;
    Register(*shader, kind, substitute, shaderClass.flashlight, substitute != nullptr && shaderClass.emissive, forward, shaderClass.probeSamplers,
        shaderClass.forwardUniforms, shaderClass.detailMode);
    return hr;
}

IDirect3DPixelShader9* SubstitutePixelShader(IDirect3DPixelShader9* requested)
{
    g_state.requested = requested;
    auto info = g_state.shaders.find(requested);
    g_state.bound = info != g_state.shaders.end() ? info->second : ShaderInfo{};
    if (g_state.forwardActive && g_state.bound.forward != nullptr)
        return g_state.bound.forward;
    if (!g_state.active)
        return requested;
    auto substitute = g_state.substitutes.find(requested);
    return substitute != g_state.substitutes.end() ? substitute->second : requested;
}

template <class Call>
void OwnCall(Call call)
{
    g_state.ownCall = true;
    call();
    g_state.ownCall = false;
}

void SetDeviceState(IDirect3DDevice9* device, D3DRENDERSTATETYPE state, TrackedState& tracked, DWORD value)
{
    if (tracked.device == value)
        return;
    OwnCall([&] { device->SetRenderState(state, value); });
    tracked.device = value;
}

// The skin replacement's detail blend mode; the game never uses its register.
void SetDetailMode(IDirect3DDevice9* device)
{
    const float mode[4] = {static_cast<float>(g_state.bound.detailMode), 0.0f, 0.0f, 0.0f};
    OwnCall([&] { device->SetPixelShaderConstantF(kDetailModeRegister, mode, 1); });
}

// G-buffer draws write albedo, normal, self-illumination and coverage; every other draw goes to one target alone,
// as D3D12 needs a null view where a shader writes nothing.
void BindTargets(IDirect3DDevice9* device, Targets targets)
{
    if (g_state.boundTargets == targets)
        return;
    g_state.boundTargets = targets;
    const bool gbuffer = targets == Targets::GBuffer;
    OwnCall([&] {
        device->SetRenderTarget(0, targets == Targets::Game ? g_state.colorTarget.Get() : g_state.albedoSurface.Get());
        device->SetRenderTarget(kNormalTarget, gbuffer ? g_state.normalSurface.Get() : nullptr);
        device->SetRenderTarget(kEmissiveTarget, gbuffer ? g_state.emissiveSurface.Get() : nullptr);
        device->SetRenderTarget(kFlagsTarget, gbuffer ? g_state.flagsSurface.Get() : nullptr);
        device->SetViewport(&g_state.viewport);
    });
}

DrawAction BeforeDraw(IDirect3DDevice9* device)
{
    if (g_state.boundSwayRegister >= 0) {
        const auto& params = g_state.vertexConstants[g_state.boundSwayRegister];
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_state.swayClock = {params[1], {params[2], params[3]}, now.QuadPart, true};
    }
    if (g_state.forwardActive && g_state.bound.forward != nullptr)
        return DrawAction::Forward;
    // VertexLitGeneric has no lighting preview variant of its flashlight pass, so on translucent models (window glass)
    // the game draws it with no pixel shader, which comes out white.
    if (g_state.forwardActive && g_state.requested == nullptr && g_state.blending && g_state.destinationBlend == D3DBLEND_ONE)
        return DrawAction::Skip;
    if (!g_state.active)
        return DrawAction::Draw;
    // Preview shaders write unlit materials as surfaces to be lit. Those drawn without a depth test ($ignorez,
    // as 2D skybox faces often are) stay the colour they draw.
    const bool water = g_state.bound.kind == ShaderKind::Water && g_state.depthTest && g_state.gameTarget == g_state.colorTarget.Get();
    if (water) {
        const auto& waterFog = g_state.constants[kWaterFogColorRegister];
        g_state.waterFogColor = std::array<float, 3>{waterFog[0], waterFog[1], waterFog[2]};
    }
    const bool gbuffer =
        (g_state.bound.kind == ShaderKind::GBuffer || water) && g_state.depthTest && g_state.gameTarget == g_state.colorTarget.Get();
    if (g_state.bound.kind == ShaderKind::GBuffer && !gbuffer && !g_state.loggedOtherTarget) {
        g_state.loggedOtherTarget = true;
        log::Info("G-buffer: a G-buffer draw went to another render target during a capture; it is drawn there unlit");
    }
    // Multiplying decals (DecalModulate's mod2x blend, in gamma space with grey neutral) darken the surface under
    // them, so they multiply its albedo; the composite would cover them in the game's target.
    const bool modulate = !gbuffer && g_state.bound.kind == ShaderKind::Other && g_state.blending && g_state.depthTest &&
        g_state.gameTarget == g_state.colorTarget.Get() && g_state.sourceBlend == D3DBLEND_DESTCOLOR && g_state.destinationBlend == D3DBLEND_SRCCOLOR;
    BindTargets(device, gbuffer ? Targets::GBuffer : modulate ? Targets::Albedo : Targets::Game);
    if (gbuffer) {
        SetDeviceState(device, D3DRS_SRGBWRITEENABLE, g_state.srgbWrite, FALSE);
        SetDeviceState(device, D3DRS_COLORWRITEENABLE2, g_state.emissiveWrites, g_state.bound.emissive ? kColorWriteRgb : 0);
    } else if (modulate) {
        SetDeviceState(device, D3DRS_SRGBWRITEENABLE, g_state.srgbWrite, FALSE);
    } else {
        SetDeviceState(device, D3DRS_SRGBWRITEENABLE, g_state.srgbWrite, g_state.srgbWrite.game);
    }
    if (g_state.bound.kind == ShaderKind::ProjectedShadow)
        return DrawAction::Skip;
    // Additive passes (emissive glows, sprites) add light rather than surface colour, so they go to the light
    // target, which is added after lighting without being lit again. VertexLitGeneric has no lighting preview
    // variant of its flashlight pass, so the game draws those with no pixel shader (fixed-function white); the
    // flashlight already reaches those models through the projected light the other passes captured.
    if (g_state.bound.kind == ShaderKind::Other && g_state.blending && g_state.destinationBlend == D3DBLEND_ONE &&
        (g_state.sourceBlend == D3DBLEND_ONE || g_state.sourceBlend == D3DBLEND_SRCALPHA))
        return g_state.requested != nullptr ? DrawAction::RedirectToLight : DrawAction::Skip;
    // Flashlight passes are lit by ray tracing from the light they describe. Their light cannot be kept: VertexLitGeneric
    // has none in the lighting preview, and light added to a surface would show through props drawn over it later.
    if (g_state.bound.kind == ShaderKind::Flashlight) {
        CaptureProjectedLight();
        return DrawAction::Skip;
    }
    if (gbuffer && g_state.bound.detailMode >= 0)
        SetDetailMode(device);
    if (gbuffer) {
        // The game's sRGB state for its own lit surfaces says how the back buffer is encoded.
        ++(g_state.srgbWrite.game ? g_state.srgbDraws : g_state.linearDraws);
        for (int r = 0; r < 4; ++r)
            g_state.viewProj[r] = g_state.vertexConstants[kViewProjection + r];
    }
    return DrawAction::Draw;
}

// Additive passes add light on top of the base pass. Sending them to their own target keeps the G-buffer albedo
// clean; the lighting pass adds the result back.
void BeginLightRedirect(IDirect3DDevice9* device)
{
    OwnCall([&] {
        device->SetRenderTarget(0, g_state.lightSurface.Get());
        device->SetViewport(&g_state.viewport);
    });
}

void EndLightRedirect(IDirect3DDevice9* device)
{
    OwnCall([&] {
        device->SetRenderTarget(0, g_state.gameTarget);
        device->SetViewport(&g_state.viewport);
    });
}

void OnRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
{
    if (g_state.ownCall || !g_state.active || index != 0)
        return;
    g_state.gameTarget = surface;
    // The game replaced the albedo target; its own target must not be paired with the G-buffer's.
    if (g_state.boundTargets != Targets::Game) {
        g_state.boundTargets = Targets::Game;
        OwnCall([&] {
            device->SetRenderTarget(kNormalTarget, nullptr);
            device->SetRenderTarget(kEmissiveTarget, nullptr);
            device->SetRenderTarget(kFlagsTarget, nullptr);
        });
    }
}

void OnViewport(const D3DVIEWPORT9& viewport) { g_state.viewport = viewport; }

HRESULT CreateVertexShader(IDirect3DDevice9* device, const DWORD* code, IDirect3DVertexShader9** shader, CreateVertexShaderFn create)
{
    HRESULT hr = create(device, code, shader);
    if (FAILED(hr))
        return hr;
    const size_t tokens = code != nullptr ? bytecode::TokenCount(code) : 0;
    g_state.vertexShaders[*shader] = ClassifyVertexShader(tokens != 0 ? std::span<const DWORD>(code, tokens) : std::span<const DWORD>(), kTrackedVertexRegisters);
    return hr;
}

void OnVertexShader(IDirect3DVertexShader9* shader)
{
    auto info = g_state.vertexShaders.find(shader);
    const VertexShaderClass bound = info != g_state.vertexShaders.end() ? info->second : VertexShaderClass{};
    g_state.boundVertex = bound.flashlight;
    g_state.boundSwayRegister = bound.swayRegister;
}

void OnVertexShaderConstants(UINT start, const float* data, UINT count)
{
    if (data == nullptr)
        return;
    for (UINT r = start; r < start + count && r < kTrackedVertexRegisters; ++r)
        std::memcpy(g_state.vertexConstants[r].data(), data + (r - start) * 4, sizeof(float) * 4);
}

const float* JitterVertexConstants(UINT start, const float* data, UINT count, std::vector<float>& scratch)
{
    if (!g_state.active || (g_state.jitter[0] == 0.0f && g_state.jitter[1] == 0.0f) || start >= kModelViewProjection + kProjectionRegisters ||
        start + count <= kModelViewProjection)
        return data;
    scratch.assign(data, data + count * 4);
    // Shifting clip x/y by jitter * w moves the image by a constant NDC offset: row0 += jx * row3, row1 += jy * row3.
    for (UINT base : {kModelViewProjection, kViewProjection}) {
        const auto& row3 = g_state.vertexConstants[base + 3];
        for (UINT axis = 0; axis < 2; ++axis) {
            const UINT reg = base + axis;
            if (reg < start || reg >= start + count)
                continue;
            for (int c = 0; c < 4; ++c)
                scratch[(reg - start) * 4 + c] = g_state.vertexConstants[reg][c] + g_state.jitter[axis] * row3[c];
        }
    }
    return scratch.data();
}

const Matrix4& LastCaptureViewProj() { return g_state.viewProj; }

void SetJitter(float x, float y)
{
    g_state.jitter[0] = x;
    g_state.jitter[1] = y;
}

std::array<float, 2> Jitter() { return {g_state.jitter[0], g_state.jitter[1]}; }

void OnTexture(DWORD stage, IDirect3DBaseTexture9* texture)
{
    if (stage < kTextureStages && !g_state.ownCall)
        g_state.textures[stage] = texture;
}

void OnSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    if (sampler >= kTextureStages || g_state.ownCall)
        return;
    for (size_t i = 0; i < kTrackedSamplerStates; ++i) {
        if (kProbeSamplerStates[i] == type)
            g_state.samplerStates[sampler][i] = value;
    }
}

std::array<IDirect3DVolumeTexture9*, 3> ProbeTextures()
{
    return {g_state.probeTextures[0].Get(), g_state.probeTextures[1].Get(), g_state.probeTextures[2].Get()};
}

void BeginForward(IDirect3DDevice9* device, const ForwardConstants& constants)
{
    if (!g_state.probeTextures[0])
        return;
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
    g_state.forwardActive = engine::SetFixedLighting(2);
    engine::SetUnlitFilter(g_state.forwardActive);
    g_state.forwardDraws = 0;
    // The pixel shader bound now was chosen before forward lighting began.
    device->SetPixelShader(g_state.requested);
}

void EndForward(IDirect3DDevice9* device)
{
    if (!g_state.forwardActive)
        return;
    g_state.forwardActive = false;
    engine::SetUnlitFilter(false);
    engine::SetFixedLighting(0);
    device->SetPixelShader(g_state.requested);
    if (g_state.forwardLogged < 3) {
        ++g_state.forwardLogged;
        log::Info("G-buffer: {} forward-lit draws ({} preview shaders have a forward variant, {} could not take one)", g_state.forwardDraws,
            ShaderStatistics().forward, ShaderStatistics().forwardFailures + g_state.forwardFailures);
    }
}

void BeginForwardDraw(IDirect3DDevice9* device)
{
    if (g_state.bound.detailMode >= 0)
        SetDetailMode(device);
    ++g_state.forwardDraws;
    // Additive draws (glows, sprites) add light; they are shown as drawn.
    g_state.forwardConstants[0][0] = g_state.blending && g_state.destinationBlend == D3DBLEND_ONE ? 1.0f : 0.0f;
    OwnCall([&] {
        for (UINT i = 0; i < kForwardConstantCount; ++i)
            device->SetPixelShaderConstantF(g_state.bound.forwardUniforms[i], g_state.forwardConstants[i].data(), 1);
        for (size_t i = 0; i < 3; ++i) {
            const UINT stage = g_state.bound.probeSamplers[i];
            device->SetTexture(stage, g_state.probeTextures[i].Get());
            for (size_t s = 0; s < kTrackedSamplerStates; ++s)
                device->SetSamplerState(stage, kProbeSamplerStates[s], kProbeSamplerValues[s]);
        }
    });
}

void EndForwardDraw(IDirect3DDevice9* device)
{
    OwnCall([&] {
        // The game caches the constants it set; registers a ps_2_b variant borrowed get the game's values back.
        for (UINT r : g_state.bound.forwardUniforms) {
            if (r < kTrackedRegisters)
                device->SetPixelShaderConstantF(r, g_state.constants[r].data(), 1);
        }
        for (size_t i = 0; i < 3; ++i) {
            const UINT stage = g_state.bound.probeSamplers[i];
            device->SetTexture(stage, g_state.textures[stage]);
            for (size_t s = 0; s < kTrackedSamplerStates; ++s)
                device->SetSamplerState(stage, kProbeSamplerStates[s], g_state.samplerStates[stage][s]);
        }
    });
}

std::span<const ProjectedLight> ProjectedLights() { return g_state.lastProjected; }

std::span<const ComPtr<IDirect3DBaseTexture9>> ProjectedCookies() { return g_state.lastCookies; }

const SwayClock& LastSwayClock() { return g_state.swayClock; }

void OnRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
    TrackedState* tracked = state == D3DRS_SRGBWRITEENABLE ? &g_state.srgbWrite : state == D3DRS_COLORWRITEENABLE2 ? &g_state.emissiveWrites : nullptr;
    if (tracked != nullptr) {
        tracked->device = value;
        if (!g_state.ownCall)
            tracked->game = value;
    }
    if (state == D3DRS_ZENABLE)
        g_state.depthTest = value != D3DZB_FALSE;
    else if (state == D3DRS_ALPHABLENDENABLE)
        g_state.blending = value != FALSE;
    else if (state == D3DRS_SRCBLEND)
        g_state.sourceBlend = value;
    else if (state == D3DRS_DESTBLEND)
        g_state.destinationBlend = value;
}

void OnPixelShaderConstants(UINT start, const float* data, UINT count)
{
    if (data == nullptr || g_state.ownCall)
        return;
    for (UINT r = start; r < start + count && r < kTrackedRegisters; ++r)
        std::memcpy(g_state.constants[r].data(), data + (r - start) * 4, sizeof(float) * 4);
}

bool Begin(IDirect3DDevice9* device)
{
    if (g_state.active || !g_state.normalSurface)
        return g_state.active;
    if (!engine::SetFixedLighting(2))
        return false;
    for (IDirect3DSurface9* target : {g_state.albedoSurface.Get(), g_state.emissiveSurface.Get(), g_state.normalSurface.Get(),
             g_state.flagsSurface.Get(), g_state.lightSurface.Get()})
        device->ColorFill(target, nullptr, 0);
    g_state.colorTarget.Reset();
    g_state.waterFogColor.reset();
    device->GetRenderTarget(0, &g_state.colorTarget);
    g_state.gameTarget = g_state.colorTarget.Get();
    g_state.boundTargets = Targets::Game;
    g_state.active = true;
    ++g_state.sections;
    // The view's projection was set before the capture began; send it again so it picks up the jitter.
    device->SetVertexShaderConstantF(kModelViewProjection, g_state.vertexConstants[kModelViewProjection].data(), kProjectionRegisters);
    g_state.projected.clear();
    g_state.projectedCookies.clear();
    g_state.srgbDraws = 0;
    g_state.linearDraws = 0;
    device->SetPixelShader(g_state.requested);
    if (!g_state.loggedCounts) {
        g_state.loggedCounts = true;
        log::Info("G-buffer: {} preview shaders patched, {} model shaders replaced so far", ShaderStatistics().patched, ShaderStatistics().replaced);
    }
    return true;
}

void End(IDirect3DDevice9* device, ViewKind kind)
{
    if (!g_state.active)
        return;
    if (kind != ViewKind::Viewmodel) {
        // Every lit shader's vertex fog constant holds the view's range fog (cFogParams: end / range, 1,
        // 1 - max density, 1 / range) and pixel c29 its colour, even in the preview shaders that do not fog.
        // Source writes that constant only when fog is on, so in a view without fog it still holds the last
        // fogged view's (gm_publicindoorswimmingpool, which has none, turned black a few hundred units away);
        // zero density stands for no fog.
        const auto& range = g_state.vertexConstants[kVertexFogRegister];
        const auto& color = g_state.constants[kFogColorRegister];
        Fog& fog = g_state.fog[kind == ViewKind::Skybox ? 1 : 0];
        const std::optional<engine::FogState> current = engine::CurrentFog();
        fog.params = current && current->mode != 0 ? std::array<float, 4>{range[0] - 1.0f, 0.0f, 1.0f - range[2], range[3]} : std::array<float, 4>{};
        fog.color = {color[0], color[1], color[2]};
        // Under water the view's fog is the water's: Source blends it in linearly (PIXEL_FOG_TYPE_HEIGHT), where its range
        // fog over air squares the factor. The view is under water when its fog colour is the water surface's.
        if (current && current->mode != 0 && g_state.waterFogColor) {
            bool same = true;
            for (int c = 0; c < 3; ++c)
                same = same && std::fabs(std::pow(current->color[c] / 255.0f, 2.2f) - (*g_state.waterFogColor)[c]) < 0.01f;
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
            for (size_t i = 0; i < g_state.projected.size(); ++i) {
                const ProjectedLight& light = g_state.projected[i];
                const auto& m = light.worldToTexture;
                log::Info("  origin ({:.1f} {:.1f} {:.1f}) far {:.1f} color ({:.3f} {:.3f} {:.3f}) attenuation ({:.3f} {:.3f} {:.3f}) cookie {}; rows "
                          "({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f})",
                    light.origin[0], light.origin[1], light.origin[2], light.farZ, light.color[0], light.color[1], light.color[2], light.attenuation[0],
                    light.attenuation[1], light.attenuation[2], static_cast<void*>(g_state.projectedCookies[i].Get()), m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1],
                    m[1][2], m[1][3], m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
            }
        }
        g_state.lastProjected = std::move(g_state.projected);
        g_state.lastCookies = std::move(g_state.projectedCookies);
    }
    g_state.flashlightDraws = 0;
    g_state.projected.clear();
    g_state.projectedCookies.clear();
    engine::SetFixedLighting(0);
    BindTargets(device, Targets::Game);
    SetDeviceState(device, D3DRS_SRGBWRITEENABLE, g_state.srgbWrite, g_state.srgbWrite.game);
    SetDeviceState(device, D3DRS_COLORWRITEENABLE2, g_state.emissiveWrites, g_state.emissiveWrites.game);
    g_state.active = false;
    device->SetPixelShader(g_state.requested);
    device->SetVertexShaderConstantF(kModelViewProjection, g_state.vertexConstants[kModelViewProjection].data(), kProjectionRegisters);
    if (g_state.srgbDraws + g_state.linearDraws == 0)
        return;
    const bool srgb = g_state.srgbDraws >= g_state.linearDraws;
    if (srgb != g_state.outputSrgb || !g_state.loggedSrgb) {
        g_state.loggedSrgb = true;
        log::Info("G-buffer: the game draws lit surfaces with sRGB {} ({} sRGB draws, {} linear draws)", srgb ? "on" : "off", g_state.srgbDraws,
            g_state.linearDraws);
        g_state.outputSrgb = srgb;
    }
}

bool Active() { return g_state.active; }

IDirect3DTexture9* AlbedoTexture() { return g_state.albedo.Get(); }

IDirect3DTexture9* EmissiveTexture() { return g_state.emissive.Get(); }

IDirect3DTexture9* NormalTexture() { return g_state.normal.Get(); }

IDirect3DTexture9* FlagsTexture() { return g_state.flags.Get(); }

IDirect3DTexture9* LightTexture() { return g_state.light.Get(); }

float TonemapScale()
{
    const float scale = g_state.constants[kLightScaleRegister][0];
    return scale > 0.0f ? scale : 1.0f;
}

const Fog& ViewFog(ViewKind kind) { return g_state.fog[kind == ViewKind::Skybox ? 1 : 0]; }

bool OutputIsSrgb() { return g_state.outputSrgb; }

}
