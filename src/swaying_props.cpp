#include "swaying_props.h"

#include "log.h"

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

// Props farther from the eye than this keep their model's still BLAS: there a sway of a unit or two is under a pixel.
constexpr float kSwayDistance = 2048.0f;
constexpr UINT64 kAlignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
constexpr UINT kThreadsPerGroup = 64;

UINT64 Align(UINT64 value) { return (value + kAlignment - 1) & ~(kAlignment - 1); }

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

struct SwayingProps::Header {
    float time;
    float previousTime;
    float wind[2];
    float previousWind[2];
    uint32_t padding[2];
};

struct SwayingProps::Job {
    float objectToWorld[3][4];
    uint32_t sourceView;
    uint32_t vertexMaterialsView;
    uint32_t vertexCount;
    uint32_t outputOffset;
};

ComPtr<ID3D12Resource> SwayingProps::CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
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
    if (FAILED(m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer))))
        return nullptr;
    return buffer;
}

void SwayingProps::Describe(const Scene& scene, const SwayingProp& prop, D3D12_GPU_VIRTUAL_ADDRESS vertices,
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, D3D12_RAYTRACING_GEOMETRY_DESC (&descs)[2],
    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles, D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage) const
{
    const Scene::ModelBuffers buffers = *scene.Buffers(prop.model->sceneModel);
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = Scene::GeometryDescs(vertices, buffers.vertexCount, buffers.indices, buffers.triangleCount, buffers.opaqueTriangles, descs);
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = descs;
    if (buffers.opaqueTriangles < buffers.triangleCount && scene.LinkMicromaps(prop.model->sceneModel, descs[inputs.NumDescs - 1], triangles, linkage))
        inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS;
}

bool SwayingProps::SetProps(const Scene& scene, std::span<const SwayMaterial> materials, std::vector<SwayingProp> props)
{
    static_assert(sizeof(Header) == 32 && sizeof(Job) == 64 && sizeof(SwayMaterial) == 64);
    Clear();
    if (props.empty())
        return true;
    std::erase_if(props, [&](const SwayingProp& prop) {
        const std::optional<Scene::ModelBuffers> buffers = scene.Buffers(prop.model->sceneModel);
        return !buffers || buffers->vertexCount != prop.model->vertexMaterials.size();
    });
    auto fail = [&](const char* what) {
        log::Info("Scene: swaying props keep still shadows ({})", what);
        Clear();
        return false;
    };
    m_materials = CreateBuffer(materials.size_bytes(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    if (!m_materials || FAILED(m_materials->Map(0, nullptr, &mapped)))
        return fail("creating the material table failed");
    std::memcpy(mapped, materials.data(), materials.size_bytes());
    m_materials->Unmap(0, nullptr);
    m_materialsView = m_descriptors->CreateRawView(m_materials.Get());
    m_views.push_back(m_materialsView);

    std::vector<PropState> states(props.size());
    UINT64 vertexBytes = 0;
    UINT64 blasBytes = 0;
    UINT64 scratchBytes = 0;
    UINT64 vertices = 0;
    for (size_t i = 0; i < props.size(); ++i) {
        const SwayingProp& prop = props[i];
        PropState& state = states[i];
        auto shared = std::find_if(m_vertexMaterials.begin(), m_vertexMaterials.end(), [&](const VertexMaterials& entry) { return entry.model == prop.model; });
        if (shared == m_vertexMaterials.end()) {
            const std::vector<uint32_t>& vertexMaterials = prop.model->vertexMaterials;
            ComPtr<ID3D12Resource> buffer = CreateBuffer(vertexMaterials.size() * sizeof(uint32_t), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!buffer || FAILED(buffer->Map(0, nullptr, &mapped)))
                return fail("creating vertex materials failed");
            std::memcpy(mapped, vertexMaterials.data(), vertexMaterials.size() * sizeof(uint32_t));
            buffer->Unmap(0, nullptr);
            const UINT view = m_descriptors->CreateRawView(buffer.Get());
            m_views.push_back(view);
            m_vertexMaterials.push_back({prop.model, std::move(buffer), view});
            shared = m_vertexMaterials.end() - 1;
        }
        state.vertexMaterialsView = shared->view;
        const UINT vertexCount = static_cast<UINT>(prop.model->vertexMaterials.size());
        state.vertexOffset = vertexBytes;
        vertexBytes += Align(vertexCount * sizeof(float) * 3);
        vertices += vertexCount;

        D3D12_RAYTRACING_GEOMETRY_DESC descs[2];
        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC triangles;
        D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC linkage;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        Describe(scene, prop, 0, inputs, descs, triangles, linkage);
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
        m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        state.blasOffset = blasBytes;
        blasBytes += Align(info.ResultDataMaxSizeInBytes);
        state.scratchOffset = scratchBytes;
        scratchBytes += Align(std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes));
    }
    if (std::find(m_views.begin(), m_views.end(), DescriptorHeap::kInvalid) != m_views.end())
        return fail("the descriptor heap is full");

    constexpr D3D12_RESOURCE_FLAGS kUav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    m_current = CreateBuffer(vertexBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_previous = CreateBuffer(vertexBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_blas = CreateBuffer(blasBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    m_scratch = CreateBuffer(scratchBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_jobsSlotSize = Align(sizeof(Header) + props.size() * sizeof(Job));
    m_jobs = CreateBuffer(m_jobsSlotSize * Scene::kFrameSlots, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_current || !m_previous || !m_blas || !m_scratch || !m_jobs || FAILED(m_jobs->Map(0, nullptr, reinterpret_cast<void**>(&m_jobsMapped))))
        return fail("creating buffers failed");
    m_verticesWritable = true;
    m_currentUav = m_descriptors->CreateRawUav(m_current.Get());
    m_previousUav = m_descriptors->CreateRawUav(m_previous.Get());
    m_views.push_back(m_currentUav);
    m_views.push_back(m_previousUav);
    for (UINT slot = 0; slot < Scene::kFrameSlots; ++slot) {
        m_jobsViews[slot] = m_descriptors->CreateRawView(m_jobs.Get(), slot * m_jobsSlotSize, m_jobsSlotSize);
        m_views.push_back(m_jobsViews[slot]);
    }
    for (size_t i = 0; i < props.size(); ++i) {
        PropState& state = states[i];
        const UINT64 bytes = Align(props[i].model->vertexMaterials.size() * sizeof(float) * 3);
        state.currentView = m_descriptors->CreateRawView(m_current.Get(), state.vertexOffset, bytes);
        state.previousView = m_descriptors->CreateRawView(m_previous.Get(), state.vertexOffset, bytes);
        m_views.push_back(state.currentView);
        m_views.push_back(state.previousView);
    }
    if (std::find(m_views.begin(), m_views.end(), DescriptorHeap::kInvalid) != m_views.end())
        return fail("the descriptor heap is full");
    log::Info("Scene: {} swaying static props ({} vertices, {:.1f} MB of acceleration structures)", props.size(), vertices,
        static_cast<double>(blasBytes + scratchBytes) / (1024.0 * 1024.0));
    m_props = std::move(props);
    m_states = std::move(states);
    return true;
}

void SwayingProps::Clear()
{
    for (UINT view : m_views)
        m_descriptors->Free(view);
    m_views.clear();
    m_props.clear();
    m_states.clear();
    m_vertexMaterials.clear();
    m_materials.Reset();
    m_current.Reset();
    m_previous.Reset();
    m_blas.Reset();
    m_scratch.Reset();
    m_jobs.Reset();
    m_jobsMapped = nullptr;
    m_materialsView = m_currentUav = m_previousUav = DescriptorHeap::kInvalid;
}

std::optional<SwayingProps::Jobs> SwayingProps::BeginUpdate(ID3D12GraphicsCommandList4* list, const Scene& scene, UINT slot, const float (&eye)[3],
    float time, float previousTime, const float (&wind)[2], const float (&previousWind)[2])
{
    if (m_props.empty())
        return std::nullopt;
    std::byte* base = m_jobsMapped + slot * m_jobsSlotSize;
    auto* jobs = reinterpret_cast<Job*>(base + sizeof(Header));
    UINT count = 0;
    UINT maxVertices = 0;
    for (size_t i = 0; i < m_props.size(); ++i) {
        const SwayingProp& prop = m_props[i];
        PropState& state = m_states[i];
        const auto& m = prop.instance.transform;
        const float dx = m[0][3] - eye[0];
        const float dy = m[1][3] - eye[1];
        const float dz = m[2][3] - eye[2];
        state.moved = dx * dx + dy * dy + dz * dz < kSwayDistance * kSwayDistance;
        if (!state.moved)
            continue;
        const Scene::ModelBuffers buffers = *scene.Buffers(prop.model->sceneModel);
        Job& job = jobs[count++];
        std::memcpy(job.objectToWorld, m, sizeof(job.objectToWorld));
        job.sourceView = buffers.positionsView;
        job.vertexMaterialsView = state.vertexMaterialsView;
        job.vertexCount = buffers.vertexCount;
        job.outputOffset = static_cast<uint32_t>(state.vertexOffset);
        maxVertices = std::max(maxVertices, buffers.vertexCount);
    }
    if (count == 0)
        return std::nullopt;
    Header header = {time, previousTime, {wind[0], wind[1]}, {previousWind[0], previousWind[1]}, {}};
    std::memcpy(base, &header, sizeof(header));
    if (!m_verticesWritable) {
        D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(m_current.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(m_previous.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        list->ResourceBarrier(2, barriers);
        m_verticesWritable = true;
    }
    return Jobs{m_jobsViews[slot], m_materialsView, m_currentUav, m_previousUav, count, (maxVertices + kThreadsPerGroup - 1) / kThreadsPerGroup};
}

void SwayingProps::FinishUpdate(ID3D12GraphicsCommandList4* list, const Scene& scene)
{
    D3D12_RESOURCE_BARRIER barriers[] = {
        Transition(m_current.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
        Transition(m_previous.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(2, barriers);
    m_verticesWritable = false;
    for (size_t i = 0; i < m_props.size(); ++i) {
        PropState& state = m_states[i];
        if (!state.moved)
            continue;
        D3D12_RAYTRACING_GEOMETRY_DESC descs[2];
        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC triangles;
        D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC linkage;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
        Describe(scene, m_props[i], m_current->GetGPUVirtualAddress() + state.vertexOffset, desc.Inputs, descs, triangles, linkage);
        desc.DestAccelerationStructureData = m_blas->GetGPUVirtualAddress() + state.blasOffset;
        if (state.built) {
            desc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            desc.SourceAccelerationStructureData = desc.DestAccelerationStructureData;
        }
        desc.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress() + state.scratchOffset;
        list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        state.built = true;
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &barrier);
}

void SwayingProps::Append(std::vector<SceneInstance>& instances) const
{
    for (size_t i = 0; i < m_props.size(); ++i) {
        SceneInstance& instance = instances.emplace_back(m_props[i].instance);
        if (instance.mask == 0)
            instance.mask = kStaticPropMask;
        const PropState& state = m_states[i];
        if (!state.moved || !state.built)
            continue;
        instance.blas = m_blas->GetGPUVirtualAddress() + state.blasOffset;
        instance.positionsView = state.currentView;
        instance.previousPositionsView = state.previousView;
    }
}

}
