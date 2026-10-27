#pragma once

#include "scene.h"
#include "studio_model.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <memory>
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

// Animated shadow casters: skins vertices on the CPU each frame and refits one BLAS per entity and
// frame slot, so frames still in flight keep their own copy. Each entity moves to its next slot every frame it
// is recorded, which keeps last frame's vertices for motion vectors.
class SkinnedCasters {
public:
    void Initialize(ID3D12Device5* device, DescriptorHeap& descriptors)
    {
        m_device = device;
        m_descriptors = &descriptors;
    }
    bool PrepareModel(SkinnedModel& model);
    // Each entity's slot is reused every Scene::kFrameSlots calls with it, so the caller must wait for the frame
    // that many calls back before recording.
    void Record(ID3D12GraphicsCommandList4* list, const Scene& scene, const std::vector<SkinnedEntity>& entities, std::vector<SceneInstance>& instances);
    void Clear();

private:
    struct SlotState {
        Microsoft::WRL::ComPtr<ID3D12Resource> vertices;
        bsp::Float3* mapped = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
        Microsoft::WRL::ComPtr<ID3D12Resource> blas;
        UINT verticesView = DescriptorHeap::kInvalid;
        bool built = false;
    };
    struct EntityState {
        std::shared_ptr<SkinnedModel> model;
        std::array<SlotState, Scene::kFrameSlots> slots;
        uint64_t lastSeen = 0;
        UINT slot = 0;
        // The Record call with entities that last included this entity.
        uint64_t lastRecord = 0;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);
    bool PrepareSlot(SlotState& slot, const SkinnedModel& model, const Scene& scene, int sceneModel);
    // Describes an entity's BLAS: the skinned vertices in `vertices` and, where its scene model has them, its
    // opacity micromaps (`triangles` and `linkage` back the descs).
    static void Describe(const SkinnedModel& model, const Scene& scene, int sceneModel,
        D3D12_GPU_VIRTUAL_ADDRESS vertices, D3D12_GPU_VIRTUAL_ADDRESS indices, D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
        D3D12_RAYTRACING_GEOMETRY_DESC (&descs)[2], D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles, D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage);
    void Release(EntityState& state);

    ID3D12Device5* m_device = nullptr;
    DescriptorHeap* m_descriptors = nullptr;
    std::unordered_map<int, EntityState> m_states;
    uint64_t m_frame = 0;
    uint64_t m_records = 0;
};

}
