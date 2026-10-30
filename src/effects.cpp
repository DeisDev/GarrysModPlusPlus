#include "effects.h"

#include "gbuffer.h"
#include "log.h"
#include "mod.h"
#include "renderer.h"
#include "settings.h"
#include "world.h"

#include <d3d9on12.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp::effects {

namespace {

const D3DFORMAT kIntzFormat = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

struct State {
    IDirect3DDevice9* device = nullptr;
    ComPtr<IDirect3DDevice9On12> device9On12;
    // Flushes Direct3D 9's commands to the shared queue before lighting reads what they drew.
    ComPtr<IDirect3DQuery9> flushQuery;
    bool lighting = false;
    ComPtr<IDirect3DTexture9> depthTexture;
    ComPtr<IDirect3DSurface9> depthSurface;
    bool depthReplaced = false;
    bool gbufferReady = false;
    DWORD renderThread = 0;
    bool warnedQueuedRendering = false;
    bool viewmodelCapture = false;
    D3DVIEWPORT9 viewmodelViewport = {};
};

State g_state;

std::filesystem::path ModuleDirectory()
{
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module) ||
        GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return {};
    return std::filesystem::path(path).parent_path();
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
        hr = device->QueryInterface(IID_PPV_ARGS(&g_state.device9On12));
        if (SUCCEEDED(hr))
            hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &g_state.flushQuery);
        if (FAILED(hr)) {
            log::Info("Lighting: initialization failed ({:#010x})", Hex(hr));
            return;
        }
        g_state.lighting =
            world::Initialize(renderer->Device(), renderer->Queue(), renderer->Descriptors(), renderer->SupportsOpacityMicromaps(), ModuleDirectory());
    }
}

void OnBeforeReset()
{
    if (g_state.lighting)
        world::WaitForGpu();
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
    if (g_state.viewmodelCapture) {
        g_state.viewmodelCapture = false;
        FinishCapture(gbuffer::ViewKind::Viewmodel);
    }
    if (gbuffer::Active() && g_state.device != nullptr) {
        log::Info("Effects: a G-buffer capture was still open at present; closing it");
        gbuffer::End(g_state.device, gbuffer::ViewKind::Main);
    }
    if (g_state.device != nullptr)
        gbuffer::EndForward(g_state.device);
    D3DSURFACE_DESC desc = {};
    ComPtr<IDirect3DSurface9> backBuffer;
    if (g_state.device != nullptr && SUCCEEDED(g_state.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
        backBuffer->GetDesc(&desc);
    world::OnPresent(desc.Width, desc.Height, true);
}

namespace {

// A capture can start when the game draws into the back buffer with our depth buffer on the render thread.
bool CanCapture(IDirect3DDevice9* device, ComPtr<IDirect3DSurface9>& backBuffer)
{
    if (device == nullptr || !g_state.lighting || !g_state.gbufferReady || !world::Ready() || gbuffer::Active() || !GetSettings().realtimeLighting)
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

// Lighting reads the view's Direct3D 9 resources on the queue 9on12 shares: Direct3D 9 is flushed first, and the
// resources are handed back with the fence value lighting's commands signal. Returns what the view's translucent
// surfaces are lit with, if anything.
std::optional<gbuffer::ForwardConstants> RunLighting(gbuffer::ViewKind kind, IDirect3DSurface9* backBuffer)
{
    g_state.flushQuery->Issue(D3DISSUE_END);
    g_state.flushQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    ID3D12CommandQueue* queue = GetRenderer()->Queue();
    std::vector<IDirect3DResource9*> unwrapped9;
    std::vector<ComPtr<ID3D12Resource>> unwrapped;
    auto unwrap = [&](IDirect3DResource9* resource) -> ID3D12Resource* {
        if (resource == nullptr)
            return nullptr;
        for (size_t i = 0; i < unwrapped9.size(); ++i) {
            if (unwrapped9[i] == resource)
                return unwrapped[i].Get();
        }
        ComPtr<ID3D12Resource> result;
        if (FAILED(g_state.device9On12->UnwrapUnderlyingResource(resource, queue, IID_PPV_ARGS(&result))))
            return nullptr;
        unwrapped9.push_back(resource);
        unwrapped.push_back(result);
        return result.Get();
    };
    auto returnAll = [&] {
        UINT64 value = world::FenceValue();
        ID3D12Fence* fence = world::Fence();
        for (IDirect3DResource9* resource : unwrapped9)
            g_state.device9On12->ReturnUnderlyingResource(resource, 1, &value, &fence);
    };

    IDirect3DResource9* const targets9[] = {backBuffer, g_state.depthTexture.Get(), gbuffer::NormalTexture(), gbuffer::FlagsTexture(), gbuffer::LightTexture(),
        gbuffer::AlbedoTexture(), gbuffer::EmissiveTexture()};
    std::array<ID3D12Resource*, std::size(targets9)> targets = {};
    for (size_t i = 0; i < targets.size(); ++i) {
        targets[i] = unwrap(targets9[i]);
        if (targets[i] == nullptr) {
            log::Info("Lighting: unwrapping input {} failed; lighting disabled", i);
            returnAll();
            world::Shutdown();
            g_state.lighting = false;
            return std::nullopt;
        }
    }
    ViewInputs inputs{targets[0], targets[1], targets[2], targets[3], targets[4], targets[5], targets[6], gbuffer::ProjectedLights(), {}, kind, 0.0f, 1.0f};
    // A cookie that cannot be unwrapped falls back to a round spot.
    std::vector<ID3D12Resource*> cookies;
    if (kind != gbuffer::ViewKind::Skybox && GetSettings().projectedLights) {
        for (const ComPtr<IDirect3DBaseTexture9>& cookie : gbuffer::ProjectedCookies())
            cookies.push_back(unwrap(cookie.Get()));
        inputs.cookies = cookies;
    }
    if (kind == gbuffer::ViewKind::Main && GetSettings().translucentLighting) {
        const std::array<IDirect3DVolumeTexture9*, 3> probes = gbuffer::ProbeTextures();
        for (size_t i = 0; i < probes.size(); ++i)
            inputs.probes[i] = unwrap(probes[i]);
    }
    if (kind == gbuffer::ViewKind::Viewmodel) {
        inputs.depthMin = g_state.viewmodelViewport.MinZ;
        inputs.depthMax = g_state.viewmodelViewport.MaxZ;
    }
    inputs.viewProj = gbuffer::LastCaptureViewProj();
    inputs.jitter = gbuffer::Jitter();
    inputs.tonemapScale = gbuffer::TonemapScale();
    inputs.outputSrgb = gbuffer::OutputIsSrgb();
    inputs.fog = gbuffer::ViewFog(kind);
    inputs.sway = gbuffer::LastSwayClock();
    world::LightView(inputs);
    returnAll();
    return world::ForwardLighting(inputs);
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
    if (const std::optional<gbuffer::ForwardConstants> forward = RunLighting(kind, backBuffer.Get()))
        gbuffer::BeginForward(device, *forward);
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

void OnBeforeStretchRect(IDirect3DSurface9* source)
{
    if (!g_state.viewmodelCapture || source == nullptr)
        return;
    // Bloom copies the back buffer before the next viewport change; without the lit viewmodel in it, the glow of
    // what lies behind the viewmodel is added over it.
    ComPtr<IDirect3DSurface9> backBuffer;
    if (FAILED(g_state.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || backBuffer.Get() != source)
        return;
    g_state.viewmodelCapture = false;
    FinishCapture(gbuffer::ViewKind::Viewmodel);
}

void OnViewOpaqueBegin()
{
    IDirect3DDevice9* device = g_state.device;
    ComPtr<IDirect3DSurface9> backBuffer;
    if (!CanCapture(device, backBuffer))
        return;
    D3DSURFACE_DESC desc;
    backBuffer->GetDesc(&desc);
    const std::array<float, 2> jitter = world::ViewJitter(desc.Width, desc.Height);
    gbuffer::SetJitter(jitter[0], jitter[1]);
    gbuffer::Begin(device);
}

void OnViewOpaqueEnd(bool inSkybox)
{
    if (!g_state.viewmodelCapture)
        FinishCapture(inSkybox ? gbuffer::ViewKind::Skybox : gbuffer::ViewKind::Main);
}

void OnViewTranslucentEnd()
{
    if (g_state.device != nullptr)
        gbuffer::EndForward(g_state.device);
}

}
