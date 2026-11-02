#pragma once

#include "d3d12_device.h"
#include "dlss.h"
#include "nrd_denoiser.h"
#include "restir.h"
#include "scene.h"
#include "settings.h"
#include "skinned_casters.h"
#include "swaying_props.h"
#include "view_types.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace gmodpp {

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
    uint32_t lightShadowMask;
    uint32_t indirectRayFlags;
    uint32_t dispatchOriginView;
    uint32_t probeRedView;
    uint32_t probeGreenView;
    uint32_t probeBlueView;
    uint32_t lightStylesView;
    uint32_t additiveAfterResolve;
    uint32_t glossyReflections;
    uint32_t bakedTrianglesView;
    uint32_t bakedFacesView;
    uint32_t bakedLightView;
    uint32_t leafAmbientView;
    uint32_t ambientSamplesView;
    float skyCameraX;
    float skyCameraY;
    float skyCameraZ;
    float worldToSky;
    // Phong highlights (shaders/direct.hlsl), added unlit by shaders/deferred_lighting.hlsl.
    uint32_t specularView;
    // Each water surface triangle's body of water (Scene::Water), or kInvalidView.
    uint32_t waterTrianglesView;
    // The light warp textures of the view's G-buffer twins (shaderapi_capture.h, LightWarpSlot).
    uint32_t lightWarpCount;
    // The 2D sky as the game draws it (ViewInputs::skyCube) and its blurred light for indirect rays (shaders/sky.hlsl),
    // or kInvalidView.
    uint32_t skyCubeView;
    uint32_t skyLightView;
    // Scene::LightLists' light grid.
    float gridOrigin[3];
    float gridCellSize;
    uint32_t gridDims[3];
    uint32_t gridCellsView;
    uint32_t gridIndicesView;
    uint32_t gridTailView;
    // Scene::EmittersView and EmitterCount (0 without EmissiveLighting::Full), and the EmissiveLighting setting.
    uint32_t emittersView;
    uint32_t emitterCount;
    uint32_t emissiveLighting;
    // 1 when volumetricView holds the map's fog, lit (shaders/volumetrics.hlsl), in place of Source's flat fog.
    uint32_t volumetricFog;
    // RTXDI's debug image (DebugView::LightSample and ReservoirM, shaders/restir.hlsl), or kInvalidView.
    uint32_t restirDebugView;
    // The 3D skybox's fog (ViewInputs::skyboxFog), for reflected rays that continue into it.
    float skyboxFogParams[4];
    float skyboxFogColor[3];
    uint32_t skyboxFogEnabled;
    // Self-illumination from the material table (shaders/reflections.hlsl) and NRD's validation overlay, or kInvalidView.
    uint32_t selfIllumView;
    uint32_t nrdValidationView;
    // Settings::viewmodelShadowRays.
    uint32_t viewmodelShadowRays;
    uint32_t padding;
    // The map's bodies of water (Scene::Water), each BSP leaf's body, the normal map frames they animate through, and the
    // highest surface; the body holding the eye (kInvalidView in air), the clock their normal maps animate by, and the
    // main view's caustics (shaders/caustics.hlsl) over the square from causticsOrigin, 1 / causticsInvSize across.
    uint32_t waterBodiesView;
    uint32_t waterBodyCount;
    uint32_t leafWaterView;
    uint32_t waterFramesView;
    float waterTop;
    uint32_t eyeWater;
    float waterTime;
    uint32_t causticsView;
    float causticsOrigin[2];
    float causticsInvSize;
    // The water surfaces as the game drew them (Lighting::WriteWaterDraws), one entry per body, or kInvalidView.
    uint32_t waterDrawsView;
    // The water's sun maps (shaders/caustics.hlsl): the fine one over the caustics' square, and the coarse one over
    // the square from coarseCausticsOrigin, 1 / coarseCausticsInvSize across; kInvalidView without sunlight on water.
    uint32_t waterSunView;
    uint32_t coarseWaterSunView;
    float coarseCausticsInvSize;
    // The first brush entity body of water (WaterViews::brushFirst), found by its bounds alone.
    uint32_t brushWaterFirst;
    float coarseCausticsOrigin[2];
    uint32_t padding5[2];
    uint32_t lightWarpViews[64];
};

// Mirrors SamplingConstants in shaders/nvidia/sharc_common.hlsli: the radiance cache's parameters, bound to every pass
// (root parameter 3) beside FrameConstants.
struct SamplingConstants {
    float cacheCamera[3];
    float sceneScale;
    float cacheCameraPrevious[3];
    float levelBias;
    uint32_t cacheEnabled;
    uint32_t capacity;
    uint32_t accumulationView;
    uint32_t resolvedView;
    uint32_t accumulationFrames;
    uint32_t staleFrames;
    float radianceScale;
    uint32_t padding;
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
    // On water: the rippled normal the G-buffer holds there and the one shaders rebuild from the normal map.
    bool water = false;
    float drawnWaterNormal[3] = {};
    float rebuiltWaterNormal[3] = {};
};

// What one view rendered: its colour target, which keeps what the G-buffer does not cover, and the G-buffer, with
// the capture's state. Every resource is in D3D12_RESOURCE_STATE_COMMON when Run starts and when its commands end.
struct ViewInputs {
    ID3D12Resource* color;
    ID3D12Resource* depth;
    ID3D12Resource* normal;
    ID3D12Resource* flags;
    ID3D12Resource* light;
    ID3D12Resource* albedo;
    ID3D12Resource* emissive;
    std::span<const gbuffer::ProjectedLight> projected;
    // Each projected light's cookie texture, or null for a round spot.
    std::span<ID3D12Resource* const> cookies;
    // Shader resource views of the light warp textures the G-buffer's flags refer to.
    std::span<const UINT> lightWarps;
    gbuffer::ViewKind kind;
    // The viewport depth range the view was drawn with.
    float depthMin;
    float depthMax;
    uint64_t frame;
    // How far the view's image is shifted by the jitter, in render pixels (x right, y down).
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    // Where the main view's irradiance volume goes for the game's forward-lit draws: copies of the red, green and
    // blue volumes, or none.
    std::array<ID3D12Resource*, 3> probes = {};
    // The local player's origin (its feet), when its model is known.
    std::optional<std::array<float, 3>> localPlayer;
    // Source's view-projection rows (c8-c11) the G-buffer was drawn with, without the jitter, and the jitter added
    // to them (clip-space offsets, NDC units).
    gbuffer::Matrix4 viewProj = {};
    std::array<float, 2> jitter = {};
    // The tonemap scale Source multiplies lit colours by (cLightScale.x), and whether the colour target holds sRGB.
    float tonemapScale = 1.0f;
    bool outputSrgb = false;
    gbuffer::Fog fog;
    // The 3D skybox view's fog, for reflected rays that continue into the skybox.
    gbuffer::Fog skyboxFog;
    // The 2D sky the game drew on this map (capture::SkyFaces), with views of it as a cube and as an array of its
    // faces; null without one.
    ID3D12Resource* skyCube = nullptr;
    UINT skyCubeView = DescriptorHeap::kInvalid;
    UINT skyFacesView = DescriptorHeap::kInvalid;
    // When the game last drew a swaying model.
    gbuffer::SwayClock sway;
    // The water surfaces the game drew, last time each was drawn, and the normal map frame each had bound (or null).
    std::span<const gbuffer::WaterSurfaceDraw> waterDraws;
    std::span<ID3D12Resource* const> waterNormalMaps;
    // env_sun as the client has it, and the sun GMod's painted sky (env_skypaint) last drew: the procedural sky and the
    // sun's disc show this sun rather than the map's (Lighting::RecordSky). With sun_follows_env_sun the sunlight takes
    // env_sun's direction too (Lighting::FillConstants).
    std::optional<gbuffer::EnvSun> envSun;
    std::optional<std::array<float, 3>> paintedSun;
};

// Lights a view's G-buffer on the D3D12 queue the game shares and writes the result back into its colour
// target: traced sun and local lights, plus one-bounce traced indirect light that is accumulated over
// frames and denoised. The 3D skybox is lit without tracing.
class Lighting {
public:
    // `dlssDirectory` holds nvngx_dlssd.dll.
    bool Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue, DescriptorHeap& descriptors, const RaytracingSupport& support,
        const std::filesystem::path& dlssDirectory);
    // Submits the view's commands to the queue. The game's commands that wrote the inputs must be submitted first.
    void Run(const ViewInputs& inputs, Scene& scene, std::span<const SceneInstance> dynamic, const std::vector<SkinnedEntity>& skinned);
    // Runs the rest of the map's bounce light bake without a view, waiting for each part, and reports the share done after
    // each one. Does nothing once it is baked.
    void Bake(Scene& scene, const std::function<void(float)>& progress);
    // The fence and value the commands submitted so far signal.
    ID3D12Fence* Fence() const { return m_fence.Get(); }
    uint64_t FenceValue() const { return m_fenceValue; }
    SkinnedCasters& Skinned() { return m_skinned; }
    SwayingProps& Sway() { return m_sway; }
    // The resolution the main view's ray tracing runs at when DLSS Ray Reconstruction is in use; false when the
    // view is lit at the output resolution instead.
    bool RayReconstructionSize(UINT outputWidth, UINT outputHeight, UINT& width, UINT& height);
    void WaitForGpu();
    // Forgets everything learnt about the scene: frame histories and the radiance cache (a new map).
    void InvalidateHistory()
    {
        m_historyValid = false;
        m_probeHistoryValid = false;
        m_cacheClear = true;
    }
    // Whether the last main view filled the irradiance volume for translucent surfaces, and the volume's first cell.
    bool VolumeReady() const { return m_volumeReady; }
    // 1 / the irradiance volume's average in open sunlit air when the last main view drew its fog lit, or 0.
    float LitFogScale() const { return m_litFogScale; }
    // The last main view's frame constants and scene for forward twins (shaders/forward_twin.hlsli); 0 before one ran
    // with its irradiance volume.
    D3D12_GPU_VIRTUAL_ADDRESS ForwardFrame() const { return m_volumeReady ? m_forwardFrame : 0; }
    D3D12_GPU_VIRTUAL_ADDRESS ForwardScene() const { return m_volumeReady ? m_forwardScene : 0; }
    std::array<int, 3> VolumeOrigin() const { return m_probeOrigin; }
    // What the game's 2D sky draws show, as the last main view made it.
    const gbuffer::SkyDraw& SkyDraw() const { return m_skyDraw; }
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
    enum class Pass {
        Surface,
        Primary,
        Probes,
        Ao,
        Volumetrics,
        Direct,
        GiTrace,
        GiTemporal,
        GiFilter,
        Lighting,
        TemporalAntiAliasing,
        Sharpen,
        Reflections,
        ReflectionTemporal,
        DlssInputs,
        DlssResolve,
        DirectWide,
        Sway,
        TranslucentDepth,
        ViewRect,
        Baked,
        Sky,
        SharcUpdate,
        SharcResolve,
        DirectSun,
        RestirPdf,
        RestirPresample,
        RestirRegir,
        RestirInitial,
        RestirTemporal,
        RestirSpatial,
        RestirShade,
        NrdInputs,
        NrdResolve,
        SunShadow,
        Caustics,
        Skin,
        SkyAtmosphere,
        Count
    };
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
    // Render-resolution copies of the G-buffer for Ray Reconstruction, written together by Pass::DlssInputs, and its
    // diffuse albedo guide, whose traced water pixels Pass::Lighting overwrites.
    enum RayReconstructionInput { RrColor, RrDepth, RrNormal, RrFlags, RrLight, RrAlbedo, RrEmissive, RrAlbedoGuide, RrInputCount };

    bool EnsureTargets(UINT width, UINT height);
    // Creates the radiance cache's buffers, or frees them when `wanted` is false (waiting for the GPU).
    bool EnsureRadianceCache(bool wanted);
    // Binds the compute root signature with the view's frame constants, the scene and the slot's sampling constants.
    void BindCompute(D3D12_GPU_VIRTUAL_ADDRESS frameConstants, D3D12_GPU_VIRTUAL_ADDRESS scene, UINT slot);
    // Records RTXDI's passes for the main view (shaders/restir.hlsl): the light PDF, presampling, ReGIR, initial sampling,
    // reuse in `mode`, and shading into the sampled remainder and highlights.
    void RecordRestir(RestirMode mode, bool debugImage);
    // NRD for the main view at the output size with the chosen denoiser (re)made; false when it cannot be (logged once).
    bool EnsureNrd(DenoiserType type);
    // Records NRD's denoising of the main view's raw indirect light and reflections into m_giFiltered[0] and
    // m_reflectionHistory[current] (shaders/nrd.hlsl), then binds the compute root signature again; false when NRD failed,
    // leaving them unwritten.
    bool RecordNrd(const FrameConstants& constants, const ViewInputs& inputs, UINT slot, UINT current, D3D12_GPU_VIRTUAL_ADDRESS frameConstants,
        D3D12_GPU_VIRTUAL_ADDRESS scene);
    // The camera an NRD denoiser saw in its last frame.
    struct NrdHistory {
        bool valid = false;
        uint32_t frame = 0;
        float worldToView[16] = {};
        float viewToClip[16] = {};
        float jitter[2] = {};
    };
    // NRD's settings for the main view, restarting without `history` (or the view's), which this frame's camera becomes.
    nrd::CommonSettings NrdCommonSettings(const FrameConstants& constants, const ViewInputs& inputs, NrdHistory& history);
    // NRD's SIGMA for the main view's sun shadows at the output size; false when it cannot be made (logged once).
    bool EnsureSigma();
    // Traces the main view's sun shadows to the sun's disc (shaders/sun_shadow.hlsl) and, when `denoise`, has SIGMA
    // denoise them, then binds the compute root signature again. Returns the direct pass's input0 and input1
    // (shaders/direct.hlsl, SunShadowAt); without SIGMA when it failed, none: the sun's shadows stay hard.
    std::pair<UINT, UINT> RecordSunShadow(const FrameConstants& constants, const ViewInputs& inputs, bool denoise, UINT slot,
        D3D12_GPU_VIRTUAL_ADDRESS frameConstants, D3D12_GPU_VIRTUAL_ADDRESS scene);
    bool CreateTarget(Texture& texture, UINT width, UINT height, DXGI_FORMAT format, UINT depth = 0);
    // A cube `size` texels a side: srv views it as a cube, uav and `faces` (an SRV, when given) as an array of its faces.
    bool CreateCube(Texture& texture, UINT size, DXGI_FORMAT format, UINT* faces);
    bool CreateSkyLight();
    // Makes what the main view's 2D sky shows (shaders/sky_atmosphere.hlsl): with the physical sky, the procedural sky
    // in the game's colours, which `constants` then read in place of the game's sky; with the sun's disc, what draws it.
    // Publishes both in m_skyDraw.
    void RecordSky(FrameConstants& constants, const ViewInputs& inputs, const Scene& scene);
    // Writes each body of water's surface as the game last drew it (the draw whose fog colour is the body's) into the
    // slot's m_waterDraws, with views of their normal maps from `firstView` on, and points `constants` at them.
    void WriteWaterDraws(UINT slot, const ViewInputs& inputs, const Scene& scene, UINT firstView, FrameConstants& constants);
    // Traces the main view's caustics around its eye into m_caustics (shaders/caustics.hlsl) and points `constants` at
    // them; without water near the eye or sunlight there are none.
    void RecordCaustics(FrameConstants& constants, const Scene& scene);
    // Points `constants` at the last caustics and sun maps RecordCaustics traced.
    void SetCausticsConstants(FrameConstants& constants) const;
    // Binds the pass's pipeline: its opacity micromap variant while they are in use, when it has one.
    void SetPipeline(Pass pass);
    void DispatchGroups(Pass pass, const PassArgs& args, UINT x, UINT y, UINT z);
    // Moves the swaying props near the eye to this frame's sway (SwayingProps).
    void UpdateSway(const Scene& scene, UINT slot, const gbuffer::SwayClock& clock, const float (&eye)[3], D3D12_GPU_VIRTUAL_ADDRESS frameConstants);
    // Skins `skinned` (shaders/skin.hlsl), refits their BLASes and adds their instances to m_frameInstances.
    void UpdateSkinned(const Scene& scene, const std::vector<SkinnedEntity>& skinned, UINT slot, D3D12_GPU_VIRTUAL_ADDRESS frameConstants);
    // Hands the slot's copy of the irradiance volume to light_queries.
    void PublishVolume(UINT slot);
    void Use(Texture& texture, D3D12_RESOURCE_STATES state);
    void Dispatch(Pass pass, const PassArgs& args);
    bool FillConstants(FrameConstants& constants, const Scene& scene, const ViewInputs& inputs);
    // The constants that depend on the map alone: the sun and sky as the map sets them, the 3D skybox's camera, the
    // lights, their lists and grid, the emitters and the materials.
    void FillSceneConstants(FrameConstants& constants, const Scene& scene);
    // The share of the bounce light bake done.
    float BakeProgress(Scene& scene) const;
    // After the bake's last part has run: saves the baked light to the map's cache.
    void FinishBake(Scene& scene);
    // Records the next part of the map's bounce light bake (shaders/baked_bounce.hlsl) in `frame`'s scene, with the map's
    // lights at their baked strength, then puts `frameConstants` back.
    void BakeStep(Scene& scene, const SceneFrame& frame, D3D12_GPU_VIRTUAL_ADDRESS frameConstants);
    bool EnsureDepthPipeline(DXGI_FORMAT format);
    UINT WriteProjectors(UINT slot, std::span<const gbuffer::ProjectedLight> projected, std::span<ID3D12Resource* const> cookies, UINT cookieViews,
        const FrameConstants& constants, const std::optional<std::array<float, 3>>& localPlayer);

    ID3D12Device5* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    DescriptorHeap* m_descriptors = nullptr;
    bool m_opacityMicromaps = false;
    bool m_radianceCacheSupported = false;
    // NVIDIA RTXDI for the main view's local lights (LightSampling other than Legacy).
    Restir m_restir;
    bool m_restirAvailable = false;
    // The mode and size the main view last sampled with RTXDI, whose reservoirs the next may reuse.
    std::optional<RestirMode> m_restirLastMode;
    UINT m_restirLastSize[2] = {};
    // The light style values of the last main view, for RTXDI's temporal reuse.
    std::array<float, engine::kLightStyles> m_lastLightStyles = {};
    Texture m_restirDebug;
    // NVIDIA NRD, the main view's denoiser without Ray Reconstruction (DenoiserType other than BuiltIn), its inputs and
    // outputs, and the camera of its last frame.
    NrdDenoiser m_nrd;
    bool m_nrdFailed = false;
    NrdHistory m_nrdHistory;
    Texture m_nrdViewZ;
    Texture m_nrdNormalRoughness;
    Texture m_nrdDiffuse;
    Texture m_nrdSpecular;
    Texture m_nrdDiffuseOut;
    Texture m_nrdSpecularOut;
    Texture m_nrdValidation;
    // The main view's soft sun shadows: SIGMA's noisy penumbra and translucency (or Ray Reconstruction's input), and its
    // output. SIGMA shares NRD's view depth and normal inputs.
    NrdDenoiser m_sigma;
    bool m_sigmaFailed = false;
    NrdHistory m_sigmaHistory;
    Texture m_sunPenumbra;
    Texture m_sunTranslucency;
    Texture m_sunShadow;
    // The material table's self-illumination (shaders/reflections.hlsl), added by the lighting pass.
    Texture m_selfIllum;
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
    // SamplingConstants per slot.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sampling;
    std::byte* m_samplingMapped = nullptr;
    // Bound as the radiance cache's hash entries while there is no cache: root descriptors must point somewhere.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_unusedRootUav;
    // The radiance cache (NVIDIA SHaRC): hash entries (a root UAV), accumulation and resolved buffers, all with
    // kCacheCapacity entries; `m_cacheClear` zeroes them before the next update.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cacheHash;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cacheAccumulation;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cacheResolved;
    UINT m_cacheAccumulationUav = DescriptorHeap::kInvalid;
    UINT m_cacheResolvedUav = DescriptorHeap::kInvalid;
    bool m_cacheClear = true;
    bool m_cacheWanted = false;
    // Creating the cache failed once; it is not tried again.
    bool m_cacheFailed = false;
    // The main view's eye this frame, which sizes the cache's voxels for every view, and when the cache was last updated.
    float m_cacheCamera[3] = {};
    float m_cacheCameraPrevious[3] = {};
    // The last main view's constants with its irradiance volume, per slot, for game draws that light themselves
    // (forward twins), and the scene they trace.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_forwardConstants;
    std::byte* m_forwardConstantsMapped = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_forwardFrame = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_forwardScene = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bakeConstants;
    std::byte* m_bakeConstantsMapped = nullptr;
    // The light style values the bake lights with: style 0 on, the others off, as VRAD baked bounce light.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bakeStyles;
    UINT m_bakeStylesView = DescriptorHeap::kInvalid;
    bool m_bakeLogPending = false;
    // The baked light on its way to the cache (BakedBuffers::cachePath).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bakeCacheReadback;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_projectors;
    std::byte* m_projectorsMapped = nullptr;
    std::array<UINT, kSlots> m_projectorViews = {};
    // Per slot: each body of water's last drawn surface (WaterDraw in shaders/water_body.hlsli).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_waterDraws;
    std::byte* m_waterDrawsMapped = nullptr;
    std::array<UINT, kSlots> m_waterDrawViews = {};
    bool m_loggedWaterDraw = false;
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
    // The sky's light for indirect rays (shaders/sky.hlsl): the drawn sky averaged to a few texels a face, as an array
    // (uav) and a cube (srv); ready once a main view has made it.
    Texture m_skyLight;
    bool m_skyLightReady = false;
    // The procedural sky (shaders/sky_atmosphere.hlsl): the atmosphere's transmittance and multiple scattering tables,
    // made once; the clear sky for m_atmosphereSun (faces m_atmosphereSkyFaces); the colour bands taken from the game's
    // sky; and the sky that replaces it (faces m_proceduralSkyFaces), ready once a main view has made it this map.
    Texture m_transmittance;
    Texture m_multipleScattering;
    Texture m_atmosphereSky;
    UINT m_atmosphereSkyFaces = DescriptorHeap::kInvalid;
    Texture m_skyBands;
    Texture m_proceduralSky;
    UINT m_proceduralSkyFaces = DescriptorHeap::kInvalid;
    bool m_atmosphereFailed = false;
    bool m_atmosphereTables = false;
    std::optional<std::array<float, 3>> m_atmosphereSun;
    bool m_proceduralSkyReady = false;
    gbuffer::SkyDraw m_skyDraw;
    // The main view's caustics and the water's sun maps (shaders/caustics.hlsl): the photon counts, how much the water
    // focuses sunlight over the square from m_causticsOrigin, 1 / m_causticsInvSize across, and the sun maps of that
    // square and of the coarse one that covers the map; ready once a main view has traced them this map.
    Texture m_causticCounts;
    Texture m_caustics;
    Texture m_waterSun;
    Texture m_coarseWaterSun;
    bool m_causticsReady = false;
    float m_causticsOrigin[2] = {};
    float m_causticsInvSize = 0.0f;
    float m_coarseCausticsOrigin[2] = {};
    float m_coarseCausticsInvSize = 0.0f;
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
    // How far the 3D skybox's surfaces are from the main view's eye, as the map's space scales them (shaders/primary.hlsl),
    // written by the skybox view of frame m_skyboxDistanceFrame for the main view's motion.
    Texture m_skyboxDistance;
    std::optional<uint64_t> m_skyboxDistanceFrame;
    // Exact direct light of the main view's current and last frame (shaders/direct.hlsl), and its sampled remainder.
    std::array<Texture, 2> m_direct;
    Texture m_directRemainder;
    Texture m_specular;
    Texture m_giRaw;
    std::array<Texture, 2> m_giHistory;
    std::array<Texture, 2> m_giMoments;
    std::array<Texture, 2> m_giFiltered;
    // The denoised light the main view used last frame, which indirect rays reuse; kInvalid when there is none.
    Texture* m_lastDenoised = nullptr;
    // Shaded colour.
    Texture m_lit;
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
    // The volume's indirect light alone, before the fill adds direct light, for forward twins that trace their own;
    // their UAVs are consecutive (probes.hlsl writes output3 + channel).
    std::array<Texture, 3> m_probeIndirect;
    std::array<int, 3> m_probeOrigin = {};
    bool m_probeHistoryValid = false;
    bool m_volumeReady = false;
    float m_litFogScale = 0.0f;
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
    UpscalingMode m_rrSizeMode = UpscalingMode::Count;
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
    // The scene the main view of frame m_mainSceneFrame (run m_mainSceneRun) traced, from slot m_mainSceneSlot, which the
    // viewmodel reuses when it is the next run: no other run has rebuilt that slot since.
    SceneFrame m_mainScene = {};
    uint64_t m_mainSceneFrame = ~0ull;
    uint64_t m_mainSceneRun = ~0ull;
    UINT m_mainSceneSlot = 0;
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
    // GPU time of the 3D skybox's and viewmodel's runs since the last report, and of the 3D skybox's alone.
    double m_otherViewsMsTotal = 0.0;
    double m_skyboxMsTotal = 0.0;
    uint32_t m_timingReports = 0;
    uint32_t m_cameraFailures = 0;
    std::array<bool, kSlots> m_slotWasMain = {};
    std::array<bool, kSlots> m_slotWasSkybox = {};
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
