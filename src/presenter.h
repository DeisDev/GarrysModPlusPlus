#pragma once

#include <d3d12.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

namespace gmodpp {

class Renderer;

class Presenter {
public:
    explicit Presenter(Renderer& renderer) : m_renderer(renderer) {}

    bool Initialize(IDirect3DDevice9* device, HWND fallbackWindow, const D3DPRESENT_PARAMETERS& params);
    bool PresentFrame(IDirect3DDevice9* device);
    void BeforeReset();
    void AfterReset(const D3DPRESENT_PARAMETERS& params);
    HWND SinkWindow() const { return m_sinkWindow; }
    bool Active() const { return m_active; }

private:
    static constexpr UINT kBufferCount = 3;

    bool CreateSinkWindow();
    bool EnsureCopySource(IDirect3DDevice9* device);
    void WaitForGpu();
    void Disable(const char* reason, HRESULT hr);

    Renderer& m_renderer;
    Microsoft::WRL::ComPtr<IDirect3DDevice9On12> m_device9On12;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvIncrement = 0;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kBufferCount> m_allocators;
    std::array<uint64_t, kBufferCount> m_allocatorFenceValues = {};
    Microsoft::WRL::ComPtr<IDirect3DTexture9> m_copySource;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> m_copySourceSurface;
    Microsoft::WRL::ComPtr<IDirect3DQuery9> m_flushQuery;
    HANDLE m_fenceEvent = nullptr;
    HWND m_window = nullptr;
    HWND m_sinkWindow = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_syncInterval = 1;
    uint64_t m_fenceValue = 0;
    uint64_t m_frame = 0;
    bool m_active = false;
};

}
