#pragma once

#include "scene.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace gmodpp {

// A material's $treesway parameters as VertexLitGeneric passes them to its vertex shader. Mirrors SwayMaterial in
// shaders/sway.hlsl.
struct SwayMaterial {
    float height = 1000.0f;
    float startHeight = 0.1f;
    float radius = 300.0f;
    float startRadius = 0.2f;
    float speed = 1.0f;
    float strength = 10.0f;
    float scrumbleFrequency = 12.0f;
    float scrumbleStrength = 10.0f;
    float highWindSpeedMultiplier = 2.0f;
    float scrumbleFalloffExp = 1.0f;
    float falloffExp = 1.5f;
    float scrumbleSpeed = 5.0f;
    float speedLerpStart = 3.0f;
    float speedLerpEnd = 6.0f;
    // $treesway: 1 for trees and plants, 2 for hanging sheets and vines.
    uint32_t mode = 1;
    // $treeswaystatic: a fixed wind instead of the game's.
    uint32_t staticWind = 0;
};

// A static prop model with swaying materials: each vertex's index into the SwayingProps materials, or kNoSway.
struct SwayingModel {
    static constexpr uint32_t kNoSway = 0xFFFFFFFF;
    int sceneModel = -1;
    std::vector<uint32_t> vertexMaterials;
};

struct SwayingProp {
    std::shared_ptr<SwayingModel> model;
    SceneInstance instance;
};

// Static props whose materials sway in the wind. The game moves their vertices in its vertex shader, so their
// shadows would stand still with the model's BLAS. Each main view, shaders/sway.hlsl moves a copy of the vertices
// of every prop within kSwayDistance of the eye the same way, and that prop's own BLAS is refit in place; farther
// props keep the model's still BLAS.
class SwayingProps {
public:
    void Initialize(ID3D12Device5* device, DescriptorHeap& descriptors)
    {
        m_device = device;
        m_descriptors = &descriptors;
    }
    // Replaces the props; their models' vertexMaterials index `materials`. False when GPU resources fail, which
    // leaves no props.
    bool SetProps(const Scene& scene, std::span<const SwayMaterial> materials, std::vector<SwayingProp> props);
    void Clear();
    bool Empty() const { return m_props.empty(); }

    // This frame's sway jobs, written to `slot`'s part of the job buffer for shaders/sway.hlsl.
    struct Jobs {
        UINT jobsView;
        UINT materialsView;
        UINT currentUav;
        UINT previousUav;
        UINT count;
        UINT groupsX;
    };
    // Picks the props near `eye` and writes their jobs; nothing when none is near. The caller dispatches the
    // jobs and then calls FinishUpdate.
    std::optional<Jobs> BeginUpdate(ID3D12GraphicsCommandList4* list, const Scene& scene, UINT slot, const float (&eye)[3], float time,
        float previousTime, const float (&wind)[2], const float (&previousWind)[2]);
    // Refits the BLASes of the props BeginUpdate moved.
    void FinishUpdate(ID3D12GraphicsCommandList4* list, const Scene& scene);
    // Adds every prop to `instances`: moved ones with their own BLAS and vertices, the others as plain instances.
    void Append(std::vector<SceneInstance>& instances) const;

private:
    // Mirrors SwayHeader and SwayJob in shaders/sway.hlsl.
    struct Header;
    struct Job;
    struct PropState {
        UINT64 vertexOffset = 0;
        UINT64 blasOffset = 0;
        UINT64 scratchOffset = 0;
        UINT currentView = DescriptorHeap::kInvalid;
        UINT previousView = DescriptorHeap::kInvalid;
        UINT vertexMaterialsView = DescriptorHeap::kInvalid;
        bool built = false;
        bool moved = false;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);
    void Describe(const Scene& scene, const SwayingProp& prop, D3D12_GPU_VIRTUAL_ADDRESS vertices, D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
        D3D12_RAYTRACING_GEOMETRY_DESC (&descs)[2], D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles, D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage) const;

    ID3D12Device5* m_device = nullptr;
    DescriptorHeap* m_descriptors = nullptr;
    std::vector<SwayingProp> m_props;
    std::vector<PropState> m_states;
    struct VertexMaterials {
        std::shared_ptr<SwayingModel> model;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        UINT view = DescriptorHeap::kInvalid;
    };
    std::vector<VertexMaterials> m_vertexMaterials;
    // Every view this class created.
    std::vector<UINT> m_views;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materials;
    UINT m_materialsView = DescriptorHeap::kInvalid;
    // Every prop's vertices for this frame and the last, its BLAS, and the scratch memory of its refits.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_current;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_previous;
    UINT m_currentUav = DescriptorHeap::kInvalid;
    UINT m_previousUav = DescriptorHeap::kInvalid;
    bool m_verticesWritable = true;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_blas;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_scratch;
    // The jobs of each frame slot.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_jobs;
    std::byte* m_jobsMapped = nullptr;
    UINT64 m_jobsSlotSize = 0;
    std::array<UINT, Scene::kFrameSlots> m_jobsViews = {};
};

}
