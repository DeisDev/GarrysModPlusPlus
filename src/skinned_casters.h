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
    studio::Geometry geometry;
    std::vector<studio::Matrix3x4> poseToBone;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
};

struct SkinnedEntity {
    int key;
    std::shared_ptr<SkinnedModel> model;
    std::vector<studio::Matrix3x4> boneToWorld;
};

// Animated shadow casters: skins vertices on the CPU each frame and refits one BLAS per entity and
// frame slot, so frames still in flight keep their own copy.
class SkinnedCasters {
public:
    void Initialize(ID3D12Device5* device) { m_device = device; }
    bool PrepareModel(SkinnedModel& model);
    void Record(ID3D12GraphicsCommandList4* list, UINT slot, const std::vector<SkinnedEntity>& entities, std::vector<SceneInstance>& instances);
    void Clear() { m_states.clear(); }

private:
    struct SlotState {
        Microsoft::WRL::ComPtr<ID3D12Resource> vertices;
        bsp::Float3* mapped = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
        Microsoft::WRL::ComPtr<ID3D12Resource> blas;
        bool built = false;
    };
    struct EntityState {
        std::shared_ptr<SkinnedModel> model;
        std::array<SlotState, Scene::kFrameSlots> slots;
        uint64_t lastSeen = 0;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);
    bool PrepareSlot(SlotState& slot, const SkinnedModel& model);

    ID3D12Device5* m_device = nullptr;
    std::unordered_map<int, EntityState> m_states;
    uint64_t m_frame = 0;
};

}
