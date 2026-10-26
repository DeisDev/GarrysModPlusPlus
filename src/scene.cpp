#include "scene.h"

#include "log.h"

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

constexpr UINT kWorldMask = 0x01;
constexpr UINT kStaticPropMask = 0x02;
constexpr UINT kDynamicMask = 0x04;

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TlasInputs(UINT count, D3D12_GPU_VIRTUAL_ADDRESS instances)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.NumDescs = count;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.InstanceDescs = instances;
    return inputs;
}

}

bool Scene::Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue)
{
    m_device = device;
    m_queue = queue;
    HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocator.Get(), nullptr, IID_PPV_ARGS(&m_list));
    if (SUCCEEDED(hr))
        hr = m_list->Close();
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        log::Info("Scene: initialization failed ({:#010x})", Hex(hr));
        return false;
    }
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

ComPtr<ID3D12Resource> Scene::CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heapProperties = {heap};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<UINT64>(size, 4);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> buffer;
    HRESULT hr = m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    if (FAILED(hr))
        log::Info("Scene: buffer of {} bytes failed ({:#010x})", size, Hex(hr));
    return buffer;
}

ComPtr<ID3D12Resource> Scene::Upload(const void* data, UINT64 size)
{
    ComPtr<ID3D12Resource> buffer = CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    if (buffer && SUCCEEDED(buffer->Map(0, nullptr, &mapped))) {
        std::memcpy(mapped, data, size);
        buffer->Unmap(0, nullptr);
        return buffer;
    }
    return nullptr;
}

int Scene::BuildBlas(const std::vector<bsp::Float3>& positions, const std::vector<uint32_t>& indices)
{
    if (indices.empty())
        return -1;
    ComPtr<ID3D12Resource> vertexBuffer = Upload(positions.data(), positions.size() * sizeof(bsp::Float3));
    ComPtr<ID3D12Resource> indexBuffer = Upload(indices.data(), indices.size() * sizeof(uint32_t));
    if (!vertexBuffer || !indexBuffer)
        return -1;

    D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.VertexBuffer.StartAddress = vertexBuffer->GetGPUVirtualAddress();
    geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(bsp::Float3);
    geometry.Triangles.VertexCount = static_cast<UINT>(positions.size());
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.IndexBuffer = indexBuffer->GetGPUVirtualAddress();
    geometry.Triangles.IndexCount = static_cast<UINT>(indices.size());
    geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = 1;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    ComPtr<ID3D12Resource> scratch = CreateBuffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> result = CreateBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    if (!scratch || !result)
        return -1;

    m_allocator->Reset();
    m_list->Reset(m_allocator.Get(), nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
    desc.Inputs = inputs;
    desc.DestAccelerationStructureData = result->GetGPUVirtualAddress();
    desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    m_list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    HRESULT hr = m_list->Close();
    if (FAILED(hr)) {
        log::Info("Scene: closing the BLAS build failed ({:#010x})", Hex(hr));
        return -1;
    }
    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    m_queue->Signal(m_fence.Get(), ++m_fenceValue);
    m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);

    m_models.push_back(result);
    return static_cast<int>(m_models.size() - 1);
}

int Scene::AddModel(const studio::Geometry& geometry)
{
    return BuildBlas(geometry.positions, geometry.indices);
}

void Scene::Clear()
{
    m_models.clear();
    m_staticInstances.clear();
    m_worldModel = -1;
    m_name.clear();
    m_sun = {};
}

bool Scene::LoadMap(const std::string& name, const bsp::WorldGeometry& world)
{
    Clear();
    m_worldModel = BuildBlas(world.positions, world.indices);
    if (m_worldModel < 0)
        return false;
    m_name = name;
    m_sun = world.sun;
    return true;
}

bool Scene::EnsureFrameCapacity(FrameTlas& frame, UINT count)
{
    if (frame.tlas && frame.capacity >= count)
        return true;
    const UINT capacity = std::max<UINT>(count * 2, 256);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = TlasInputs(capacity, 0);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    frame = {};
    frame.instances = CreateBuffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * capacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    frame.scratch = CreateBuffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    frame.tlas = CreateBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    if (!frame.instances || !frame.scratch || !frame.tlas ||
        FAILED(frame.instances->Map(0, nullptr, reinterpret_cast<void**>(&frame.mapped)))) {
        frame = {};
        return false;
    }
    frame.capacity = capacity;
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::RecordFrameTlas(ID3D12GraphicsCommandList4* list, UINT slot, std::span<const SceneInstance> dynamic)
{
    FrameTlas& frame = m_frames[slot % kFrameSlots];
    const UINT count = static_cast<UINT>(1 + m_staticInstances.size() + dynamic.size());
    if (!EnsureFrameCapacity(frame, count))
        return 0;

    UINT written = 0;
    auto write = [&](int model, D3D12_GPU_VIRTUAL_ADDRESS blas, const float (*transform)[4], UINT mask) {
        if (blas == 0 && model >= 0 && static_cast<size_t>(model) < m_models.size())
            blas = m_models[model]->GetGPUVirtualAddress();
        if (blas == 0)
            return;
        D3D12_RAYTRACING_INSTANCE_DESC& instance = frame.mapped[written];
        instance = {};
        if (transform != nullptr)
            std::memcpy(instance.Transform, transform, sizeof(instance.Transform));
        else
            instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
        instance.InstanceID = written;
        instance.InstanceMask = mask;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instance.AccelerationStructure = blas;
        ++written;
    };
    write(m_worldModel, 0, nullptr, kWorldMask);
    for (const SceneInstance& instance : m_staticInstances)
        write(instance.model, instance.blas, instance.transform, kStaticPropMask);
    for (const SceneInstance& instance : dynamic)
        write(instance.model, instance.blas, instance.transform, instance.mask != 0 ? instance.mask : kDynamicMask);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
    desc.Inputs = TlasInputs(written, frame.instances->GetGPUVirtualAddress());
    desc.DestAccelerationStructureData = frame.tlas->GetGPUVirtualAddress();
    desc.ScratchAccelerationStructureData = frame.scratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier = UavBarrier(frame.tlas.Get());
    list->ResourceBarrier(1, &barrier);
    return frame.tlas->GetGPUVirtualAddress();
}

}
