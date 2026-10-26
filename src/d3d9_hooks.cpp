#include "d3d9_hooks.h"

#include "capture.h"
#include "d3d9_vtable.h"
#include "effects.h"
#include "frame_trace.h"
#include "log.h"
#include "mod.h"
#include "presenter.h"
#include "renderer.h"
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

HWND PresentFrame(IDirect3DDevice9* device, HWND requestedWindow)
{
    static uint64_t frame = 0;
    ++frame;
    const Options& options = GetOptions();
    if (options.devMode && frame % 30 == 0) {
        std::filesystem::path request = options.gameDirectory / L"gmodpp_capture.request";
        if (DeleteFileW(request.c_str()))
            CaptureBackBuffer(device, options.gameDirectory / std::format(L"gmodpp_capture_{}.bmp", frame));
    }
    Renderer* renderer = GetRenderer();
    renderer->OnPresent();
    effects::OnPresent();
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
        GetRenderer()->OnVertexShaderConstants(start, data, count);
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

GMODPP_DEVICE_HOOK(SetRenderTarget, (IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface), (self, index, surface),
    trace::Line("SetRenderTarget {} {}", index, trace::Describe(surface)))
GMODPP_DEVICE_HOOK(SetDepthStencilSurface, (IDirect3DDevice9* self, IDirect3DSurface9* surface), (self, surface),
    trace::Line("SetDepthStencilSurface {}", trace::Describe(surface)))
GMODPP_DEVICE_HOOK(SetViewport, (IDirect3DDevice9* self, const D3DVIEWPORT9* vp), (self, vp),
    trace::Line("SetViewport {} {} {}x{} z {}..{}", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ))
GMODPP_DEVICE_HOOK(Clear, (IDirect3DDevice9* self, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil),
    (self, count, rects, flags, color, z, stencil), trace::Line("Clear rects {} flags {:#x} color {:#010x} z {} stencil {}", count, flags, color, z, stencil))
GMODPP_DEVICE_HOOK(BeginScene, (IDirect3DDevice9* self), (self), trace::Line("BeginScene"))
GMODPP_DEVICE_HOOK(EndScene, (IDirect3DDevice9* self), (self), trace::Line("EndScene"))
GMODPP_DEVICE_HOOK(DrawPrimitive, (IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT start, UINT count), (self, type, start, count),
    trace::Draw("DrawPrimitive", count))
GMODPP_DEVICE_HOOK(DrawIndexedPrimitive, (IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex, UINT numVertices, UINT startIndex, UINT count),
    (self, type, baseVertex, minVertex, numVertices, startIndex, count), trace::Draw("DrawIndexedPrimitive", count))
GMODPP_DEVICE_HOOK(DrawPrimitiveUP, (IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride), (self, type, count, data, stride),
    trace::Draw("DrawPrimitiveUP", count))
GMODPP_DEVICE_HOOK(DrawIndexedPrimitiveUP, (IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT minVertex, UINT numVertices, UINT count, const void* indices, D3DFORMAT indexFormat, const void* vertices, UINT stride),
    (self, type, minVertex, numVertices, count, indices, indexFormat, vertices, stride), trace::Draw("DrawIndexedPrimitiveUP", count))
GMODPP_DEVICE_HOOK(StretchRect, (IDirect3DDevice9* self, IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst, const RECT* dstRect, D3DTEXTUREFILTERTYPE filter),
    (self, src, srcRect, dst, dstRect, filter), trace::Line("StretchRect {} -> {}", trace::Describe(src), trace::Describe(dst)))
GMODPP_DEVICE_HOOK(SetVertexShader, (IDirect3DDevice9* self, IDirect3DVertexShader9* shader), (self, shader),
    trace::Line("SetVertexShader {}", static_cast<void*>(shader)))
GMODPP_DEVICE_HOOK(SetPixelShader, (IDirect3DDevice9* self, IDirect3DPixelShader9* shader), (self, shader),
    trace::Line("SetPixelShader {}", static_cast<void*>(shader)))

#undef GMODPP_DEVICE_HOOK

void OnGameDeviceCreated(IDirect3DDevice9* device, HWND focusWindow, const D3DPRESENT_PARAMETERS& params)
{
    Renderer* renderer = GetRenderer();
    if (renderer == nullptr || !renderer->AttachGameDevice(device, focusWindow, params))
        return;

    Patch(device, kSlotDeviceReset, reinterpret_cast<void*>(&HookReset));
    Patch(device, kSlotDevicePresent, reinterpret_cast<void*>(&HookPresent));
    Patch(device, kSlotDeviceSetVertexShaderConstantF, reinterpret_cast<void*>(&HookSetVertexShaderConstantF));
    Patch(device, kSlotDeviceCreateDepthStencilSurface, reinterpret_cast<void*>(&HookCreateDepthStencilSurface));
    if (GetOptions().devMode) {
        Patch(device, kSlotDeviceSetRenderTarget, reinterpret_cast<void*>(&HookSetRenderTarget));
        Patch(device, kSlotDeviceSetDepthStencilSurface, reinterpret_cast<void*>(&HookSetDepthStencilSurface));
        Patch(device, kSlotDeviceSetViewport, reinterpret_cast<void*>(&HookSetViewport));
        Patch(device, kSlotDeviceClear, reinterpret_cast<void*>(&HookClear));
        Patch(device, kSlotDeviceBeginScene, reinterpret_cast<void*>(&HookBeginScene));
        Patch(device, kSlotDeviceEndScene, reinterpret_cast<void*>(&HookEndScene));
        Patch(device, kSlotDeviceDrawPrimitive, reinterpret_cast<void*>(&HookDrawPrimitive));
        Patch(device, kSlotDeviceDrawIndexedPrimitive, reinterpret_cast<void*>(&HookDrawIndexedPrimitive));
        Patch(device, kSlotDeviceDrawPrimitiveUP, reinterpret_cast<void*>(&HookDrawPrimitiveUP));
        Patch(device, kSlotDeviceDrawIndexedPrimitiveUP, reinterpret_cast<void*>(&HookDrawIndexedPrimitiveUP));
        Patch(device, kSlotDeviceStretchRect, reinterpret_cast<void*>(&HookStretchRect));
        Patch(device, kSlotDeviceSetVertexShader, reinterpret_cast<void*>(&HookSetVertexShader));
        Patch(device, kSlotDeviceSetPixelShader, reinterpret_cast<void*>(&HookSetPixelShader));
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
