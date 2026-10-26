#pragma once

#include "bsp.h"
#include "studio_model.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace gmodpp {

struct SceneInstance {
    int model = -1;
    // Explicit BLAS for geometry the scene does not own (skinned casters); used instead of `model`.
    D3D12_GPU_VIRTUAL_ADDRESS blas = 0;
    UINT mask = 0;
    float transform[3][4];
};

// Ray-tracing acceleration structures for the current map: one BLAS per model, rebuilt TLAS per frame.
class Scene {
public:
    static constexpr UINT kFrameSlots = 3;

    bool Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue);
    bool LoadMap(const std::string& name, const bsp::WorldGeometry& world);
    // Builds a BLAS for the geometry and returns its model id, or -1 when the geometry is empty or fails.
    int AddModel(const studio::Geometry& geometry);
    void SetStaticInstances(std::vector<SceneInstance> instances) { m_staticInstances = std::move(instances); }
    void Clear();

    bool Ready() const { return m_worldModel >= 0; }
    const std::string& Name() const { return m_name; }
    const bsp::SunLight& Sun() const { return m_sun; }

    // Records a TLAS build for this frame into `list`; the caller waits for `slot`'s previous use first.
    D3D12_GPU_VIRTUAL_ADDRESS RecordFrameTlas(ID3D12GraphicsCommandList4* list, UINT slot, std::span<const SceneInstance> dynamic);

private:
    struct FrameTlas {
        Microsoft::WRL::ComPtr<ID3D12Resource> instances;
        D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
        UINT capacity = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
        Microsoft::WRL::ComPtr<ID3D12Resource> tlas;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);
    Microsoft::WRL::ComPtr<ID3D12Resource> Upload(const void* data, UINT64 size);
    int BuildBlas(const std::vector<bsp::Float3>& positions, const std::vector<uint32_t>& indices);
    bool EnsureFrameCapacity(FrameTlas& frame, UINT count);

    ID3D12Device5* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> m_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_models;
    std::vector<SceneInstance> m_staticInstances;
    std::array<FrameTlas, kFrameSlots> m_frames;
    int m_worldModel = -1;
    std::string m_name;
    bsp::SunLight m_sun;
};

}
