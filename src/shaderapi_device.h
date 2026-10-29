#pragma once

#include "descriptor_heap.h"
#include "shaderapi_native.h"
#include "shaderapi_types.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <vector>

namespace gmodpp::native {

// A resource with the state the frame's command list leaves it in.
struct TrackedResource {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

// The Direct3D 12 device, the game window's flip-model swap chain and the frame's command list. Commands are recorded
// into one list per frame, which Present submits; the backend keeps kFrames frames in flight.
class Backend {
public:
    static constexpr UINT kBackBuffers = 3;
    static constexpr UINT kFrames = 2;

    struct Upload {
        ID3D12Resource* buffer;
        UINT64 offset;
        std::uint8_t* data;
    };

    bool SetMode(HWND window, const ShaderDeviceInfo& info);
    void Present();

    bool HasDevice() const { return m_device != nullptr; }
    ID3D12Device* Device() const { return m_device.Get(); }
    ID3D12GraphicsCommandList* Commands() const { return m_commandList.Get(); }
    DescriptorHeap& Descriptors() { return m_descriptors; }
    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }
    HWND Window() const { return m_window; }

    // CPU-written memory the frame's commands can read until the frame completes.
    Upload AllocateUpload(UINT64 size, UINT64 alignment);
    // Keeps an object alive until the GPU has finished the current frame.
    void Retire(Microsoft::WRL::ComPtr<ID3D12Resource> resource);
    // Frees a descriptor once the GPU has finished the current frame.
    void RetireDescriptor(UINT index);
    void Transition(TrackedResource& resource, D3D12_RESOURCE_STATES state);

    // materialsystem's IShaderUtil, from the factory given to IShaderDeviceMgr::Connect.
    void* shaderUtil = nullptr;

private:
    struct UploadPage {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        std::uint8_t* data = nullptr;
        UINT64 size = 0;
        UINT64 used = 0;
    };

    struct Frame {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        std::uint64_t fence = 0;
        std::vector<UploadPage> uploads;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> retired;
        std::vector<UINT> retiredDescriptors;
    };

    bool CreateDevice();
    bool CreateSwapChain();
    void BeginFrame();
    void WaitForGpu();

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    std::array<TrackedResource, kBackBuffers> m_backBuffers;
    std::array<Frame, kFrames> m_frames;
    UINT m_frame = 0;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    bool m_recording = false;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    std::uint64_t m_fenceValue = 0;
    DescriptorHeap m_descriptors;
    UINT m_rtvSize = 0;
    HWND m_window = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    bool m_vsync = true;
    bool m_tearing = false;
};

Backend& GetBackend();

}
