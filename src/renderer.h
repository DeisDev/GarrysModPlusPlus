#pragma once

#include "descriptor_heap.h"

#include <d3d12.h>
#include <d3d9.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace gmodpp {

struct Options;
class Presenter;

class Renderer {
public:
    static Renderer* Create(LUID adapterLuid, const Options& options);

    ID3D12Device5* Device() const { return m_device.Get(); }
    ID3D12CommandQueue* Queue() const { return m_queue.Get(); }
    DescriptorHeap& Descriptors() { return m_descriptors; }
    const LUID& AdapterLuid() const { return m_adapterLuid; }

    bool AttachGameDevice(IDirect3DDevice9* device, HWND focusWindow, const D3DPRESENT_PARAMETERS& params);
    Presenter* GetPresenter() const { return m_presenter.get(); }
    bool SupportsRaytracing() const { return m_raytracing; }
    void OnPresent();
    void OnReset();

private:
    void DrainDebugMessages();

    Microsoft::WRL::ComPtr<ID3D12Device5> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_infoQueue;
    DescriptorHeap m_descriptors;
    std::unique_ptr<Presenter> m_presenter;
    LUID m_adapterLuid = {};
    bool m_raytracing = false;
    uint64_t m_frame = 0;
};

Renderer* GetRenderer();

}
