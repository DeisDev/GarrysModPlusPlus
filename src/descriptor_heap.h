#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <mutex>
#include <vector>

namespace gmodpp {

// The single shader-visible CBV/SRV/UAV heap. Shaders index it directly (SM 6.6), so geometry and per-frame
// views share one heap: a fixed block reserved at startup plus individually allocated persistent views.
class DescriptorHeap {
public:
    static constexpr UINT kInvalid = ~0u;

    bool Initialize(ID3D12Device* device, UINT capacity);
    // Reserves `count` contiguous descriptors for the lifetime of the heap.
    UINT Reserve(UINT count);
    UINT Allocate();
    void Free(UINT index);

    ID3D12DescriptorHeap* Heap() const { return m_heap.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index) const;

    // Creates a raw (ByteAddressBuffer) view of a whole buffer and returns its index, or kInvalid.
    UINT CreateRawView(ID3D12Resource* buffer);

private:
    ID3D12Device* m_device = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT m_increment = 0;
    UINT m_capacity = 0;
    UINT m_next = 0;
    std::vector<UINT> m_free;
    std::mutex m_mutex;
};

}
