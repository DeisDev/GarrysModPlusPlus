#pragma once

#include "scene.h"
#include "studio_model.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace gmodpp {

struct SkinnedModel {
    // Triangles in the scene model's order: alpha-tested ones last, from opaqueTriangles on.
    studio::Geometry geometry;
    UINT opaqueTriangles = 0;
    std::vector<studio::Matrix3x4> poseToBone;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
};

struct SkinnedEntity {
    int key;
    // The scene model with the same triangles; its indices and albedo are shared with the skinned copy.
    int sceneModel;
    std::shared_ptr<SkinnedModel> model;
    std::vector<studio::Matrix3x4> boneToWorld;
    UINT mask;
};

// Animated shadow casters: shaders/skin.hlsl skins each entity's vertices on the GPU every frame, into a buffer of the
// entity's for the frame slot, and that slot's BLAS is refit, so frames still in flight keep their own copy. Each entity
// moves to its next slot every frame it is recorded, which keeps last frame's vertices for motion vectors.
class SkinnedCasters {
public:
    void Initialize(ID3D12Device5* device, DescriptorHeap& descriptors)
    {
        m_device = device;
        m_descriptors = &descriptors;
    }
    bool PrepareModel(SkinnedModel& model);
    // This update's skinning jobs for shaders/skin.hlsl: thread group y is the job, x covers its vertices.
    struct Jobs {
        UINT jobsView;
        UINT count;
        UINT groupsX;
    };
    // Writes the skinning jobs of `entities`, each moved to its next slot, and makes their vertex buffers writable;
    // nothing when none can be skinned. The caller dispatches the jobs, then calls FinishUpdate, with or without jobs.
    // Each entity's slot is reused every Scene::kFrameSlots calls with entities, so the caller must wait for the frame
    // that many calls back first.
    std::optional<Jobs> BeginUpdate(ID3D12GraphicsCommandList4* list, const Scene& scene, const std::vector<SkinnedEntity>& entities);
    // Refits the BLASes of the entities BeginUpdate skinned and adds their instances to `instances`.
    void FinishUpdate(ID3D12GraphicsCommandList4* list, const Scene& scene, std::vector<SceneInstance>& instances);
    void Clear();

private:
    struct SlotState {
        // This slot's skinned positions, written by shaders/skin.hlsl (UNORDERED_ACCESS while `writable`, otherwise
        // ALL_SHADER_RESOURCE for the BLAS refit and the shaders that read hit surfaces).
        Microsoft::WRL::ComPtr<ID3D12Resource> vertices;
        Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
        Microsoft::WRL::ComPtr<ID3D12Resource> blas;
        UINT verticesView = DescriptorHeap::kInvalid;
        UINT verticesUav = DescriptorHeap::kInvalid;
        bool writable = true;
        bool built = false;
    };
    struct EntityState {
        std::shared_ptr<SkinnedModel> model;
        std::array<SlotState, Scene::kFrameSlots> slots;
        uint64_t lastSeen = 0;
        UINT slot = 0;
        // The BeginUpdate call with entities that last included this entity.
        uint64_t lastRecord = 0;
    };
    // A model's bind pose with its bone weights (SourceVertex records), copied to the GPU the first time an entity of
    // it is skinned. The model is held so its address, the key, is not reused while the entry exists.
    struct ModelSource {
        std::shared_ptr<SkinnedModel> model;
        Microsoft::WRL::ComPtr<ID3D12Resource> upload;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        UINT view = DescriptorHeap::kInvalid;
    };
    // Each update's jobs and bone transforms, by m_records modulo the frame slots, grown as needed.
    struct JobBuffer {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        std::byte* mapped = nullptr;
        UINT64 size = 0;
        UINT view = DescriptorHeap::kInvalid;
    };
    // An entity BeginUpdate skinned, for FinishUpdate.
    struct Pending {
        EntityState* state;
        int sceneModel;
        UINT mask;
        UINT previousPositions;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);
    bool PrepareSlot(SlotState& slot, const SkinnedModel& model, const Scene& scene, int sceneModel);
    // The model's source on the GPU, recording its copy into `list` (with its barrier in `barriers`) the first time.
    const ModelSource* Source(ID3D12GraphicsCommandList4* list, const std::shared_ptr<SkinnedModel>& model, std::vector<D3D12_RESOURCE_BARRIER>& barriers);
    bool EnsureJobs(JobBuffer& jobs, UINT64 size);
    // Describes an entity's BLAS: the skinned vertices in `vertices` and, where its scene model has them, its
    // opacity micromaps (`triangles` and `linkage` back the descs).
    static void Describe(const SkinnedModel& model, const Scene& scene, int sceneModel,
        D3D12_GPU_VIRTUAL_ADDRESS vertices, D3D12_GPU_VIRTUAL_ADDRESS indices, D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
        D3D12_RAYTRACING_GEOMETRY_DESC (&descs)[2], D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles, D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage);
    void Release(EntityState& state);

    ID3D12Device5* m_device = nullptr;
    DescriptorHeap* m_descriptors = nullptr;
    std::unordered_map<int, EntityState> m_states;
    std::unordered_map<const SkinnedModel*, ModelSource> m_sources;
    std::array<JobBuffer, Scene::kFrameSlots> m_jobs;
    std::vector<Pending> m_pending;
    uint64_t m_frame = 0;
    uint64_t m_records = 0;
};

}
