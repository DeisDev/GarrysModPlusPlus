#pragma once

#include "descriptor_heap.h"

#include <NRD.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <vector>

namespace gmodpp {

// NVIDIA NRD's diffuse and specular denoiser (RELAX or REBLUR) or its sun shadow denoiser (SIGMA), driven through its C API with our own D3D12 pipelines:
// one root signature for all its pipelines (a descriptor table of its SRVs then UAVs, a root constant buffer and its
// static samplers, at the registers and spaces InstanceDesc names, as NRDIntegration.hpp lays them out), its texture pools,
// and descriptors and constants from rings per frame slot.
class NrdDenoiser {
public:
    enum class Method { Relax, Reblur, Sigma };

    // A texture the application provides for one of NRD's resource types, with its state, which Denoise updates.
    struct Resource {
        ID3D12Resource* texture = nullptr;
        D3D12_RESOURCE_STATES* state = nullptr;
    };
    using Resources = std::array<Resource, static_cast<size_t>(nrd::ResourceType::MAX_NUM)>;

    // Creates the denoiser for `width` x `height` pixels; false (logged) when it cannot.
    bool Initialize(ID3D12Device* device, DescriptorHeap& descriptors, Method method, UINT width, UINT height);
    void Release();
    ~NrdDenoiser() { Release(); }
    bool Matches(Method method, UINT width, UINT height) const { return m_instance != nullptr && method == m_method && width == m_width && height == m_height; }
    Method CurrentMethod() const { return m_method; }

    // Records NRD's dispatches for a frame into `list`, which then needs its descriptor heap and root signature again.
    // `slot` picks the rings the GPU is done with; `lightDirection` (towards the sun, world space) is SIGMA's.
    bool Denoise(ID3D12GraphicsCommandList* list, UINT slot, const nrd::CommonSettings& common, const Resources& resources,
        const float (&lightDirection)[3] = {});

private:
    // The frame rate, low-pass filtered, measured between calls to Denoise (once per frame).
    float SmoothedFps();

    struct PoolTexture {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };

    ID3D12Device* m_device = nullptr;
    DescriptorHeap* m_descriptors = nullptr;
    Method m_method = Method::Relax;
    UINT m_width = 0;
    UINT m_height = 0;
    nrd::Instance* m_instance = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    std::vector<Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_pipelines;
    // Permanent pool first, then the transient pool (InstanceDesc's order).
    std::vector<PoolTexture> m_pool;
    UINT m_permanentPoolSize = 0;
    UINT m_tableTextures = 0;
    UINT m_tableStorages = 0;
    UINT m_tableSize = 0;
    // Descriptor tables and constants for kMaxDispatches dispatches per frame slot.
    static constexpr UINT kSlots = 3;
    static constexpr UINT kMaxDispatches = 64;
    UINT m_tables = DescriptorHeap::kInvalid;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constants;
    std::byte* m_constantsMapped = nullptr;
    UINT m_constantStride = 0;
    LONGLONG m_lastDenoiseTicks = 0;
    float m_smoothedFrameMs = 1000.0f / 60.0f;
};

}
