#include "d3d9_hooks.h"

#include "capture.h"
#include "d3d9_vtable.h"
#include "effects.h"
#include "frame_trace.h"
#include "gbuffer.h"
#include "log.h"
#include "mod.h"
#include "presenter.h"
#include "renderer.h"
#include "settings.h"
#include "watchdog.h"

#include <intrin.h>

#include <array>
#include <atomic>
#include <mutex>

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace gmodpp::hooks {

namespace {

struct SlotPatch {
    void** vtable;
    int slot;
    void* original;
};

std::array<SlotPatch, 32> g_patches;
std::atomic<size_t> g_patchCount = 0;
std::mutex g_patchMutex;
std::atomic<IDirect3DDevice9*> g_gameDevice = nullptr;

void** VTable(void* object) { return *static_cast<void***>(object); }

void Patch(void* object, int slot, void* hook)
{
    std::scoped_lock lock(g_patchMutex);
    void** vtable = VTable(object);
    if (vtable[slot] == hook)
        return;
    size_t count = g_patchCount.load(std::memory_order_relaxed);
    if (count == g_patches.size()) {
        log::Info("Hook table full; cannot hook vtable slot {}", slot);
        return;
    }
    DWORD protection;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &protection)) {
        log::Info("VirtualProtect failed for vtable slot {}: {}", slot, GetLastError());
        return;
    }
    g_patches[count] = {vtable, slot, vtable[slot]};
    g_patchCount.store(count + 1, std::memory_order_release);
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), protection, &protection);
}

template <class Fn>
Fn Original(void* object, int slot)
{
    void** vtable = VTable(object);
    size_t count = g_patchCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (g_patches[i].vtable == vtable && g_patches[i].slot == slot)
            return reinterpret_cast<Fn>(g_patches[i].original);
    }
    __fastfail(FAST_FAIL_INVALID_ARG);
    return nullptr;
}

bool CalledFromShaderApi(void* returnAddress)
{
    HMODULE caller = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCWSTR>(returnAddress), &caller);
    return caller != nullptr && caller == GetModuleHandleW(L"shaderapidx9.dll");
}

bool IsGameDevice(IDirect3DDevice9* device) { return device == g_gameDevice.load(std::memory_order_relaxed); }

void RecordFrameInterval()
{
    static LARGE_INTEGER frequency = {};
    static LARGE_INTEGER previous = {};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (frequency.QuadPart == 0)
        QueryPerformanceFrequency(&frequency);
    else
        RecordFrameTime(static_cast<float>(static_cast<double>(now.QuadPart - previous.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart)));
    previous = now;
}

HWND PresentFrame(IDirect3DDevice9* device, HWND requestedWindow)
{
    static uint64_t frame = 0;
    ++frame;
    RecordFrameInterval();
    Renderer* renderer = GetRenderer();
    renderer->OnPresent();
    effects::OnPresent();
    const Options& options = GetOptions();
    if (options.devMode && frame % 30 == 0) {
        std::filesystem::path request = options.gameDirectory / L"gmodpp_capture.request";
        if (DeleteFileW(request.c_str()))
            CaptureBackBuffer(device, options.gameDirectory / std::format(L"gmodpp_capture_{}.bmp", frame));
    }
    watchdog::NotePresent();
    trace::OnFrameBoundary();

    Presenter* presenter = renderer->GetPresenter();
    if (presenter != nullptr && presenter->PresentFrame(device))
        return presenter->SinkWindow();
    return requestedWindow;
}

void LogPresentResult(HRESULT hr)
{
    static HRESULT last = S_OK;
    if (hr != last)
        log::Info("Present returned {:#010x}", static_cast<unsigned>(hr));
    last = hr;
}

void BeforeReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
{
    GetRenderer()->OnReset();
    effects::OnBeforeReset();
    if (Presenter* presenter = GetRenderer()->GetPresenter())
        presenter->BeforeReset();
    ComPtr<IDirect3D9> d3d;
    D3DDEVICE_CREATION_PARAMETERS creation;
    if (SUCCEEDED(device->GetDirect3D(&d3d)) && SUCCEEDED(device->GetCreationParameters(&creation)))
        effects::PrepareDeviceParameters(d3d.Get(), creation.AdapterOrdinal, *params);
}

void AfterReset(IDirect3DDevice9* device, HRESULT hr, const D3DPRESENT_PARAMETERS* params)
{
    log::Info("Reset to {}x{} returned {:#010x}", params->BackBufferWidth, params->BackBufferHeight, static_cast<unsigned>(hr));
    if (FAILED(hr))
        return;
    effects::OnDeviceReady(device, *params);
    if (Presenter* presenter = GetRenderer()->GetPresenter())
        presenter->AfterReset(*params);
}

void LogDeviceParameters(const char* call, HWND focusWindow, const D3DPRESENT_PARAMETERS* params)
{
    HWND window = params->hDeviceWindow ? params->hDeviceWindow : focusWindow;
    log::Info("{}: {}x{} format {} msaa {} windowed {} swap {} interval {:#x} auto depth {} ({}); window {}", call,
        params->BackBufferWidth, params->BackBufferHeight, static_cast<int>(params->BackBufferFormat), static_cast<int>(params->MultiSampleType),
        params->Windowed, static_cast<int>(params->SwapEffect), params->PresentationInterval, params->EnableAutoDepthStencil,
        static_cast<int>(params->AutoDepthStencilFormat), static_cast<void*>(window));
}

using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SetVertexShaderConstantFFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using PresentExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using ResetExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using CreateDepthStencilSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using CreateDeviceExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* params)
{
    if (!IsGameDevice(self))
        return Original<ResetFn>(self, kSlotDeviceReset)(self, params);
    BeforeReset(self, params);
    HRESULT hr = Original<ResetFn>(self, kSlotDeviceReset)(self, params);
    AfterReset(self, hr, params);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResetEx(IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode)
{
    if (!IsGameDevice(self))
        return Original<ResetExFn>(self, kSlotDeviceExResetEx)(self, params, mode);
    BeforeReset(self, params);
    HRESULT hr = Original<ResetExFn>(self, kSlotDeviceExResetEx)(self, params, mode);
    AfterReset(self, hr, params);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty)
{
    if (!IsGameDevice(self))
        return Original<PresentFn>(self, kSlotDevicePresent)(self, src, dst, window, dirty);
    HWND target = PresentFrame(self, window);
    HRESULT hr = Original<PresentFn>(self, kSlotDevicePresent)(self, src, dst, target, dirty);
    LogPresentResult(hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresentEx(IDirect3DDevice9Ex* self, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty, DWORD flags)
{
    if (!IsGameDevice(self))
        return Original<PresentExFn>(self, kSlotDeviceExPresentEx)(self, src, dst, window, dirty, flags);
    HWND target = PresentFrame(self, window);
    HRESULT hr = Original<PresentExFn>(self, kSlotDeviceExPresentEx)(self, src, dst, target, dirty, flags);
    LogPresentResult(hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSetVertexShaderConstantF(IDirect3DDevice9* self, UINT start, const float* data, UINT count)
{
    if (IsGameDevice(self) && data != nullptr) {
        gbuffer::OnVertexShaderConstants(start, data, count);
        static std::vector<float> jittered;
        data = gbuffer::JitterVertexConstants(start, data, count, jittered);
        if (trace::Active()) {
            std::string values;
            for (UINT i = 0; i < count && start + i < 20; ++i) {
                const float* v = data + i * 4;
                values += std::format(" c{}=({:.4g} {:.4g} {:.4g} {:.4g})", start + i, v[0], v[1], v[2], v[3]);
            }
            trace::Line("VSConst c{} x{}{}", start, count, values);
        }
    }
    return Original<SetVertexShaderConstantFFn>(self, kSlotDeviceSetVertexShaderConstantF)(self, start, data, count);
}

HRESULT STDMETHODCALLTYPE HookCreateDepthStencilSurface(IDirect3DDevice9* self, UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample, DWORD quality, BOOL discard, IDirect3DSurface9** surface, HANDLE* shared)
{
    HRESULT hr = Original<CreateDepthStencilSurfaceFn>(self, kSlotDeviceCreateDepthStencilSurface)(self, width, height, format, multisample, quality, discard, surface, shared);
    if (IsGameDevice(self) && GetOptions().devMode)
        log::Info("CreateDepthStencilSurface {}x{} format {:#x} msaa {} discard {} -> {:#010x}", width, height,
            static_cast<unsigned>(format), static_cast<int>(multisample), discard, static_cast<unsigned>(hr));
    return hr;
}

#define GMODPP_DEVICE_HOOK(Name, Params, Args, TraceStatement)                                         \
    using Name##Fn = HRESULT(STDMETHODCALLTYPE*) Params;                                               \
    HRESULT STDMETHODCALLTYPE Hook##Name Params                                                        \
    {                                                                                                  \
        if (trace::Active() && IsGameDevice(self))                                                     \
            TraceStatement;                                                                            \
        return Original<Name##Fn>(self, kSlotDevice##Name) Args;                                       \
    }

GMODPP_DEVICE_HOOK(SetDepthStencilSurface, (IDirect3DDevice9* self, IDirect3DSurface9* surface), (self, surface),
    trace::Line("SetDepthStencilSurface {}", trace::Describe(surface)))
GMODPP_DEVICE_HOOK(Clear, (IDirect3DDevice9* self, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil),
    (self, count, rects, flags, color, z, stencil), trace::Line("Clear rects {} flags {:#x} color {:#010x} z {} stencil {}", count, flags, color, z, stencil))
GMODPP_DEVICE_HOOK(BeginScene, (IDirect3DDevice9* self), (self), trace::Line("BeginScene"))
GMODPP_DEVICE_HOOK(EndScene, (IDirect3DDevice9* self), (self), trace::Line("EndScene"))
#undef GMODPP_DEVICE_HOOK

using StretchRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);

HRESULT STDMETHODCALLTYPE HookStretchRect(IDirect3DDevice9* self, IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst, const RECT* dstRect,
    D3DTEXTUREFILTERTYPE filter)
{
    if (IsGameDevice(self)) {
        if (trace::Active())
            trace::Line("StretchRect {} -> {}", trace::Describe(src), trace::Describe(dst));
        effects::OnBeforeStretchRect(src);
    }
    return Original<StretchRectFn>(self, kSlotDeviceStretchRect)(self, src, srcRect, dst, dstRect, filter);
}

using SetVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetSamplerStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);

HRESULT STDMETHODCALLTYPE HookSetVertexShader(IDirect3DDevice9* self, IDirect3DVertexShader9* shader)
{
    if (IsGameDevice(self)) {
        if (trace::Active())
            trace::Line("SetVertexShader {}", static_cast<void*>(shader));
        gbuffer::OnVertexShader(shader);
    }
    return Original<SetVertexShaderFn>(self, kSlotDeviceSetVertexShader)(self, shader);
}

HRESULT STDMETHODCALLTYPE HookCreateVertexShader(IDirect3DDevice9* self, const DWORD* code, IDirect3DVertexShader9** shader)
{
    auto create = Original<gbuffer::CreateVertexShaderFn>(self, kSlotDeviceCreateVertexShader);
    if (!IsGameDevice(self))
        return create(self, code, shader);
    return gbuffer::CreateVertexShader(self, code, shader, create);
}

HRESULT STDMETHODCALLTYPE HookSetTexture(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* texture)
{
    if (IsGameDevice(self))
        gbuffer::OnTexture(stage, texture);
    return Original<SetTextureFn>(self, kSlotDeviceSetTexture)(self, stage, texture);
}

HRESULT STDMETHODCALLTYPE HookSetSamplerState(IDirect3DDevice9* self, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    if (IsGameDevice(self))
        gbuffer::OnSamplerState(sampler, type, value);
    return Original<SetSamplerStateFn>(self, kSlotDeviceSetSamplerState)(self, sampler, type, value);
}

using SetViewportFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);

HRESULT STDMETHODCALLTYPE HookSetViewport(IDirect3DDevice9* self, const D3DVIEWPORT9* vp)
{
    if (IsGameDevice(self) && vp != nullptr) {
        if (trace::Active())
            trace::Line("SetViewport {} {} {}x{} z {}..{}", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
        gbuffer::OnViewport(*vp);
    }
    HRESULT hr = Original<SetViewportFn>(self, kSlotDeviceSetViewport)(self, vp);
    if (IsGameDevice(self) && vp != nullptr)
        effects::OnViewport(*vp);
    return hr;
}

using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using SetPixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPixelShaderConstantFFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using SetRenderStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);

HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface)
{
    if (IsGameDevice(self) && trace::Active())
        trace::Line("SetRenderTarget {} {}", index, trace::Describe(surface));
    HRESULT hr = Original<SetRenderTargetFn>(self, kSlotDeviceSetRenderTarget)(self, index, surface);
    if (IsGameDevice(self) && SUCCEEDED(hr))
        gbuffer::OnRenderTarget(self, index, surface);
    return hr;
}

template <class Draw>
HRESULT GameDraw(IDirect3DDevice9* self, const char* name, UINT count, Draw draw)
{
    if (!IsGameDevice(self))
        return draw();
    if (trace::Active())
        trace::Draw(name, count);
    const gbuffer::DrawAction action = gbuffer::BeforeDraw(self);
    if (action == gbuffer::DrawAction::Skip)
        return D3D_OK;
    if (action == gbuffer::DrawAction::Forward) {
        gbuffer::BeginForwardDraw(self);
        HRESULT hr = draw();
        gbuffer::EndForwardDraw(self);
        return hr;
    }
    if (action != gbuffer::DrawAction::RedirectToLight)
        return draw();
    gbuffer::BeginLightRedirect(self);
    HRESULT hr = draw();
    gbuffer::EndLightRedirect(self);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    return GameDraw(self, "DrawPrimitive", count,
        [&] { return Original<DrawPrimitiveFn>(self, kSlotDeviceDrawPrimitive)(self, type, start, count); });
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex, UINT numVertices,
    UINT startIndex, UINT count)
{
    return GameDraw(self, "DrawIndexedPrimitive", count, [&] {
        return Original<DrawIndexedPrimitiveFn>(self, kSlotDeviceDrawIndexedPrimitive)(self, type, baseVertex, minVertex, numVertices, startIndex, count);
    });
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride)
{
    return GameDraw(self, "DrawPrimitiveUP", count,
        [&] { return Original<DrawPrimitiveUPFn>(self, kSlotDeviceDrawPrimitiveUP)(self, type, count, data, stride); });
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT minVertex, UINT numVertices, UINT count,
    const void* indices, D3DFORMAT indexFormat, const void* vertices, UINT stride)
{
    return GameDraw(self, "DrawIndexedPrimitiveUP", count, [&] {
        return Original<DrawIndexedPrimitiveUPFn>(self, kSlotDeviceDrawIndexedPrimitiveUP)(self, type, minVertex, numVertices, count, indices,
            indexFormat, vertices, stride);
    });
}

HRESULT STDMETHODCALLTYPE HookSetPixelShader(IDirect3DDevice9* self, IDirect3DPixelShader9* shader)
{
    if (IsGameDevice(self)) {
        if (trace::Active())
            trace::Line("SetPixelShader {}", static_cast<void*>(shader));
        shader = gbuffer::SubstitutePixelShader(shader);
    }
    return Original<SetPixelShaderFn>(self, kSlotDeviceSetPixelShader)(self, shader);
}

HRESULT STDMETHODCALLTYPE HookCreatePixelShader(IDirect3DDevice9* self, const DWORD* code, IDirect3DPixelShader9** shader)
{
    auto create = Original<gbuffer::CreatePixelShaderFn>(self, kSlotDeviceCreatePixelShader);
    if (!IsGameDevice(self))
        return create(self, code, shader);
    return gbuffer::CreatePixelShader(self, code, shader, create);
}

HRESULT STDMETHODCALLTYPE HookSetPixelShaderConstantF(IDirect3DDevice9* self, UINT start, const float* data, UINT count)
{
    if (IsGameDevice(self))
        gbuffer::OnPixelShaderConstants(start, data, count);
    return Original<SetPixelShaderConstantFFn>(self, kSlotDeviceSetPixelShaderConstantF)(self, start, data, count);
}

HRESULT STDMETHODCALLTYPE HookSetRenderState(IDirect3DDevice9* self, D3DRENDERSTATETYPE state, DWORD value)
{
    if (IsGameDevice(self))
        gbuffer::OnRenderState(state, value);
    return Original<SetRenderStateFn>(self, kSlotDeviceSetRenderState)(self, state, value);
}

void OnGameDeviceCreated(IDirect3DDevice9* device, HWND focusWindow, const D3DPRESENT_PARAMETERS& params)
{
    Renderer* renderer = GetRenderer();
    if (renderer == nullptr || !renderer->AttachGameDevice(device, focusWindow, params))
        return;

    Patch(device, kSlotDeviceReset, reinterpret_cast<void*>(&HookReset));
    Patch(device, kSlotDevicePresent, reinterpret_cast<void*>(&HookPresent));
    Patch(device, kSlotDeviceSetVertexShaderConstantF, reinterpret_cast<void*>(&HookSetVertexShaderConstantF));
    Patch(device, kSlotDeviceCreateDepthStencilSurface, reinterpret_cast<void*>(&HookCreateDepthStencilSurface));
    Patch(device, kSlotDeviceCreatePixelShader, reinterpret_cast<void*>(&HookCreatePixelShader));
    Patch(device, kSlotDeviceSetPixelShader, reinterpret_cast<void*>(&HookSetPixelShader));
    Patch(device, kSlotDeviceSetPixelShaderConstantF, reinterpret_cast<void*>(&HookSetPixelShaderConstantF));
    Patch(device, kSlotDeviceSetRenderState, reinterpret_cast<void*>(&HookSetRenderState));
    Patch(device, kSlotDeviceDrawPrimitive, reinterpret_cast<void*>(&HookDrawPrimitive));
    Patch(device, kSlotDeviceDrawIndexedPrimitive, reinterpret_cast<void*>(&HookDrawIndexedPrimitive));
    Patch(device, kSlotDeviceSetViewport, reinterpret_cast<void*>(&HookSetViewport));
    Patch(device, kSlotDeviceSetVertexShader, reinterpret_cast<void*>(&HookSetVertexShader));
    Patch(device, kSlotDeviceCreateVertexShader, reinterpret_cast<void*>(&HookCreateVertexShader));
    Patch(device, kSlotDeviceSetTexture, reinterpret_cast<void*>(&HookSetTexture));
    Patch(device, kSlotDeviceSetSamplerState, reinterpret_cast<void*>(&HookSetSamplerState));
    Patch(device, kSlotDeviceSetRenderTarget, reinterpret_cast<void*>(&HookSetRenderTarget));
    Patch(device, kSlotDeviceDrawPrimitiveUP, reinterpret_cast<void*>(&HookDrawPrimitiveUP));
    Patch(device, kSlotDeviceDrawIndexedPrimitiveUP, reinterpret_cast<void*>(&HookDrawIndexedPrimitiveUP));
    Patch(device, kSlotDeviceStretchRect, reinterpret_cast<void*>(&HookStretchRect));
    if (GetOptions().devMode) {
        Patch(device, kSlotDeviceSetDepthStencilSurface, reinterpret_cast<void*>(&HookSetDepthStencilSurface));
        Patch(device, kSlotDeviceClear, reinterpret_cast<void*>(&HookClear));
        Patch(device, kSlotDeviceBeginScene, reinterpret_cast<void*>(&HookBeginScene));
        Patch(device, kSlotDeviceEndScene, reinterpret_cast<void*>(&HookEndScene));
    }

    IDirect3DDevice9Ex* deviceEx = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&deviceEx)))) {
        Patch(deviceEx, kSlotDeviceExPresentEx, reinterpret_cast<void*>(&HookPresentEx));
        Patch(deviceEx, kSlotDeviceExResetEx, reinterpret_cast<void*>(&HookResetEx));
        deviceEx->Release();
    }
    g_gameDevice.store(device, std::memory_order_relaxed);
    if (GetOptions().devMode)
        watchdog::Start();
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
    D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** device)
{
    const bool fromGame = CalledFromShaderApi(_ReturnAddress());
    if (fromGame)
        effects::PrepareDeviceParameters(self, adapter, *params);
    HRESULT hr = Original<CreateDeviceFn>(self, kSlotD3D9CreateDevice)(self, adapter, type, window, flags, params, device);
    if (SUCCEEDED(hr) && fromGame) {
        LogDeviceParameters("CreateDevice", window, params);
        OnGameDeviceCreated(*device, window, *params);
        effects::OnDeviceReady(*device, *params);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateDeviceEx(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
    D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** device)
{
    const bool fromGame = CalledFromShaderApi(_ReturnAddress());
    if (fromGame)
        effects::PrepareDeviceParameters(self, adapter, *params);
    HRESULT hr = Original<CreateDeviceExFn>(self, kSlotD3D9ExCreateDeviceEx)(self, adapter, type, window, flags, params, mode, device);
    if (SUCCEEDED(hr) && fromGame) {
        LogDeviceParameters("CreateDeviceEx", window, params);
        OnGameDeviceCreated(*device, window, *params);
        effects::OnDeviceReady(*device, *params);
    }
    return hr;
}

}

void InstallInterfaceHooks(IDirect3D9* d3d)
{
    Patch(d3d, kSlotD3D9CreateDevice, reinterpret_cast<void*>(&HookCreateDevice));
}

void InstallInterfaceHooks(IDirect3D9Ex* d3d)
{
    Patch(d3d, kSlotD3D9CreateDevice, reinterpret_cast<void*>(&HookCreateDevice));
    Patch(d3d, kSlotD3D9ExCreateDeviceEx, reinterpret_cast<void*>(&HookCreateDeviceEx));
}

}
