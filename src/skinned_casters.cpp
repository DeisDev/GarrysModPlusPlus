#include "skinned_casters.h"

#include "log.h"

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

constexpr uint64_t kForgetAfterFrames = 120;
constexpr UINT kThreadsPerGroup = 64;
// Studio models weigh a vertex by up to three bones.
constexpr int kMaxVertexBones = 3;

// Mirrors SkinJob in shaders/skin.hlsl.
struct Job {
    uint32_t sourceView;
    uint32_t vertexCount;
    uint32_t outputUav;
    uint32_t bonesOffset;
};
static_assert(sizeof(Job) == 16);

// Mirrors SkinVertex in shaders/skin.hlsl.
struct SourceVertex {
    bsp::Float3 position;
    uint32_t bones;
    float weights[kMaxVertexBones];
    uint32_t padding;
};
static_assert(sizeof(SourceVertex) == 32);
static_assert(sizeof(studio::Matrix3x4) == 48);

UINT64 Align16(UINT64 value) { return (value + 15) & ~UINT64{15}; }

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

}

ComPtr<ID3D12Resource> SkinnedCasters::CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
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

bool SkinnedCasters::PrepareModel(SkinnedModel& model)
{
    const UINT64 bytes = model.geometry.indices.size() * sizeof(uint32_t);
    model.indexBuffer = CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    if (!model.indexBuffer || FAILED(model.indexBuffer->Map(0, nullptr, &mapped)))
        return false;
    std::memcpy(mapped, model.geometry.indices.data(), bytes);
    model.indexBuffer->Unmap(0, nullptr);
    return true;
}

void SkinnedCasters::Describe(const SkinnedModel& model, const Scene& scene, int sceneModel,
    D3D12_GPU_VIRTUAL_ADDRESS vertices, D3D12_GPU_VIRTUAL_ADDRESS indices, D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
    D3D12_RAYTRACING_GEOMETRY_DESC (&descs)[2], D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles, D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage)
{
    const UINT triangleCount = static_cast<UINT>(model.geometry.indices.size() / 3);
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.NumDescs = Scene::GeometryDescs(vertices, static_cast<UINT>(model.geometry.positions.size()), indices, triangleCount, model.opaqueTriangles, descs);
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = descs;
    if (model.opaqueTriangles < triangleCount && scene.LinkMicromaps(sceneModel, descs[inputs.NumDescs - 1], triangles, linkage))
        inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS;
}

bool SkinnedCasters::PrepareSlot(SlotState& slot, const SkinnedModel& model, const Scene& scene, int sceneModel)
{
    if (slot.blas)
        return slot.verticesView != DescriptorHeap::kInvalid && slot.verticesUav != DescriptorHeap::kInvalid;
    const UINT vertexCount = static_cast<UINT>(model.geometry.positions.size());
    slot.vertices = CreateBuffer(Align16(sizeof(bsp::Float3) * vertexCount), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COMMON);
    if (!slot.vertices)
        return false;

    D3D12_RAYTRACING_GEOMETRY_DESC descs[2];
    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC triangles;
    D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC linkage;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    Describe(model, scene, sceneModel, 0, 0, inputs, descs, triangles, linkage);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    slot.scratch = CreateBuffer(std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes), D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slot.blas = CreateBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    slot.built = false;
    if (!slot.scratch || !slot.blas)
        return false;
    slot.verticesView = m_descriptors->CreateRawView(slot.vertices.Get());
    slot.verticesUav = m_descriptors->CreateRawUav(slot.vertices.Get());
    return slot.verticesView != DescriptorHeap::kInvalid && slot.verticesUav != DescriptorHeap::kInvalid;
}

const SkinnedCasters::ModelSource* SkinnedCasters::Source(ID3D12GraphicsCommandList4* list, const std::shared_ptr<SkinnedModel>& model,
    std::vector<D3D12_RESOURCE_BARRIER>& barriers)
{
    if (const auto found = m_sources.find(model.get()); found != m_sources.end())
        return found->second.view != DescriptorHeap::kInvalid ? &found->second : nullptr;
    ModelSource& source = m_sources[model.get()];
    source.model = model;
    const studio::Geometry& geometry = model->geometry;
    const UINT64 bytes = sizeof(SourceVertex) * geometry.positions.size();
    source.upload = CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    source.buffer = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    SourceVertex* mapped = nullptr;
    if (!source.upload || !source.buffer || FAILED(source.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return nullptr;
    // As the CPU skinned them before: at least one bone, a vertex without weights wholly on its first, and bones the
    // model's pose lacks replaced by bone 0.
    const size_t boneCount = model->poseToBone.size();
    for (size_t v = 0; v < geometry.positions.size(); ++v) {
        const studio::VertexWeights weights = v < geometry.weights.size() ? geometry.weights[v] : studio::VertexWeights{};
        SourceVertex vertex = {geometry.positions[v], 0, {}, 0};
        const int count = std::clamp<int>(weights.count, 1, kMaxVertexBones);
        for (int i = 0; i < count; ++i) {
            const uint32_t bone = weights.bone[i] < boneCount ? weights.bone[i] : 0u;
            vertex.bones |= bone << (8 * i);
            vertex.weights[i] = weights.count == 0 ? 1.0f : weights.weight[i];
        }
        vertex.bones |= static_cast<uint32_t>(count) << 24;
        mapped[v] = vertex;
    }
    source.upload->Unmap(0, nullptr);
    list->CopyBufferRegion(source.buffer.Get(), 0, source.upload.Get(), 0, bytes);
    barriers.push_back(Transition(source.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    source.view = m_descriptors->CreateRawView(source.buffer.Get());
    return source.view != DescriptorHeap::kInvalid ? &source : nullptr;
}

bool SkinnedCasters::EnsureJobs(JobBuffer& jobs, UINT64 size)
{
    if (jobs.buffer && jobs.size >= size)
        return true;
    // The slot's last jobs were read a frame slot's cycle ago, which the caller waited for (BeginUpdate).
    m_descriptors->Free(jobs.view);
    jobs = {};
    jobs.size = std::max<UINT64>(Align16(size), UINT64{64} << 10);
    jobs.size = std::max(jobs.size, Align16(size + size / 2));
    jobs.buffer = CreateBuffer(jobs.size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!jobs.buffer || FAILED(jobs.buffer->Map(0, nullptr, reinterpret_cast<void**>(&jobs.mapped)))) {
        jobs = {};
        return false;
    }
    jobs.view = m_descriptors->CreateRawView(jobs.buffer.Get());
    if (jobs.view == DescriptorHeap::kInvalid) {
        jobs = {};
        return false;
    }
    return true;
}

void SkinnedCasters::Release(EntityState& state)
{
    for (SlotState& slot : state.slots) {
        m_descriptors->Free(slot.verticesView);
        m_descriptors->Free(slot.verticesUav);
    }
    state = {};
}

void SkinnedCasters::ReleaseRetired(bool all)
{
    std::erase_if(m_retired, [&](auto& entry) {
        if (!all && m_records - entry.first < Scene::kFrameSlots)
            return false;
        Release(entry.second);
        return true;
    });
}

void SkinnedCasters::Clear()
{
    m_pending.clear();
    ReleaseRetired(true);
    for (auto& [key, state] : m_states)
        Release(state);
    m_states.clear();
    for (auto& [model, source] : m_sources)
        m_descriptors->Free(source.view);
    m_sources.clear();
    for (JobBuffer& jobs : m_jobs) {
        m_descriptors->Free(jobs.view);
        jobs = {};
    }
}

std::optional<SkinnedCasters::Jobs> SkinnedCasters::BeginUpdate(ID3D12GraphicsCommandList4* list, const Scene& scene,
    const std::vector<SkinnedEntity>& entities)
{
    ++m_frame;
    m_pending.clear();
    if (entities.empty())
        return std::nullopt;
    ++m_records;
    ReleaseRetired(false);
    // The jobs, then each job's bone transforms.
    const UINT64 jobsBytes = Align16(sizeof(Job) * entities.size());
    UINT64 bytes = jobsBytes;
    for (const SkinnedEntity& entity : entities)
        bytes += sizeof(studio::Matrix3x4) * entity.model->poseToBone.size();
    JobBuffer& jobs = m_jobs[m_records % Scene::kFrameSlots];
    if (!EnsureJobs(jobs, bytes))
        return std::nullopt;
    auto* records = reinterpret_cast<Job*>(jobs.mapped);
    UINT64 bonesAt = jobsBytes;
    UINT maxVertices = 0;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (const SkinnedEntity& entity : entities) {
        EntityState& state = m_states[entity.key];
        if (state.model != entity.model) {
            // Frames in flight may still read the old model's slots.
            if (state.model)
                m_retired.emplace_back(m_records, std::move(state));
            state = {};
            state.model = entity.model;
        }
        state.lastSeen = m_frame;
        const SlotState& previousSlot = state.slots[state.slot];
        const UINT previousPositions = state.lastRecord + 1 == m_records && previousSlot.built ? previousSlot.verticesView : DescriptorHeap::kInvalid;
        state.slot = (state.slot + 1) % Scene::kFrameSlots;
        state.lastRecord = m_records;
        SlotState& slot = state.slots[state.slot];
        const SkinnedModel& model = *entity.model;
        const ModelSource* source = Source(list, entity.model, barriers);
        if (source == nullptr || !PrepareSlot(slot, model, scene, entity.sceneModel))
            continue;

        auto* bones = reinterpret_cast<studio::Matrix3x4*>(jobs.mapped + bonesAt);
        for (size_t b = 0; b < model.poseToBone.size(); ++b)
            bones[b] = b < entity.boneToWorld.size() ? studio::Multiply(entity.boneToWorld[b], model.poseToBone[b]) : studio::Matrix3x4{};
        const UINT vertexCount = static_cast<UINT>(model.geometry.positions.size());
        records[m_pending.size()] = {source->view, vertexCount, slot.verticesUav, static_cast<uint32_t>(bonesAt)};
        bonesAt += sizeof(studio::Matrix3x4) * model.poseToBone.size();
        maxVertices = std::max(maxVertices, vertexCount);
        m_pending.push_back({&state, entity.sceneModel, entity.mask, previousPositions, entity.flags});
    }
    if (!barriers.empty())
        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    if (m_pending.empty())
        return std::nullopt;
    return Jobs{jobs.view, static_cast<UINT>(m_pending.size()), (maxVertices + kThreadsPerGroup - 1) / kThreadsPerGroup};
}

void SkinnedCasters::FinishUpdate(ID3D12GraphicsCommandList4* list, const Scene& scene, std::vector<SceneInstance>& instances)
{
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (const Pending& pending : m_pending) {
        SlotState& slot = pending.state->slots[pending.state->slot];
        barriers.push_back(Transition(slot.vertices.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE));
    }
    if (!barriers.empty())
        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    for (const Pending& pending : m_pending) {
        SlotState& slot = pending.state->slots[pending.state->slot];
        const SkinnedModel& model = *pending.state->model;
        D3D12_RAYTRACING_GEOMETRY_DESC descs[2];
        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC triangles;
        D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC linkage;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
        Describe(model, scene, pending.sceneModel, slot.vertices->GetGPUVirtualAddress(), model.indexBuffer->GetGPUVirtualAddress(), desc.Inputs, descs,
            triangles, linkage);
        if (slot.built) {
            desc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            desc.SourceAccelerationStructureData = slot.blas->GetGPUVirtualAddress();
        }
        desc.DestAccelerationStructureData = slot.blas->GetGPUVirtualAddress();
        desc.ScratchAccelerationStructureData = slot.scratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        slot.built = true;

        SceneInstance& instance = instances.emplace_back();
        instance.model = pending.sceneModel;
        instance.blas = slot.blas->GetGPUVirtualAddress();
        instance.positionsView = slot.verticesView;
        instance.previousPositionsView = pending.previousPositions;
        instance.mask = pending.mask;
        instance.flags = pending.flags;
        std::memset(instance.transform, 0, sizeof(instance.transform));
        instance.transform[0][0] = instance.transform[1][1] = instance.transform[2][2] = 1.0f;
    }
    if (!m_pending.empty()) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &barrier);
    }
    m_pending.clear();
    std::erase_if(m_states, [this](auto& entry) {
        if (m_frame - entry.second.lastSeen <= kForgetAfterFrames)
            return false;
        Release(entry.second);
        return true;
    });
}

}
