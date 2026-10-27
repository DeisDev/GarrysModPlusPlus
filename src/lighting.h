#pragma once

#include "gbuffer.h"
#include "dlss.h"
#include "scene.h"
#include "settings.h"
#include "skinned_casters.h"
#include "swaying_props.h"

#include <d3d12.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <wrl/client.h>

#include <array>
#include <optional>
#include <span>
#include <utility>
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
    uint32_t visibilityView;
    uint32_t motionView;
    uint32_t previousDistanceView;
    uint32_t pixelFlagsView;
    uint32_t probePixel;
    uint32_t probeView;
    int32_t probeOrigin[3];
    float probeSpacing;
    uint32_t aoView;
    float aoIntensity;
    uint32_t volumetricView;
    float volumetricDensity;
    uint32_t geometricNormalView;
    uint32_t exactLights;
    uint32_t sampledLightRays;
    uint32_t indirectBounces;
    uint32_t aoRays;
    uint32_t volumeRays;
    uint32_t volumetricSteps;
    uint32_t sunShadowMask;
    uint32_t indirectMask;
    uint32_t bounceMask;
    uint32_t indirectRayFlags;
    uint32_t dispatchOriginView;
    uint32_t probeRedView;
    uint32_t probeGreenView;
    uint32_t probeBlueView;
};

// What blocks the sun at a pixel of the main view (Lighting::RequestProbe).
struct ProbeResult {
    // Scene instance, or DescriptorHeap::kInvalid when the sun is visible.
    uint32_t instance;
    uint32_t triangle;
    float distance;
    float sunVisibility;
    float origin[3];
    int model;
    uint32_t mask;
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
    // Where the main view's irradiance volume goes for the game's forward-lit draws (gbuffer::ProbeTextures).
    std::array<IDirect3DVolumeTexture9*, 3> probes = {};
};

// Lights a view's G-buffer on the D3D12 queue the game shares and writes the result back into its colour
// target: traced sun and local lights, plus one-bounce traced indirect light that is accumulated over
// frames and denoised. The 3D skybox is lit without tracing.
class Lighting {
public:
    bool Initialize(Renderer& renderer, IDirect3DDevice9* device);
    void Run(const ViewInputs& inputs, Scene& scene, std::span<const SceneInstance> dynamic, const std::vector<SkinnedEntity>& skinned);
    SkinnedCasters& Skinned() { return m_skinned; }
    SwayingProps& Sway() { return m_sway; }
    // The resolution the main view's ray tracing runs at when DLSS Ray Reconstruction is in use; false when the
    // view is lit at the output resolution instead.
    bool RayReconstructionSize(UINT outputWidth, UINT outputHeight, UINT& width, UINT& height);
    void WaitForGpu();
    void InvalidateHistory()
    {
        m_historyValid = false;
        m_probeHistoryValid = false;
    }
    // Whether the last main view filled the irradiance volume, and the volume's first cell.
    bool VolumeReady() const { return m_volumeReady; }
    std::array<int, 3> VolumeOrigin() const { return m_probeOrigin; }
    // Developer tool: the next main view reports what blocks the sun at a pixel (TakeProbe).
    void RequestProbe(UINT x, UINT y)
    {
        m_probeRequested = true;
        m_probePixel = x | y << 16;
    }
    std::optional<ProbeResult> TakeProbe() { return std::exchange(m_probe, std::nullopt); }

private:
    struct Texture {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        UINT srv = DescriptorHeap::kInvalid;
        UINT uav = DescriptorHeap::kInvalid;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };
    // DirectWide is Direct for more exact lights or shared shadow rays than the High preset's.
    enum class Pass { Surface, Primary, Probes, Ao, Volumetrics, Direct, GiTrace, GiTemporal, GiFilter, Lighting, TemporalAntiAliasing, Sharpen, Reflections, ReflectionTemporal, DlssInputs, DlssResolve, DirectWide, Sway, TranslucentDepth, ViewRect, Count };
    // Mirrors PassConstants in shaders/common.hlsli: descriptor indices (none by default) and a parameter.
    struct PassArgs {
        UINT input0 = DescriptorHeap::kInvalid;
        UINT input1 = DescriptorHeap::kInvalid;
        UINT input2 = DescriptorHeap::kInvalid;
        UINT input3 = DescriptorHeap::kInvalid;
        UINT input4 = DescriptorHeap::kInvalid;
        UINT output0 = DescriptorHeap::kInvalid;
        UINT output1 = DescriptorHeap::kInvalid;
        UINT output2 = DescriptorHeap::kInvalid;
        UINT output3 = DescriptorHeap::kInvalid;
        UINT step = 0;
        UINT output4 = DescriptorHeap::kInvalid;
        UINT padding = 0;
    };
    // Render-resolution copies of the G-buffer for Ray Reconstruction, written together by Pass::DlssInputs.
    enum RayReconstructionInput { RrColor, RrDepth, RrNormal, RrFlags, RrLight, RrAlbedo, RrEmissive, RrInputCount };

    bool EnsureTargets(UINT width, UINT height);
    bool CreateTarget(Texture& texture, UINT width, UINT height, DXGI_FORMAT format, UINT depth = 0);
    void DispatchGroups(Pass pass, const PassArgs& args, UINT x, UINT y, UINT z);
    // Moves the swaying props near the eye to this frame's sway (SwayingProps).
    void UpdateSway(const Scene& scene, UINT slot, const float (&eye)[3], D3D12_GPU_VIRTUAL_ADDRESS frameConstants);
    // Hands the slot's copy of the irradiance volume to light_queries.
    void PublishVolume(UINT slot);
    void Use(Texture& texture, D3D12_RESOURCE_STATES state);
    void Dispatch(Pass pass, const PassArgs& args);
    bool FillConstants(FrameConstants& constants, const Scene& scene, const ViewInputs& inputs);
    void ReturnResources(std::span<IDirect3DResource9* const> resources, UINT count);
    bool EnsureDepthPipeline(DXGI_FORMAT format);
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
    // Shader Model 6.9 variants of the passes that trace rays, which let traversal use opacity micromaps.
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, static_cast<size_t>(Pass::Count)> m_micromapPipelines;
    bool m_useMicromaps = false;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_compositeRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_compositePipeline;
    // The composite of the viewmodel's run, which skips what its G-buffer does not cover.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_compositeCoveredPipeline;
    // The viewmodel's run dispatches its passes over a rectangle around its G-buffer (shaders/view_rect.hlsl).
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_dispatchSignature;
    Texture m_viewRect;
    bool m_rectDispatch = false;
    // Writes m_translucentDepth into the game's depth buffer, for its format.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_depthPipeline;
    DXGI_FORMAT m_depthPipelineFormat = DXGI_FORMAT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_colorCopy;
    std::array<Texture, 2> m_surface;
    // Written by shaders/primary.hlsl.
    Texture m_visibility;
    Texture m_motion;
    Texture m_previousDistance;
    Texture m_pixelFlags;
    // The face normal of each pixel's triangle (shaders/primary.hlsl), which ray origins and directions keep to.
    Texture m_geometricNormal;
    // Exact direct light of the main view's current and last frame (shaders/direct.hlsl), and its sampled remainder.
    std::array<Texture, 2> m_direct;
    Texture m_directRemainder;
    Texture m_giRaw;
    std::array<Texture, 2> m_giHistory;
    std::array<Texture, 2> m_giMoments;
    std::array<Texture, 2> m_giFiltered;
    // The denoised light the main view used last frame, which indirect rays reuse; kInvalid when there is none.
    Texture* m_lastDenoised = nullptr;
    // Shaded colour; the main view alternates between the two so the reflections pass can read last frame's.
    std::array<Texture, 2> m_lit;
    Texture m_reflections;
    // Ambient occlusion accumulated over frames (occlusion, sample count) and blurred.
    std::array<Texture, 2> m_aoHistory;
    Texture m_ao;
    // Volumetric light at half resolution: this frame's march and the accumulation of the last two frames.
    Texture m_volumetricMarch;
    std::array<Texture, 2> m_volumetricHistory;
    bool m_volumetricHistoryValid = false;
    // Irradiance volume (shaders/probes.hlsl): red, green and blue coefficients and cells of the last two frames,
    // and the filled volume copied to the game's textures.
    std::array<std::array<Texture, 4>, 2> m_probeHistory;
    std::array<Texture, 3> m_probeVolume;
    std::array<int, 3> m_probeOrigin = {};
    bool m_probeHistoryValid = false;
    bool m_volumeReady = false;
    // Copies of the filled volume per frame slot, read once the slot's frame is done for the engine's lighting
    // queries (light_queries).
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kSlots> m_volumeReadback;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 3> m_volumeFootprints = {};
    std::array<std::array<int, 3>, kSlots> m_volumeReadbackOrigin = {};
    std::array<bool, kSlots> m_volumeReadbackPending = {};
    std::vector<uint16_t> m_volumeTexels;
    // Accumulated reflections of the main view (shaders/reflection_temporal.hlsl).
    std::array<Texture, 2> m_reflectionHistory;
    std::array<Texture, 2> m_taaHistory;
    // Indirect light of the viewmodel, filtered without history so the main view's buffers stay intact.
    std::array<Texture, 2> m_viewmodelIndirect;
    Texture m_resolved;
    // The main view's depth for its translucent surfaces (shaders/translucent_depth.hlsl).
    Texture m_translucentDepth;
    std::array<Texture, RrInputCount> m_rrInputs;
    // Consecutive UAVs of m_rrInputs.
    UINT m_rrInputUavs = DescriptorHeap::kInvalid;
    Texture m_rrSpecularAlbedo;
    Texture m_rrRoughness;
    Texture m_rrHitDistance;
    float m_worldToView[16] = {};
    float m_viewToClip[16] = {};
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
    SwayingProps m_sway;
    // The sway time and wind of the last main view, for its swaying props' motion vectors.
    bool m_swayKnown = false;
    float m_swayTime = 0.0f;
    float m_swayWind[2] = {};
    std::vector<SceneInstance> m_frameInstances;
    float m_prevViewProj[4][4] = {};
    float m_prevEye[3] = {};
    bool m_historyValid = false;
    bool m_taaHistoryValid = false;
    bool m_reflectionHistoryValid = false;
    uint64_t m_lastMainFrame = 0;
    uint64_t m_mainRuns = 0;

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestamps;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_timestampReadback;
    const uint64_t* m_timestampValues = nullptr;
    double m_timestampPeriodMs = 0.0;
    std::array<double, static_cast<size_t>(GpuPass::Count)> m_passMsTotal = {};
    uint32_t m_gpuSamples = 0;
    // GPU time of the 3D skybox's and viewmodel's runs since the last report.
    double m_otherViewsMsTotal = 0.0;
    uint32_t m_timingReports = 0;
    uint32_t m_cameraFailures = 0;
    std::array<bool, kSlots> m_slotWasMain = {};
    uint64_t m_run = 0;
    bool m_loggedFormats = false;
    bool m_probeRequested = false;
    UINT m_probePixel = 0;
    std::optional<ProbeResult> m_probe;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probeBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probeReadback;
    UINT m_probeUav = DescriptorHeap::kInvalid;
    bool m_failed = false;
};

}
