#include "d3d9_hooks.h"
#include "log.h"
#include "mod.h"
#include "renderer.h"

#include <d3d9on12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace gmodpp;

namespace {

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

bool DefaultAdapterLuid(UINT sdkVersion, LUID& luid)
{
    auto create = SystemProc<decltype(&Direct3DCreate9Ex)>("Direct3DCreate9Ex");
    ComPtr<IDirect3D9Ex> d3d;
    HRESULT hr = create ? create(sdkVersion, &d3d) : E_NOINTERFACE;
    if (SUCCEEDED(hr))
        hr = d3d->GetAdapterLUID(D3DADAPTER_DEFAULT, &luid);
    if (FAILED(hr))
        log::Info("Could not query the default Direct3D 9 adapter: {:#010x}", Hex(hr));
    return SUCCEEDED(hr);
}

bool Make9On12Args(UINT sdkVersion, D3D9ON12_ARGS& args)
{
    LUID luid;
    if (!DefaultAdapterLuid(sdkVersion, luid))
        return false;
    Renderer* renderer = Renderer::Create(luid, GetOptions());
    if (renderer == nullptr)
        return false;
    args = {};
    args.Enable9On12 = TRUE;
    args.pD3D12Device = renderer->Device();
    args.ppD3D12Queues[0] = renderer->Queue();
    args.NumQueues = 1;
    return true;
}

}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    if (!GetOptions().enabled) {
        auto create = SystemProc<decltype(&Direct3DCreate9)>("Direct3DCreate9");
        return create ? create(sdkVersion) : nullptr;
    }

    D3D9ON12_ARGS args;
    auto create = SystemProc<decltype(&Direct3DCreate9On12)>("Direct3DCreate9On12");
    if (create == nullptr || !Make9On12Args(sdkVersion, args)) {
        log::Info("Direct3DCreate9: D3D9On12 setup failed");
        return nullptr;
    }
    IDirect3D9* d3d = create(sdkVersion, &args, 1);
    if (d3d == nullptr) {
        log::Info("Direct3DCreate9On12 failed");
        return nullptr;
    }
    hooks::InstallInterfaceHooks(d3d);
    return d3d;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** d3d)
{
    if (!GetOptions().enabled) {
        auto create = SystemProc<decltype(&Direct3DCreate9Ex)>("Direct3DCreate9Ex");
        return create ? create(sdkVersion, d3d) : D3DERR_NOTAVAILABLE;
    }

    D3D9ON12_ARGS args;
    auto create = SystemProc<decltype(&Direct3DCreate9On12Ex)>("Direct3DCreate9On12Ex");
    if (create == nullptr || !Make9On12Args(sdkVersion, args)) {
        log::Info("Direct3DCreate9Ex: D3D9On12 setup failed");
        return D3DERR_NOTAVAILABLE;
    }
    HRESULT hr = create(sdkVersion, &args, 1, d3d);
    if (FAILED(hr)) {
        log::Info("Direct3DCreate9On12Ex failed: {:#010x}", Hex(hr));
        return hr;
    }
    hooks::InstallInterfaceHooks(*d3d);
    return hr;
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9On12(UINT sdkVersion, D3D9ON12_ARGS* overrides, UINT overrideCount)
{
    auto create = SystemProc<decltype(&Direct3DCreate9On12)>("Direct3DCreate9On12");
    return create ? create(sdkVersion, overrides, overrideCount) : nullptr;
}

extern "C" HRESULT WINAPI Direct3DCreate9On12Ex(UINT sdkVersion, D3D9ON12_ARGS* overrides, UINT overrideCount, IDirect3D9Ex** d3d)
{
    auto create = SystemProc<decltype(&Direct3DCreate9On12Ex)>("Direct3DCreate9On12Ex");
    return create ? create(sdkVersion, overrides, overrideCount, d3d) : D3DERR_NOTAVAILABLE;
}

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name)
{
    auto fn = SystemProc<decltype(&D3DPERF_BeginEvent)>("D3DPERF_BeginEvent");
    return fn ? fn(color, name) : -1;
}

extern "C" int WINAPI D3DPERF_EndEvent()
{
    auto fn = SystemProc<decltype(&D3DPERF_EndEvent)>("D3DPERF_EndEvent");
    return fn ? fn() : -1;
}

extern "C" DWORD WINAPI D3DPERF_GetStatus()
{
    auto fn = SystemProc<decltype(&D3DPERF_GetStatus)>("D3DPERF_GetStatus");
    return fn ? fn() : 0;
}

extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame()
{
    auto fn = SystemProc<decltype(&D3DPERF_QueryRepeatFrame)>("D3DPERF_QueryRepeatFrame");
    return fn ? fn() : FALSE;
}

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name)
{
    if (auto fn = SystemProc<decltype(&D3DPERF_SetMarker)>("D3DPERF_SetMarker"))
        fn(color, name);
}

extern "C" void WINAPI D3DPERF_SetOptions(DWORD options)
{
    if (auto fn = SystemProc<decltype(&D3DPERF_SetOptions)>("D3DPERF_SetOptions"))
        fn(options);
}

extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name)
{
    if (auto fn = SystemProc<decltype(&D3DPERF_SetRegion)>("D3DPERF_SetRegion"))
        fn(color, name);
}
