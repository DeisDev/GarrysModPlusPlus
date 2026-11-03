#pragma once

#include "descriptor_heap.h"
#include "fsr.h"
#include "settings.h"
#include "shaderapi_native.h"
#include "shaderapi_types.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gmodpp::native {

// A resource with the state the frame's command list leaves it in.
struct TrackedResource {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

// Non-shader-visible descriptors (render target and depth-stencil views). The command list copies them when it
// records OMSetRenderTargets, so they can be freed at once.
class CpuDescriptorPool {
public:
    bool Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity);
    // A null handle (ptr 0) when the pool is full.
    D3D12_CPU_DESCRIPTOR_HANDLE Allocate();
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE handle);

private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT m_increment = 0;
    UINT m_capacity = 0;
    UINT m_next = 0;
    std::vector<SIZE_T> m_free;
};

// A monitor, in the order Displays() lists them.
struct DisplayInfo {
    RECT area;
    bool primary;
};
std::vector<DisplayInfo> Displays();
// The DXGI adapter of the engine's display adapter `index` (Direct3D 9 numbers one per display output), or null.
Microsoft::WRL::ComPtr<IDXGIAdapter1> AdapterOfDisplay(int index);

// The Direct3D 12 device, the game window's flip-model swap chain and the frame's command list. Commands are recorded
// into one list per frame, which Present submits; the backend keeps kFrames frames in flight.
class Backend {
public:
    static constexpr UINT kBackBuffers = 3;
    static constexpr UINT kFrames = 2;
    static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    // The depth buffer's resource format, which shader resource views can read.
    static constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R24G8_TYPELESS;

    struct Upload {
        ID3D12Resource* buffer;
        UINT64 offset;
        std::uint8_t* data;
    };

    bool SetMode(HWND window, const ShaderDeviceInfo& info);
    void Present();
    // Moves the game window onto the display Settings::display names, when that changed since the last call.
    void PlaceWindow();
    // Submits the commands recorded so far and waits for the GPU, then continues recording. For readbacks, and to
    // reclaim staging memory while the game loads many resources between two presents.
    void Flush();
    // Submits the commands recorded so far without waiting and continues recording, so work submitted to the queue
    // next runs after them.
    void Submit();

    bool HasDevice() const { return m_device != nullptr; }
    ID3D12Device* Device() const { return m_device.Get(); }
    ID3D12CommandQueue* Queue() const { return m_queue.Get(); }
    ID3D12GraphicsCommandList* Commands() const { return m_commandList.Get(); }
    DescriptorHeap& Descriptors() { return m_descriptors; }
    CpuDescriptorPool& RenderTargetViews() { return m_rtvs; }
    CpuDescriptorPool& DepthStencilViews() { return m_dsvs; }
    ID3D12DescriptorHeap* SamplerHeap() const { return m_samplerHeap.Get(); }
    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }
    HWND Window() const { return m_window; }
    // Changes whenever the command list is reset, which loses its bound state.
    std::uint64_t ListGeneration() const { return m_listGeneration; }
    // The fence value the commands recorded so far signal once they complete, and the last one completed.
    std::uint64_t PendingFence() const { return m_fenceValue + 1; }
    std::uint64_t CompletedFence() const { return m_fence->GetCompletedValue(); }

    // What draws to the default target render into: the swap chain's current buffer.
    TrackedResource& BackBuffer() { return SwapChainBuffer(); }
    D3D12_CPU_DESCRIPTOR_HANDLE BackBufferView(bool srgb) const;
    // Its shader resource view.
    UINT BackBufferTexture() const { return m_backBufferTextures[m_swapChain->GetCurrentBackBufferIndex()]; }
    TrackedResource& DepthBuffer() { return m_depthBuffer; }
    D3D12_CPU_DESCRIPTOR_HANDLE DepthBufferView() const { return m_depthView; }
    // FSR's frame generation while its swap chain presents and the menu wants it, else null.
    FsrFrameGeneration* FsrFrameGenerator()
    {
        return m_fsrFrameGeneration.Active() && GetSettings().frameGeneration == FrameGeneration::Fsr ? &m_fsrFrameGeneration : nullptr;
    }

    // CPU-written memory the frame's commands can read until the frame completes.
    Upload AllocateUpload(UINT64 size, UINT64 alignment);
    // CPU-written memory for a copy recorded at once (texture and static buffer uploads), reclaimed by Flush.
    Upload AllocateStaging(UINT64 size, UINT64 alignment);
    // The index of a sampler with these settings in the shader-visible sampler heap, created on first use.
    UINT SamplerIndex(const D3D12_SAMPLER_DESC& desc);
    // Keeps an object alive until the GPU has finished the current frame.
    void Retire(Microsoft::WRL::ComPtr<ID3D12Resource> resource);
    // Frees a descriptor once the GPU has finished the current frame.
    void RetireDescriptor(UINT index);
    void Transition(TrackedResource& resource, D3D12_RESOURCE_STATES state);

    // The command list is recorded only on the thread that created the device. Work from other threads (the engine
    // fills static prop colour meshes on a loader thread) runs there before the next draw, flush or present.
    bool OnRenderThread() const { return GetCurrentThreadId() == m_renderThread; }
    void RunOnRenderThread(std::function<void()> work);
    void RunPending();

    // materialsystem's IShaderUtil, from the factory given to IShaderDeviceMgr::Connect.
    void* shaderUtil = nullptr;
    // filesystem_stdio's IBaseFileSystem, from the same factory.
    void* fileSystem = nullptr;

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
        std::vector<UploadPage> staging;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> retired;
        std::vector<UINT> retiredDescriptors;
    };

    bool CreateDevice();
    bool CreateSwapChain();
    int m_placedDisplay = -1;
    bool CreateDepthBuffer();
    TrackedResource& SwapChainBuffer() { return m_backBuffers[m_swapChain->GetCurrentBackBufferIndex()]; }
    void DrainDebugMessages();
    void BeginFrame();
    void WaitForGpu();
    Upload Allocate(std::vector<UploadPage>& pages, UINT64 size, UINT64 alignment);

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    // The frame generator the swap chain is made for (Settings::frameGeneration when the device was created).
    FrameGeneration m_frameGeneration = FrameGeneration::Off;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_infoQueue;
    UINT64 m_debugMessages = 0;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    FsrFrameGeneration m_fsrFrameGeneration;
    std::array<TrackedResource, kBackBuffers> m_backBuffers;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBackBuffers * 2> m_backBufferViews = {};
    std::array<UINT, kBackBuffers> m_backBufferTextures = { DescriptorHeap::kInvalid, DescriptorHeap::kInvalid, DescriptorHeap::kInvalid };
    TrackedResource m_depthBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE m_depthView = {};
    std::array<Frame, kFrames> m_frames;
    UINT m_frame = 0;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    bool m_recording = false;
    std::uint64_t m_listGeneration = 0;
    UINT64 m_stagingUsed = 0;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    std::uint64_t m_fenceValue = 0;
    DescriptorHeap m_descriptors;
    CpuDescriptorPool m_rtvs;
    CpuDescriptorPool m_dsvs;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_samplerHeap;
    UINT m_samplerIncrement = 0;
    // D3D12_SAMPLER_DESC's 13 four-byte members.
    using SamplerKey = std::array<std::uint32_t, sizeof(D3D12_SAMPLER_DESC) / 4>;
    struct SamplerKeyHash {
        size_t operator()(const SamplerKey& key) const
        {
            std::uint64_t hash = 14695981039346656037ull;
            for (std::uint32_t word : key)
                hash = (hash ^ word) * 1099511628211ull;
            return static_cast<size_t>(hash);
        }
    };
    std::unordered_map<SamplerKey, UINT, SamplerKeyHash> m_samplers;
    DWORD m_renderThread = 0;
    std::mutex m_pendingMutex;
    std::vector<std::function<void()>> m_pending;
    std::atomic<bool> m_hasPending = false;
    bool m_runningPending = false;
    HWND m_window = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    bool m_vsync = true;
    bool m_tearing = false;
    bool m_deviceLost = false;
};

Backend& GetBackend();
// IShaderAPI::ChangeVideoMode: the mode the next Present switches to.
void RequestVideoMode(const ShaderDeviceInfo& info);

}
