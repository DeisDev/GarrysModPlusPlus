#include "presenter.h"

#include "log.h"
#include "renderer.h"

#include <future>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

UINT SyncIntervalFor(UINT presentationInterval)
{
    switch (presentationInterval) {
    case D3DPRESENT_INTERVAL_IMMEDIATE: return 0;
    case D3DPRESENT_INTERVAL_TWO: return 2;
    case D3DPRESENT_INTERVAL_THREE: return 3;
    case D3DPRESENT_INTERVAL_FOUR: return 4;
    default: return 1;
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

}

bool Presenter::Initialize(IDirect3DDevice9* device, HWND fallbackWindow, const D3DPRESENT_PARAMETERS& params)
{
    if (!params.Windowed) {
        log::Info("Presenter: exclusive fullscreen is not supported yet; launch with -windowed (add -noborder for borderless)");
        return false;
    }

    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&m_device9On12));
    if (FAILED(hr)) {
        Disable("device is not D3D9On12", hr);
        return false;
    }

    m_window = params.hDeviceWindow ? params.hDeviceWindow : fallbackWindow;
    m_width = params.BackBufferWidth;
    m_height = params.BackBufferHeight;
    m_syncInterval = SyncIntervalFor(params.PresentationInterval);

    ID3D12Device* d3d12 = m_renderer.Device();
    ComPtr<IDXGIFactory2> factory;
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Disable("CreateDXGIFactory2 failed", hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapChain;
    hr = factory->CreateSwapChainForHwnd(m_renderer.Queue(), m_window, &desc, nullptr, nullptr, &swapChain);
    if (SUCCEEDED(hr))
        hr = swapChain.As(&m_swapChain);
    if (FAILED(hr)) {
        Disable("CreateSwapChainForHwnd failed", hr);
        return false;
    }
    factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    for (auto& allocator : m_allocators) {
        hr = d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (FAILED(hr)) {
            Disable("CreateCommandAllocator failed", hr);
            return false;
        }
    }
    hr = d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].Get(), nullptr, IID_PPV_ARGS(&m_commandList));
    if (SUCCEEDED(hr))
        hr = m_commandList->Close();
    if (SUCCEEDED(hr))
        hr = d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        Disable("command list or fence creation failed", hr);
        return false;
    }
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &m_flushQuery);
    if (FAILED(hr)) {
        Disable("CreateQuery(EVENT) failed", hr);
        return false;
    }
    if (!CreateSinkWindow())
        return false;

    m_active = true;
    log::Info("Presenter: flip-model swap chain {}x{} on window {}, sync interval {}", m_width, m_height,
        static_cast<void*>(m_window), m_syncInterval);
    return true;
}

bool Presenter::CreateSinkWindow()
{
    std::promise<HWND> created;
    std::future<HWND> result = created.get_future();
    std::thread([&created] {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GarrysModPPPresentSink";
        RegisterClassW(&wc);
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        created.set_value(window);
        if (window == nullptr)
            return;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            DispatchMessageW(&msg);
    }).detach();
    m_sinkWindow = result.get();
    if (m_sinkWindow == nullptr) {
        Disable("could not create the present sink window", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    return true;
}

bool Presenter::EnsureCopySource(IDirect3DDevice9* device)
{
    if (m_copySource)
        return true;
    HRESULT hr = device->CreateTexture(m_width, m_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_copySource, nullptr);
    if (SUCCEEDED(hr))
        hr = m_copySource->GetSurfaceLevel(0, &m_copySourceSurface);
    if (FAILED(hr)) {
        Disable("could not create the copy source texture", hr);
        return false;
    }
    return true;
}

bool Presenter::PresentFrame(IDirect3DDevice9* device)
{
    if (!m_active || !EnsureCopySource(device))
        return false;

    ComPtr<IDirect3DSurface9> backBuffer;
    HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (SUCCEEDED(hr))
        hr = device->StretchRect(backBuffer.Get(), nullptr, m_copySourceSurface.Get(), nullptr, D3DTEXF_NONE);
    if (FAILED(hr)) {
        Disable("could not copy the back buffer", hr);
        return false;
    }

    m_flushQuery->Issue(D3DISSUE_END);
    m_flushQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);

    ID3D12CommandQueue* queue = m_renderer.Queue();
    ComPtr<ID3D12Resource> source;
    hr = m_device9On12->UnwrapUnderlyingResource(m_copySource.Get(), queue, IID_PPV_ARGS(&source));
    if (FAILED(hr)) {
        Disable("UnwrapUnderlyingResource failed", hr);
        return false;
    }

    const UINT slot = static_cast<UINT>(m_frame % kBufferCount);
    if (m_fence->GetCompletedValue() < m_allocatorFenceValues[slot]) {
        m_fence->SetEventOnCompletion(m_allocatorFenceValues[slot], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    ComPtr<ID3D12Resource> target;
    hr = m_swapChain->GetBuffer(m_swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target));
    if (SUCCEEDED(hr))
        hr = m_allocators[slot]->Reset();
    if (SUCCEEDED(hr))
        hr = m_commandList->Reset(m_allocators[slot].Get(), nullptr);
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER before[] = {
            Transition(source.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        m_commandList->ResourceBarrier(2, before);
        m_commandList->CopyResource(target.Get(), source.Get());
        D3D12_RESOURCE_BARRIER after[] = {
            Transition(source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            Transition(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
        };
        m_commandList->ResourceBarrier(2, after);
        hr = m_commandList->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {m_commandList.Get()};
        queue->ExecuteCommandLists(1, lists);
        hr = queue->Signal(m_fence.Get(), ++m_fenceValue);
    }

    UINT64 signalValue = m_fenceValue;
    ID3D12Fence* fence = m_fence.Get();
    HRESULT returned = m_device9On12->ReturnUnderlyingResource(m_copySource.Get(), 1, &signalValue, &fence);
    if (FAILED(hr) || FAILED(returned)) {
        Disable("copy to the swap chain failed", FAILED(hr) ? hr : returned);
        return false;
    }
    m_allocatorFenceValues[slot] = m_fenceValue;

    hr = m_swapChain->Present(m_syncInterval, 0);
    if (FAILED(hr)) {
        Disable("IDXGISwapChain::Present failed", hr);
        return false;
    }
    ++m_frame;
    return true;
}

void Presenter::BeforeReset()
{
    if (m_fence)
        WaitForGpu();
    m_copySourceSurface.Reset();
    m_copySource.Reset();
}

void Presenter::AfterReset(const D3DPRESENT_PARAMETERS& params)
{
    if (!m_active)
        return;
    m_syncInterval = SyncIntervalFor(params.PresentationInterval);
    if (params.BackBufferWidth == m_width && params.BackBufferHeight == m_height)
        return;
    m_width = params.BackBufferWidth;
    m_height = params.BackBufferHeight;
    HRESULT hr = m_swapChain->ResizeBuffers(kBufferCount, m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr))
        Disable("ResizeBuffers failed", hr);
    else
        log::Info("Presenter: resized to {}x{}", m_width, m_height);
}

void Presenter::WaitForGpu()
{
    if (FAILED(m_renderer.Queue()->Signal(m_fence.Get(), ++m_fenceValue)))
        return;
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void Presenter::Disable(const char* reason, HRESULT hr)
{
    log::Info("Presenter disabled: {} ({:#010x})", reason, Hex(hr));
    m_active = false;
}

}
