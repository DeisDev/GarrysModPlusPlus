#include "skinned_casters.h"

#include "log.h"

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

constexpr uint64_t kForgetAfterFrames = 120;
constexpr UINT kDynamicMask = 0x04;

studio::Matrix3x4 Multiply(const studio::Matrix3x4& a, const studio::Matrix3x4& b)
{
    studio::Matrix3x4 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c)
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (c == 3 ? a[r][3] : 0.0f);
    }
    return out;
}

bsp::Float3 Transform(const studio::Matrix3x4& m, const bsp::Float3& p)
{
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
        m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
        m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
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

bool SkinnedCasters::PrepareSlot(SlotState& slot, const SkinnedModel& model)
{
    if (slot.blas)
        return true;
    const UINT vertexCount = static_cast<UINT>(model.geometry.positions.size());
    slot.vertices = CreateBuffer(sizeof(bsp::Float3) * vertexCount, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!slot.vertices || FAILED(slot.vertices->Map(0, nullptr, reinterpret_cast<void**>(&slot.mapped))))
        return false;

    D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(bsp::Float3);
    geometry.Triangles.VertexCount = vertexCount;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.IndexCount = static_cast<UINT>(model.geometry.indices.size());
    geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.NumDescs = 1;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    slot.scratch = CreateBuffer(std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes), D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slot.blas = CreateBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    slot.built = false;
    return slot.scratch && slot.blas;
}

void SkinnedCasters::Record(ID3D12GraphicsCommandList4* list, UINT slotIndex, const std::vector<SkinnedEntity>& entities, std::vector<SceneInstance>& instances)
{
    ++m_frame;
    bool recorded = false;
    std::vector<studio::Matrix3x4> skin;
    for (const SkinnedEntity& entity : entities) {
        EntityState& state = m_states[entity.key];
        if (state.model != entity.model) {
            state = {};
            state.model = entity.model;
        }
        state.lastSeen = m_frame;
        SlotState& slot = state.slots[slotIndex % Scene::kFrameSlots];
        const SkinnedModel& model = *entity.model;
        if (!PrepareSlot(slot, model))
            continue;

        skin.resize(model.poseToBone.size());
        for (size_t b = 0; b < skin.size() && b < entity.boneToWorld.size(); ++b)
            skin[b] = Multiply(entity.boneToWorld[b], model.poseToBone[b]);
        for (size_t v = 0; v < model.geometry.positions.size(); ++v) {
            const studio::VertexWeights& w = model.geometry.weights[v];
            const bsp::Float3& p = model.geometry.positions[v];
            bsp::Float3 out = {};
            const int count = std::clamp<int>(w.count, 1, 3);
            for (int i = 0; i < count; ++i) {
                const size_t bone = w.bone[i] < skin.size() ? w.bone[i] : 0;
                const float weight = w.count == 0 ? 1.0f : w.weight[i];
                const bsp::Float3 t = Transform(skin[bone], p);
                out.x += t.x * weight;
                out.y += t.y * weight;
                out.z += t.z * weight;
            }
            slot.mapped[v] = out;
        }

        D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometry.Triangles.VertexBuffer.StartAddress = slot.vertices->GetGPUVirtualAddress();
        geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(bsp::Float3);
        geometry.Triangles.VertexCount = static_cast<UINT>(model.geometry.positions.size());
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.IndexBuffer = model.indexBuffer->GetGPUVirtualAddress();
        geometry.Triangles.IndexCount = static_cast<UINT>(model.geometry.indices.size());
        geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
        desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        if (slot.built) {
            desc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            desc.SourceAccelerationStructureData = slot.blas->GetGPUVirtualAddress();
        }
        desc.Inputs.NumDescs = 1;
        desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        desc.Inputs.pGeometryDescs = &geometry;
        desc.DestAccelerationStructureData = slot.blas->GetGPUVirtualAddress();
        desc.ScratchAccelerationStructureData = slot.scratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        slot.built = true;
        recorded = true;

        SceneInstance& instance = instances.emplace_back();
        instance.model = -1;
        instance.blas = slot.blas->GetGPUVirtualAddress();
        instance.mask = kDynamicMask;
        std::memset(instance.transform, 0, sizeof(instance.transform));
        instance.transform[0][0] = instance.transform[1][1] = instance.transform[2][2] = 1.0f;
    }
    if (recorded) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &barrier);
    }
    std::erase_if(m_states, [this](const auto& entry) { return m_frame - entry.second.lastSeen > kForgetAfterFrames; });
}

}
