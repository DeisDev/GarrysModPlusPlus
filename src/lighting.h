#pragma once

#include "gbuffer.h"
#include "dlss.h"
#include "scene.h"
#include "settings.h"
#include "skinned_casters.h"

#include <d3d12.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <wrl/client.h>

#include <array>
#include <span>
#include <vector>

namespace gmodpp {

class Renderer;

// Mirrors FrameConstants in shaders/common.hlsli.
struct FrameConstants {
    float invViewProj[4][4];
    float viewProj[4][4];
    float prevViewProj[4][4];
    float eyePosition[3];
    float tonemapScale;
    float sunDirection[3];
    float sunEnabled;
    float sunColor[3];
    uint32_t outputSrgb;
    float skyColor[3];
    uint32_t debugView;
    float invSize[2];
    uint32_t size[2];
    float normalBias;
    uint32_t traceMask;
    uint32_t frameIndex;
    uint32_t historyValid;
    float prevEyePosition[3];
    uint32_t lightCount;
    uint32_t instancesView;
    uint32_t lightsView;
    uint32_t depthView;
    uint32_t colorView;
    uint32_t gbufferNormalView;
    uint32_t flagsView;
    uint32_t surfaceView;
    uint32_t prevSurfaceView;
    float fogParams[4];
    float fogColor[3];
    uint32_t fogEnabled;
    uint32_t lightView;
    uint32_t dynamicLightsView;
    uint32_t dynamicLightCount;
    uint32_t projectorsView;
    uint32_t projectorCount;
    float lightScale;
    float indirectScale;
    float taaHistoryWeight;
    float sharpening;
    float depthMin;
    float depthScale;
    uint32_t albedoView;
    uint32_t emissiveView;
    uint32_t materialsView;
    float reflectionScale;
    uint32_t outputLinear;
    uint32_t lightRangesView;
    uint32_t lightIndicesView;
    uint32_t bspNodesView;
    uint32_t leafClustersView;
    uint32_t clusterCount;
    uint32_t viewmodel;
};

// The game's D3D9 resources one view rendered: its colour target, which keeps what the G-buffer does not
// cover, and the G-buffer.
struct ViewInputs {
    IDirect3DSurface9* color;
    IDirect3DTexture9* depth;
    IDirect3DTexture9* normal;
    IDirect3DTexture9* flags;
    IDirect3DTexture9* light;
    IDirect3DTexture9* albedo;
    IDirect3DTexture9* emissive;
    std::span<const gbuffer::ProjectedLight> projected;
    gbuffer::ViewKind kind;
    // The viewport depth range the view was drawn with.
    float depthMin;
    float depthMax;
    uint64_t frame;
    // How far the view's image is shifted by the jitter, in render pixels (x right, y down).
    float jitterX = 0.0f;
    float jitterY = 0.0f;
};

// Lights a view's G-buffer on the D3D12 queue the game shares and writes the result back into its colour
// target: traced sun and local lights, plus one-bounce traced indirect light that is accumulated over
// frames and denoised. The 3D skybox is lit without tracing.
class Lighting {
public:
    bool Initialize(Renderer& renderer, IDirect3DDevice9* device);
    void Run(const ViewInputs& inputs, Scene& scene, std::span<const SceneInstance> dynamic, const std::vector<SkinnedEntity>& skinned);
    SkinnedCasters& Skinned() { return m_skinned; }
    // The resolution the main view's ray tracing runs at when DLSS Ray Reconstruction is in use; false when the
    // view is lit at the output resolution instead.
    bool RayReconstructionSize(UINT outputWidth, UINT outputHeight, UINT& width, UINT& height);
    void WaitForGpu();
    void InvalidateHistory() { m_historyValid = false; }

private:
    struct Texture {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        UINT srv = DescriptorHeap::kInvalid;
        UINT uav = DescriptorHeap::kInvalid;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };
    enum class Pass { Surface, GiTrace, GiTemporal, GiFilter, Lighting, TemporalAntiAliasing, Sharpen, Reflections, DlssInputs, DlssResolve, Count };
    // Render-resolution copies of the G-buffer for Ray Reconstruction, written together by Pass::DlssInputs.
    enum RayReconstructionInput { RrColor, RrDepth, RrNormal, RrFlags, RrLight, RrAlbedo, RrEmissive, RrMotion, RrInputCount };

    bool EnsureTargets(UINT width, UINT height);
    bool CreateTarget(Texture& texture, UINT width, UINT height, DXGI_FORMAT format);
    void Use(Texture& texture, D3D12_RESOURCE_STATES state);
    void Dispatch(Pass pass, const std::array<UINT, 8>& constants);
    bool FillConstants(FrameConstants& constants, const Scene& scene, const ViewInputs& inputs);
    void ReturnResources(std::span<IDirect3DResource9* const> resources, UINT count);
    UINT WriteProjectors(UINT slot, std::span<const gbuffer::ProjectedLight> projected, std::span<ID3D12Resource* const> cookies, UINT cookieViews);

    Renderer* m_renderer = nullptr;
    ID3D12Device5* m_device = nullptr;
    DescriptorHeap* m_descriptors = nullptr;
    Microsoft::WRL::ComPtr<IDirect3DDevice9On12> m_device9On12;
    Microsoft::WRL::ComPtr<IDirect3DQuery9> m_flushQuery;
    static constexpr UINT kSlots = Scene::kFrameSlots;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kSlots> m_allocators;
    std::array<uint64_t, kSlots> m_slotFenceValues = {};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> m_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvIncrement = 0;
    UINT m_slotViews = DescriptorHeap::kInvalid;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constants;
    std::byte* m_constantsMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_projectors;
    std::byte* m_projectorsMapped = nullptr;
    std::array<UINT, kSlots> m_projectorViews = {};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRoot;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, static_cast<size_t>(Pass::Count)> m_pipelines;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_compositeRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_compositePipeline;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_colorCopy;
    std::array<Texture, 2> m_surface;
    Texture m_giRaw;
    std::array<Texture, 2> m_giHistory;
    std::array<Texture, 2> m_giFiltered;
    // Shaded colour; the main view alternates between the two so the reflections pass can read last frame's.
    std::array<Texture, 2> m_lit;
    Texture m_reflections;
    std::array<Texture, 2> m_taaHistory;
    // Indirect light of the viewmodel, filtered without history so the main view's buffers stay intact.
    std::array<Texture, 2> m_viewmodelIndirect;
    Texture m_resolved;
    std::array<Texture, RrInputCount> m_rrInputs;
    // Consecutive UAVs of m_rrInputs.
    UINT m_rrInputUavs = DescriptorHeap::kInvalid;
    Texture m_rrSpecularAlbedo;
    Texture m_rrRoughness;
    Texture m_rrOutput;
    RayReconstruction m_rayReconstruction;
    DlssMode m_rrSizeMode = DlssMode::Count;
    UINT m_rrSize[4] = {};
    bool m_lastMainUsedRr = false;
    UINT m_width = 0;
    UINT m_height = 0;
    // The size compute passes run over: the output size, or the render size for Ray Reconstruction.
    UINT m_gridWidth = 0;
    UINT m_gridHeight = 0;

    SkinnedCasters m_skinned;
    std::vector<SceneInstance> m_frameInstances;
    float m_prevViewProj[4][4] = {};
    float m_prevEye[3] = {};
    bool m_historyValid = false;
    bool m_taaHistoryValid = false;
    uint64_t m_lastMainFrame = 0;
    uint64_t m_mainRuns = 0;

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestamps;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_timestampReadback;
    const uint64_t* m_timestampValues = nullptr;
    double m_timestampPeriodMs = 0.0;
    std::array<double, static_cast<size_t>(GpuPass::Count)> m_passMsTotal = {};
    uint32_t m_gpuSamples = 0;
    uint32_t m_timingReports = 0;
    uint32_t m_cameraFailures = 0;
    std::array<bool, kSlots> m_slotWasMain = {};
    uint64_t m_run = 0;
    bool m_loggedFormats = false;
    bool m_failed = false;
};

}
