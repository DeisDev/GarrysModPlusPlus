#include "descriptor_heap.h"

#include "log.h"

namespace gmodpp {

bool DescriptorHeap::Initialize(ID3D12Device* device, UINT capacity)
{
    m_device = device;
    D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, capacity, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
    if (FAILED(hr)) {
        log::Info("Descriptor heap of {} entries failed ({:#010x})", capacity, static_cast<unsigned>(hr));
        return false;
    }
    m_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_capacity = capacity;
    return true;
}

UINT DescriptorHeap::Reserve(UINT count)
{
    std::scoped_lock lock(m_mutex);
    if (m_next + count > m_capacity)
        return kInvalid;
    UINT first = m_next;
    m_next += count;
    return first;
}

UINT DescriptorHeap::Allocate()
{
    std::scoped_lock lock(m_mutex);
    if (!m_free.empty()) {
        UINT index = m_free.back();
        m_free.pop_back();
        return index;
    }
    if (m_next == m_capacity) {
        log::Info("Descriptor heap is full ({} entries)", m_capacity);
        return kInvalid;
    }
    return m_next++;
}

void DescriptorHeap::Free(UINT index)
{
    if (index == kInvalid)
        return;
    std::scoped_lock lock(m_mutex);
    m_free.push_back(index);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::Cpu(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_increment;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::Gpu(UINT index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * m_increment;
    return handle;
}

UINT DescriptorHeap::CreateRawView(ID3D12Resource* buffer)
{
    UINT index = Allocate();
    if (index == kInvalid)
        return kInvalid;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = static_cast<UINT>(buffer->GetDesc().Width / 4);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    m_device->CreateShaderResourceView(buffer, &srv, Cpu(index));
    return index;
}

}
