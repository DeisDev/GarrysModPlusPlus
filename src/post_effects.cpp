#include "post_effects.h"

#include "color.h"
#include "display_output.h"
#include "engine_interfaces.h"
#include "lens_flare.h"
#include "options.h"
#include "log.h"
#include "overlay.h"
#include "settings.h"
#include "shaderapi.h"
#include "shaderapi_device.h"
#include "shaderapi_pipelines.h"
#include "shaderapi_textures.h"
#include "streamline.h"
#include "world.h"

#include <wrl/client.h>

#include <g_bloomDownCS.h>
#include <g_bloomUpCS.h>
#include <g_casCS.h>
#include <g_flareApertureCS.h>
#include <g_flareFftColumnsCS.h>
#include <g_flareFftRowsCS.h>
#include <g_flareGhostPS.h>
#include <g_flareGhostVS.h>
#include <g_flareStarburstCS.h>
#include <g_dof0CS.h>
#include <g_dof0ReverseCS.h>
#include <g_dof1CS.h>
#include <g_dof2CS.h>
#include <g_dof3CS.h>
#include <g_dof4CS.h>
#include <g_lensCS.h>
#include <g_localBlurGridCS.h>
#include <g_localBlurMeanCS.h>
#include <g_localGridCS.h>
#include <g_postCameraCS.h>
#include <g_postExposureCS.h>
#include <g_postExposureHistogramCS.h>
#include <g_postFrameMotionCS.h>
#include <g_postFinalPS.h>
#include <g_postMotionBlurCS.h>
#include <g_postNeighborMaxCS.h>
#include <g_postPrepareCS.h>
#include <g_postTileMaxCS.h>
#include <g_postVS.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp::native {

namespace {

constexpr UINT kInvalid = DescriptorHeap::kInvalid;

// Mirrors PostConstants in shaders/post/post.hlsli, in 16-byte rows.
struct PostConstants {
    std::uint32_t size[2];
    float invSize[2];
    std::int32_t origin[2];
    float invDepthSize[2];
    std::uint32_t depthView;
    std::uint32_t motionView;
    float depthScale;
    float depthBias;
    float viewmodelDepth;
    std::uint32_t reverseDepth;
    std::uint32_t bloomView;
    float frameSeconds;
    std::uint32_t cameraView;
    float cocScale;
    float focalLength;
    float cocLimit;
    std::uint32_t maxRings;
    float manualFocus;
    float focusTime;
    std::uint32_t dofBilat;
    std::uint32_t dofBilatMips[4];
    std::uint32_t dofTileRadiusUav;
    std::uint32_t dofTileRadius;
    std::uint32_t dofDilatedUav;
    std::uint32_t dofDilated;
    std::uint32_t dofNearUav;
    std::uint32_t dofNear;
    std::uint32_t dofFarUav;
    std::uint32_t dofFar;
    std::uint32_t dofGlobals;
    float shutterShare;
    std::uint32_t blurTile;
    std::uint32_t flareView;
    std::uint32_t tileCount[2];
    float flarePosition[2];
    float flareColor[3];
    float flareInFront;
    float flareStarburst;
    std::uint32_t flareVisibilityView;
    float flareRadius;
    float chromaticAberration;
    float distortion;
    float distortionScale;
    float vignette;
    float grainAmount;
    float grainScale;
    std::uint32_t grainSeed;
    float sharpness;
    std::uint32_t gradeEnabled;
    float gradeGain[3];
    float gradeContrast;
    float gradeSaturation;
    float motionScale[2];
    float focalPixels;
    std::uint32_t toneCurve;
    float highlightRolloff;
    float tonePeak;
    std::uint32_t ccCount;
    float ccDefaultWeight;
    std::uint32_t exposureView;
    float brightShare;
    float brightTarget;
    float ccWeights[4];
    std::uint32_t ccViews[4];
    float paperWhite;
    float uiWhite;
    float exposureLimit;
    float gradeExposure;
    float bloomWeights[4];
    float bloomWeightsHigh[2];
    float bloomScatter;
    std::uint32_t localGrid;
    std::uint32_t localBlurred;
    float localHighlights;
    float localShadows;
    float localDetail;
    float localBlend;
    float localMiddleGreyLog;
    float localGridMinLog;
    float localGridInvRangeLog;
    float localBlurSigma;
    std::uint32_t flareGhosts;
    std::uint32_t flareGhostCount;
    std::uint32_t flareAperture;
    std::uint32_t flareStarburstView;
    float flareStarburstSize;
    std::uint32_t padding8[2];
};
static_assert(sizeof(PostConstants) == 28 * 16);

// Mirrors PostPass in shaders/post/post.hlsli.
struct PostPass {
    UINT input0 = kInvalid;
    UINT input1 = kInvalid;
    UINT input2 = kInvalid;
    UINT output0 = kInvalid;
    UINT output1 = kInvalid;
    UINT output2 = kInvalid;
    UINT step = 0;
    UINT padding = 0;
};
static_assert(sizeof(PostPass) == 8 * sizeof(UINT));

enum class Compute { Prepare, Camera, TileMax, NeighborMax, MotionBlur, DofTiles, DofTilesReverse, DofColor, DofDilate, DofBlur, DofComposite, Cas, Lens,
    FrameMotion, ExposureHistogram, Exposure, BloomDown, BloomUp, LocalGrid, LocalBlurGrid, LocalBlurMean, FlareAperture, FlareFftRows, FlareFftColumns,
    FlareStarburst, Count };

constexpr std::span<const unsigned char> kComputeShaders[] = {g_postPrepareCS, g_postCameraCS, g_postTileMaxCS, g_postNeighborMaxCS, g_postMotionBlurCS,
    g_dof0CS, g_dof0ReverseCS, g_dof1CS, g_dof2CS, g_dof3CS, g_dof4CS, g_casCS, g_lensCS, g_postFrameMotionCS, g_postExposureHistogramCS, g_postExposureCS,
    g_bloomDownCS, g_bloomUpCS, g_localGridCS, g_localBlurGridCS, g_localBlurMeanCS, g_flareApertureCS, g_flareFftRowsCS, g_flareFftColumnsCS,
    g_flareStarburstCS};
static_assert(std::size(kComputeShaders) == static_cast<size_t>(Compute::Count));

// The HDR exposure's luminance bins (kExposureBins in shaders/post/post.hlsl), and how often (in frames) Source's auto
// exposure target is read again from its console variables.
constexpr UINT kExposureBins = 128;
constexpr unsigned kExposureTargetPollFrames = 30;
// FidelityFX DoF's half-resolution colour has 4 mips (FFX_DOF_INTERNAL_BILAT_MIP_COUNT in ffx_dof_resources.h).
constexpr UINT kDofMips = 4;
// Source's DepthRange(0, 0.1) for the held weapon (IsViewmodelRange in src/native_lighting.cpp).
constexpr float kViewmodelDepth = 0.1f;
// A full-frame sensor's height in millimetres, and Hammer units in millimetres (Valve Developer Community, "Hammer
// units": about an inch, the scale of characters).
constexpr float kFullFrameHeight = 24.0f;
constexpr float kUnitMillimetres = 25.4f;
// Film runs at 24 frames a second; its grain changes with each frame.
constexpr double kGrainRate = 24.0;
// GPU timestamps: one before the passes and one after each (PostGpuPass), in a ring of slots each read back once its
// frame has completed, averaged over kTimingSamples frames as the lighting's are.
constexpr UINT kPostPasses = static_cast<UINT>(PostGpuPass::Count);
constexpr UINT kTimingMarkers = kPostPasses + 1;
constexpr UINT kTimingSlots = Backend::kFrames + 2;
constexpr UINT kTimingSamples = 120;
// The bloom's levels below the view, each half the size of the one above (shaders/post/bloom.hlsl), the share of the
// light it scatters per unit of the game's bloom scale (veiling glare is a few percent of the light) and the most it
// may take.
constexpr size_t kBloomLevels = 6;
constexpr float kBloomScatterPerScale = 0.05f;
constexpr float kBloomScatterLimit = 0.5f;
// Local exposure's cells in pixels and luminance bins (shaders/post/local_exposure.hlsl), and its grid's range around
// middle grey in stops.
constexpr UINT kLocalCell = 32;
constexpr UINT kLocalBins = 32;
constexpr float kLocalStopsBelow = 12.0f;
constexpr float kLocalStopsAbove = 8.0f;
constexpr float kMiddleGrey = 0.18f;
// The lens flare's aperture and diffraction textures' size (shaders/post/flare.hlsl, a 256-point transform) and the
// most ghosts it draws (every pair of the lens's 10 surfaces is 45).
constexpr UINT kApertureSize = 256;
constexpr UINT kMaxGhosts = 64;
constexpr float kPi = 3.14159265f;
// The lens effects' field of view without the lighting's camera: GMod's default (fov_desired 75, horizontal at 4:3).
constexpr float kDefaultTanHalfHeight = 0.5754f;
// FidelityFX Lens's dispersion strength per unit of the chromatic aberration setting: red is magnified 0.25% less than
// blue at tan^2 of the field angle 1 (FfxLensGetRGMag: 1 - redMag is about 0.0174 per unit of strength).
constexpr float kDispersionPerUnit = 0.144f;

struct Target {
    TrackedResource texture;
    UINT srv = kInvalid;
    UINT uav = kInvalid;
};

struct State {
    bool failed = false;
    bool loggedTarget = false;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, static_cast<size_t>(Compute::Count)> pipelines;
    ComPtr<ID3D12PipelineState> final;
    // The view size the targets were made for.
    UINT width = 0;
    UINT height = 0;
    std::array<Target, 2> scene;
    Target bilateral;
    std::array<UINT, kDofMips> bilateralMips = {kInvalid, kInvalid, kInvalid, kInvalid};
    // The bloom's down and up chains, made when it first runs at this size, in smallHdrFormat (R11G11B10_FLOAT, or
    // RGBA16F where the device cannot store or draw it).
    std::array<Target, kBloomLevels> bloomDown;
    std::array<Target, kBloomLevels> bloomUp;
    DXGI_FORMAT smallHdrFormat = DXGI_FORMAT_UNKNOWN;
    // Around DoEnginePostProcessing (BeginEnginePost): whether our bloom replaces the game's in this view, and the bloom
    // scale the game drew its blur with, if it drew it.
    bool replacingBloom = false;
    bool bloomReplaced = false;
    bool bloomRecorded = false;
    float bloomScale = 0.0f;
    // The lens flare: the aperture with lensFlareBlades blades and its diffraction (each row's transform, the power
    // spectrum, the energies and the starburst), made again when the blade count changes; the ghosts uploaded each frame;
    // the flare drawn at half the view's size with its pipeline.
    Target aperture;
    Target fftRows;
    Target fftPower;
    Target fftEnergy;
    Target starburst;
    int apertureBlades = 0;
    Target ghosts;
    Target flare;
    D3D12_CPU_DESCRIPTOR_HANDLE flareTarget = {};
    ComPtr<ID3D12PipelineState> ghostPipeline;
    bool loggedLens = false;
    // Local exposure: the bilateral grid and its blur, the cells' mean luminance, blurred along x, then y.
    Target localGrid;
    Target localGridBlurred;
    Target localMean;
    Target localMeanTemp;
    Target localMeanBlurred;
    Target tileRadius;
    Target dilated;
    Target nearField;
    Target farField;
    Target globals;
    Target tileMax;
    Target neighborMax;
    UINT blurTile = 0;
    // Frame generation's inputs: the main view's motion and depth at its resolution, and the view before the HUD as the
    // swap chain shows it (display::Map), in its format.
    Target frameMotion;
    Target frameDepth;
    TrackedResource hudless;
    D3D12_CPU_DESCRIPTOR_HANDLE hudlessTarget = {};
    UINT hudlessView = kInvalid;
    UINT hudlessSize[2] = {};
    DXGI_FORMAT hudlessFormat = DXGI_FORMAT_UNKNOWN;
    // DLSS Frame Generation's UI alpha, drawn by the display mapping at the present of frames whose view without the HUD
    // was tagged (uiAlphaWanted).
    TrackedResource uiAlpha;
    D3D12_CPU_DESCRIPTOR_HANDLE uiAlphaTarget = {};
    UINT uiAlphaSize[2] = {};
    bool uiAlphaWanted = false;
    // The camera's state across frames (CameraMain in shaders/post/post.hlsl).
    Target camera;
    // The HDR brightness boost (ExposureHistogramMain and ExposureMain): the view's luminance bins, its gain across frames,
    // whether the last frame with a 3D view took it, and Source's auto exposure target it is measured against
    // (UpdateBrightTarget).
    Target exposureBins;
    Target exposure;
    bool exposureRan = false;
    unsigned exposurePolls = 0;
    float brightShare = 0.0f;
    float brightTarget = 0.0f;
    bool loggedBrightTarget = false;
    ID3D12Resource* depthResource = nullptr;
    UINT depthView = kInvalid;
    std::chrono::steady_clock::time_point last;
    double seconds = 0.0;
    // GPU timing: the timestamps, their mapped read-back, each slot's fence (0 while it holds nothing to read), the
    // next slot, and the sums of the frames read since the last average.
    bool timersFailed = false;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestampReadback;
    const std::uint64_t* timestampValues = nullptr;
    double timestampPeriodMs = 0.0;
    std::array<std::uint64_t, kTimingSlots> slotFence = {};
    UINT timingSlot = 0;
    std::array<double, kPostPasses> passMsTotal = {};
    UINT timingSamples = 0;
};

State g_state;

float Luminance(const std::array<float, 3>& color) { return 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2]; }

// The per-channel gain of white balance: warmth recolours the view as if lit by a black body of 6500 K x 2^-warmth
// rather than 6500 K (the sRGB white), tint shifts it between green and magenta; neither changes its luminance.
std::array<float, 3> GradeGain(const Settings& settings)
{
    constexpr float kNeutralKelvin = 6500.0f;
    constexpr float kTintStrength = 0.2f;
    const std::array<float, 3> neutral = BlackbodyColor(kNeutralKelvin);
    const std::array<float, 3> light = BlackbodyColor(kNeutralKelvin * std::exp2(-settings.warmth));
    std::array<float, 3> balance;
    for (size_t i = 0; i < 3; ++i)
        balance[i] = light[i] / std::max(neutral[i], 1e-6f);
    const float tint = settings.tint * kTintStrength;
    const std::array<float, 3> shift = {1.0f + tint, 1.0f - tint, 1.0f + tint};
    for (size_t i = 0; i < 3; ++i)
        balance[i] *= shift[i];
    const float scale = 1.0f / std::max(Luminance(balance), 1e-6f);
    for (float& gain : balance)
        gain *= scale;
    return balance;
}

bool GradingChanges(const Settings& settings)
{
    return settings.grading && (settings.exposure != 0.0f || settings.contrast != 1.0f || settings.saturation != 1.0f || settings.warmth != 0.0f ||
        settings.tint != 0.0f);
}

void Release(Target& target)
{
    Backend& backend = GetBackend();
    if (target.texture.resource)
        backend.Retire(std::move(target.texture.resource));
    if (target.srv != kInvalid)
        backend.RetireDescriptor(target.srv);
    if (target.uav != kInvalid)
        backend.RetireDescriptor(target.uav);
    target = {};
}

bool CreateTexture(Target& target, UINT width, UINT height, DXGI_FORMAT format, UINT mips = 1)
{
    Backend& backend = GetBackend();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = std::max(width, 1u);
    desc.Height = std::max(height, 1u);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(mips);
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    const HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&target.texture.resource));
    if (FAILED(hr)) {
        log::Info("Post effects: creating a {}x{} target failed: {:#010x}", width, height, static_cast<unsigned>(hr));
        return false;
    }
    target.texture.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    target.srv = backend.Descriptors().Allocate();
    target.uav = backend.Descriptors().Allocate();
    if (target.srv == kInvalid || target.uav == kInvalid)
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = mips;
    backend.Device()->CreateShaderResourceView(target.texture.resource.Get(), &srv, backend.Descriptors().Cpu(target.srv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    backend.Device()->CreateUnorderedAccessView(target.texture.resource.Get(), nullptr, &uav, backend.Descriptors().Cpu(target.uav));
    return true;
}

// A 3D texture with an unordered access view and a shader resource view.
bool CreateVolume(Target& target, UINT width, UINT height, UINT depth, DXGI_FORMAT format)
{
    Backend& backend = GetBackend();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width = std::max(width, 1u);
    desc.Height = std::max(height, 1u);
    desc.DepthOrArraySize = static_cast<UINT16>(std::max(depth, 1u));
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    const HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&target.texture.resource));
    if (FAILED(hr)) {
        log::Info("Post effects: creating a {}x{}x{} volume failed: {:#010x}", width, height, depth, static_cast<unsigned>(hr));
        return false;
    }
    target.texture.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    target.srv = backend.Descriptors().Allocate();
    target.uav = backend.Descriptors().Allocate();
    if (target.srv == kInvalid || target.uav == kInvalid)
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture3D.MipLevels = 1;
    backend.Device()->CreateShaderResourceView(target.texture.resource.Get(), &srv, backend.Descriptors().Cpu(target.srv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uav.Texture3D.WSize = static_cast<UINT>(-1);
    backend.Device()->CreateUnorderedAccessView(target.texture.resource.Get(), nullptr, &uav, backend.Descriptors().Cpu(target.uav));
    return true;
}

// The format of the bloom's and the flare's light: R11G11B10_FLOAT (positive, up to 65000, which the sun's disc at 1024
// paper whites fits) where the device can store it from compute shaders, draw into and sample it, else RGBA16F.
DXGI_FORMAT SmallHdrFormat()
{
    if (g_state.smallHdrFormat == DXGI_FORMAT_UNKNOWN) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {DXGI_FORMAT_R11G11B10_FLOAT};
        const bool supported = SUCCEEDED(GetBackend().Device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
            (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0 && (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0 &&
            (support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) != 0 && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
        g_state.smallHdrFormat = supported ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!supported)
            log::Info("Post effects: R11G11B10_FLOAT cannot be stored or drawn into; the bloom and the lens flare use RGBA16F");
    }
    return g_state.smallHdrFormat;
}

// A structured buffer of `count` elements of `stride` bytes, zeroed as committed resources are, with an unordered
// access view.
bool CreateBuffer(Target& target, UINT stride, UINT count)
{
    Backend& backend = GetBackend();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(stride) * count;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    const HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&target.texture.resource));
    if (FAILED(hr)) {
        log::Info("Post effects: creating a buffer failed: {:#010x}", static_cast<unsigned>(hr));
        return false;
    }
    target.texture.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    target.uav = backend.Descriptors().Allocate();
    if (target.uav == kInvalid)
        return false;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = count;
    uav.Buffer.StructureByteStride = stride;
    backend.Device()->CreateUnorderedAccessView(target.texture.resource.Get(), nullptr, &uav, backend.Descriptors().Cpu(target.uav));
    return true;
}

void ReleaseTargets()
{
    for (Target& target : g_state.scene)
        Release(target);
    for (Target& target : g_state.bloomDown)
        Release(target);
    for (Target& target : g_state.bloomUp)
        Release(target);
    for (Target* target : {&g_state.localGrid, &g_state.localGridBlurred, &g_state.localMean, &g_state.localMeanTemp, &g_state.localMeanBlurred, &g_state.flare})
        Release(*target);
    for (Target* target : {&g_state.bilateral, &g_state.tileRadius, &g_state.dilated, &g_state.nearField, &g_state.farField, &g_state.globals,
             &g_state.tileMax, &g_state.neighborMax, &g_state.frameMotion, &g_state.frameDepth})
        Release(*target);
    for (UINT& mip : g_state.bilateralMips) {
        if (mip != kInvalid)
            GetBackend().RetireDescriptor(mip);
        mip = kInvalid;
    }
    g_state.width = g_state.height = 0;
}

// The working textures for a view `width` x `height`: the colour twice (each pass reads one and writes the other),
// FidelityFX DoF's at the sizes its SDK makes them (ffx_dof.cpp, ffxDofContextCreate), and motion blur's tiles.
bool EnsureTargets(UINT width, UINT height)
{
    if (g_state.width == width && g_state.height == height)
        return true;
    ReleaseTargets();
    Backend& backend = GetBackend();
    const UINT halfWidth = (width + 1) / 2;
    const UINT halfHeight = (height + 1) / 2;
    const UINT tileWidth = (halfWidth + 3) / 4;
    const UINT tileHeight = (halfHeight + 4) / 4;
    g_state.blurTile = std::clamp((static_cast<UINT>(std::ceil(height * 0.025f)) + 7) / 8 * 8, 8u, 64u);
    const UINT blurTilesX = (width + g_state.blurTile - 1) / g_state.blurTile;
    const UINT blurTilesY = (height + g_state.blurTile - 1) / g_state.blurTile;
    bool ok = CreateTexture(g_state.scene[0], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
        CreateTexture(g_state.scene[1], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
        CreateTexture(g_state.bilateral, halfWidth, halfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, kDofMips) &&
        CreateTexture(g_state.tileRadius, tileWidth, tileHeight, DXGI_FORMAT_R16G16_FLOAT) &&
        CreateTexture(g_state.dilated, tileWidth, tileHeight, DXGI_FORMAT_R16G16_FLOAT) &&
        CreateTexture(g_state.nearField, halfWidth, halfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
        CreateTexture(g_state.farField, halfWidth, halfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT) && CreateBuffer(g_state.globals, 4, 1) &&
        CreateTexture(g_state.tileMax, blurTilesX, blurTilesY, DXGI_FORMAT_R16G16_FLOAT) &&
        CreateTexture(g_state.neighborMax, blurTilesX, blurTilesY, DXGI_FORMAT_R16G16_FLOAT) &&
        CreateTexture(g_state.frameMotion, width, height, DXGI_FORMAT_R16G16_FLOAT) &&
        CreateTexture(g_state.frameDepth, width, height, DXGI_FORMAT_R32_FLOAT);
    for (UINT mip = 0; ok && mip < kDofMips; ++mip) {
        g_state.bilateralMips[mip] = backend.Descriptors().Allocate();
        ok = g_state.bilateralMips[mip] != kInvalid;
        if (!ok)
            break;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Texture2D.MipSlice = mip;
        backend.Device()->CreateUnorderedAccessView(g_state.bilateral.texture.resource.Get(), nullptr, &uav,
            backend.Descriptors().Cpu(g_state.bilateralMips[mip]));
    }
    if (!ok) {
        ReleaseTargets();
        return false;
    }
    g_state.width = width;
    g_state.height = height;
    return true;
}

// The size of the bloom's level `level` for a view `size` across.
UINT BloomSize(UINT size, size_t level) { return std::max(1u, (size + (2u << level) - 1) >> (level + 1)); }

// The bloom's chains, made when it first runs at the targets' size (EnsureTargets drops them with the others).
bool EnsureBloomTargets()
{
    if (g_state.bloomDown[0].texture.resource)
        return true;
    const DXGI_FORMAT format = SmallHdrFormat();
    for (size_t level = 0; level < kBloomLevels; ++level) {
        const UINT width = BloomSize(g_state.width, level);
        const UINT height = BloomSize(g_state.height, level);
        if (!CreateTexture(g_state.bloomDown[level], width, height, format) || !CreateTexture(g_state.bloomUp[level], width, height, format)) {
            for (size_t i = 0; i < kBloomLevels; ++i) {
                Release(g_state.bloomDown[i]);
                Release(g_state.bloomUp[i]);
            }
            return false;
        }
    }
    return true;
}

// The bloom's level weights: falling off by e every lerp(0.8, 3, spread) levels, so a wider spread gives the large
// levels more, and adding up to 1.
std::array<float, kBloomLevels> BloomWeights(float spread)
{
    const float levels = std::lerp(0.8f, 3.0f, std::clamp(spread, 0.0f, 1.0f));
    std::array<float, kBloomLevels> weights;
    float sum = 0.0f;
    for (size_t level = 0; level < kBloomLevels; ++level) {
        weights[level] = std::exp(-static_cast<float>(level) / levels);
        sum += weights[level];
    }
    for (float& weight : weights)
        weight /= sum;
    return weights;
}

// Local exposure's grid and blurs for the targets' size.
bool EnsureLocalTargets()
{
    if (g_state.localGrid.texture.resource)
        return true;
    const UINT cellsX = (g_state.width + kLocalCell - 1) / kLocalCell;
    const UINT cellsY = (g_state.height + kLocalCell - 1) / kLocalCell;
    if (CreateVolume(g_state.localGrid, cellsX, cellsY, kLocalBins, DXGI_FORMAT_R16G16_FLOAT) &&
        CreateVolume(g_state.localGridBlurred, cellsX, cellsY, kLocalBins, DXGI_FORMAT_R16G16_FLOAT) &&
        CreateTexture(g_state.localMean, cellsX, cellsY, DXGI_FORMAT_R16_FLOAT) && CreateTexture(g_state.localMeanTemp, cellsX, cellsY, DXGI_FORMAT_R16_FLOAT) &&
        CreateTexture(g_state.localMeanBlurred, cellsX, cellsY, DXGI_FORMAT_R16_FLOAT))
        return true;
    for (Target* target : {&g_state.localGrid, &g_state.localGridBlurred, &g_state.localMean, &g_state.localMeanTemp, &g_state.localMeanBlurred})
        Release(*target);
    return false;
}

// The flare's target at half the view's size, which the ghosts are drawn into, with a render target view.
bool EnsureFlareTarget()
{
    if (g_state.flare.texture.resource)
        return true;
    Backend& backend = GetBackend();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = BloomSize(g_state.width, 0);
    desc.Height = BloomSize(g_state.height, 0);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = SmallHdrFormat();
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    const HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&g_state.flare.texture.resource));
    if (FAILED(hr)) {
        log::Info("Post effects: creating the lens flare's target failed: {:#010x}", static_cast<unsigned>(hr));
        return false;
    }
    g_state.flare.texture.state = D3D12_RESOURCE_STATE_COMMON;
    if (g_state.flareTarget.ptr == 0)
        g_state.flareTarget = backend.RenderTargetViews().Allocate();
    backend.Device()->CreateRenderTargetView(g_state.flare.texture.resource.Get(), nullptr, g_state.flareTarget);
    g_state.flare.srv = backend.Descriptors().Allocate();
    if (g_state.flare.srv == kInvalid)
        return false;
    backend.Device()->CreateShaderResourceView(g_state.flare.texture.resource.Get(), nullptr, backend.Descriptors().Cpu(g_state.flare.srv));
    return true;
}

// The ghosts' buffer: kMaxGhosts records the CPU uploads each frame, read through a structured shader resource view.
bool EnsureGhostBuffer()
{
    if (g_state.ghosts.texture.resource)
        return true;
    Backend& backend = GetBackend();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(lens_flare::Ghost) * kMaxGhosts;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    const HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&g_state.ghosts.texture.resource));
    if (FAILED(hr)) {
        log::Info("Post effects: creating the lens flare's ghost buffer failed: {:#010x}", static_cast<unsigned>(hr));
        return false;
    }
    g_state.ghosts.texture.state = D3D12_RESOURCE_STATE_COMMON;
    g_state.ghosts.srv = backend.Descriptors().Allocate();
    if (g_state.ghosts.srv == kInvalid)
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = kMaxGhosts;
    srv.Buffer.StructureByteStride = sizeof(lens_flare::Ghost);
    backend.Device()->CreateShaderResourceView(g_state.ghosts.texture.resource.Get(), &srv, backend.Descriptors().Cpu(g_state.ghosts.srv));
    return true;
}

// The aperture's textures and the diffraction's working textures, which do not depend on the view's size.
bool EnsureApertureTextures()
{
    if (g_state.aperture.texture.resource)
        return true;
    if (CreateTexture(g_state.aperture, kApertureSize, kApertureSize, DXGI_FORMAT_R8_UNORM) &&
        CreateTexture(g_state.fftRows, kApertureSize, kApertureSize, DXGI_FORMAT_R32G32_FLOAT) &&
        CreateTexture(g_state.fftPower, kApertureSize, kApertureSize, DXGI_FORMAT_R32_FLOAT) && CreateBuffer(g_state.fftEnergy, 4, kApertureSize + 1) &&
        CreateTexture(g_state.starburst, kApertureSize, kApertureSize, DXGI_FORMAT_R16G16B16A16_FLOAT))
        return true;
    for (Target* target : {&g_state.aperture, &g_state.fftRows, &g_state.fftPower, &g_state.fftEnergy, &g_state.starburst})
        Release(*target);
    return false;
}

// Frame generation's view without the HUD, in the swap chain's format, made again when the frame's size or the format
// changes.
bool EnsureHudless(UINT width, UINT height, DXGI_FORMAT format)
{
    if (g_state.hudless.resource && g_state.hudlessSize[0] == width && g_state.hudlessSize[1] == height && g_state.hudlessFormat == format)
        return true;
    Backend& backend = GetBackend();
    if (g_state.hudless.resource)
        backend.Retire(std::move(g_state.hudless.resource));
    g_state.hudless = {};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    const HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&g_state.hudless.resource));
    if (FAILED(hr)) {
        log::Info("Post effects: creating frame generation's copy of the view failed: {:#010x}", static_cast<unsigned>(hr));
        return false;
    }
    g_state.hudless.state = D3D12_RESOURCE_STATE_COMMON;
    if (g_state.hudlessTarget.ptr == 0)
        g_state.hudlessTarget = backend.RenderTargetViews().Allocate();
    backend.Device()->CreateRenderTargetView(g_state.hudless.resource.Get(), nullptr, g_state.hudlessTarget);
    if (g_state.hudlessView == kInvalid)
        g_state.hudlessView = backend.Descriptors().Allocate();
    backend.Device()->CreateShaderResourceView(g_state.hudless.resource.Get(), nullptr, backend.Descriptors().Cpu(g_state.hudlessView));
    g_state.hudlessSize[0] = width;
    g_state.hudlessSize[1] = height;
    g_state.hudlessFormat = format;
    return true;
}

// The depth buffer's shader resource view, made again when the buffer is.
bool EnsureDepthView()
{
    Backend& backend = GetBackend();
    ID3D12Resource* depth = backend.DepthBuffer().resource.Get();
    if (depth == nullptr)
        return false;
    if (depth == g_state.depthResource)
        return true;
    if (g_state.depthView == kInvalid)
        g_state.depthView = backend.Descriptors().Allocate();
    if (g_state.depthView == kInvalid)
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    backend.Device()->CreateShaderResourceView(depth, &srv, backend.Descriptors().Cpu(g_state.depthView));
    g_state.depthResource = depth;
    return true;
}

bool EnsureRoot()
{
    if (g_state.root)
        return true;
    // Every pass embeds the same root signature (POST_ROOT_SIGNATURE in shaders/post/post.hlsli).
    const HRESULT hr = GetBackend().Device()->CreateRootSignature(0, g_postPrepareCS, sizeof(g_postPrepareCS), IID_PPV_ARGS(&g_state.root));
    if (FAILED(hr)) {
        log::Info("Post effects: creating the root signature failed: {:#010x}", static_cast<unsigned>(hr));
        return false;
    }
    return GetBackend().Device() != nullptr && (g_state.camera.texture.resource || CreateBuffer(g_state.camera, 16, 2)) &&
        (g_state.exposureBins.texture.resource || CreateBuffer(g_state.exposureBins, 4, kExposureBins)) &&
        (g_state.exposure.texture.resource || CreateBuffer(g_state.exposure, 16, 1));
}

bool EnsureTimers()
{
    if (g_state.timestampValues != nullptr)
        return true;
    if (g_state.timersFailed)
        return false;
    g_state.timersFailed = true;
    Backend& backend = GetBackend();
    const D3D12_QUERY_HEAP_DESC heapDesc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTimingSlots * kTimingMarkers};
    HRESULT hr = backend.Device()->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&g_state.timestamps));
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeof(std::uint64_t) * kTimingSlots * kTimingMarkers;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
        hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&g_state.timestampReadback));
    }
    void* mapped = nullptr;
    if (SUCCEEDED(hr))
        hr = g_state.timestampReadback->Map(0, nullptr, &mapped);
    UINT64 frequency = 0;
    if (SUCCEEDED(hr))
        hr = backend.Queue()->GetTimestampFrequency(&frequency);
    if (FAILED(hr) || frequency == 0) {
        log::Info("Post effects: GPU timing is unavailable: {:#010x}", static_cast<unsigned>(hr));
        return false;
    }
    g_state.timestampPeriodMs = 1000.0 / static_cast<double>(frequency);
    g_state.timestampValues = static_cast<const std::uint64_t*>(mapped);
    g_state.timersFailed = false;
    return true;
}

// Adds the times of every slot whose frame has completed, and every kTimingSamples frames publishes their average.
void ReadTimings()
{
    const std::uint64_t completed = GetBackend().CompletedFence();
    for (UINT slot = 0; slot < kTimingSlots; ++slot) {
        if (g_state.slotFence[slot] == 0 || completed < g_state.slotFence[slot])
            continue;
        g_state.slotFence[slot] = 0;
        const std::uint64_t* markers = g_state.timestampValues + slot * kTimingMarkers;
        if (markers[kPostPasses] <= markers[0])
            continue;
        for (UINT pass = 0; pass < kPostPasses; ++pass)
            g_state.passMsTotal[pass] += static_cast<double>(markers[pass + 1] - markers[pass]) * g_state.timestampPeriodMs;
        if (++g_state.timingSamples < kTimingSamples)
            continue;
        Statistics& statistics = GetStatistics();
        statistics.postMs = 0.0;
        for (UINT pass = 0; pass < kPostPasses; ++pass) {
            statistics.postPassMs[pass] = g_state.passMsTotal[pass] / g_state.timingSamples;
            statistics.postMs += statistics.postPassMs[pass];
        }
        g_state.passMsTotal = {};
        g_state.timingSamples = 0;
    }
}

std::optional<float> ConVarShare(const char* name)
{
    const std::optional<std::string> value = engine::ConVarString(name);
    if (!value)
        return std::nullopt;
    char* end = nullptr;
    const float percent = std::strtof(value->c_str(), &end);
    if (end == value->c_str() || !std::isfinite(percent))
        return std::nullopt;
    return percent / 100.0f;
}

// Source's auto exposure target (the SDK's viewpostprocess.cpp, mat_tonemap_algorithm 1): its brightest
// mat_tonemap_percent_bright_pixels percent of pixels start at mat_tonemap_percent_target percent of white. False while
// those console variables cannot be read, which leaves HDR without its brightness boost.
bool UpdateBrightTarget()
{
    if (g_state.exposurePolls++ % kExposureTargetPollFrames != 0)
        return g_state.brightTarget > 0.0f;
    const std::optional<float> share = ConVarShare("mat_tonemap_percent_bright_pixels");
    const std::optional<float> target = ConVarShare("mat_tonemap_percent_target");
    if (!share || !target || *target <= 0.0f) {
        if (!std::exchange(g_state.loggedBrightTarget, true))
            log::Info("Post effects: Source's auto exposure target (mat_tonemap_percent_target, mat_tonemap_percent_bright_pixels) cannot be read; "
                      "the HDR brightness boost is off");
        g_state.brightTarget = 0.0f;
        return false;
    }
    g_state.brightShare = std::clamp(*share, 0.0f, 1.0f);
    g_state.brightTarget = *target;
    return true;
}

// The pipeline states are made when an effect first needs them.
ID3D12PipelineState* Pipeline(Compute pass)
{
    ComPtr<ID3D12PipelineState>& pipeline = g_state.pipelines[static_cast<size_t>(pass)];
    if (!pipeline) {
        const std::span<const unsigned char> shader = kComputeShaders[static_cast<size_t>(pass)];
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = g_state.root.Get();
        desc.CS = {shader.data(), shader.size()};
        const HRESULT hr = GetBackend().Device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr)) {
            log::Info("Post effects: creating pass {} failed: {:#010x}", static_cast<int>(pass), static_cast<unsigned>(hr));
            g_state.failed = true;
            return nullptr;
        }
    }
    return pipeline.Get();
}

ID3D12PipelineState* FinalPipeline()
{
    if (!g_state.final) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = g_state.root.Get();
        desc.VS = {g_postVS, sizeof(g_postVS)};
        desc.PS = {g_postFinalPS, sizeof(g_postFinalPS)};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = Backend::kFrameFormat;
        desc.SampleDesc.Count = 1;
        const HRESULT hr = GetBackend().Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_state.final));
        if (FAILED(hr)) {
            log::Info("Post effects: creating the final pass failed: {:#010x}", static_cast<unsigned>(hr));
            g_state.failed = true;
            return nullptr;
        }
    }
    return g_state.final.Get();
}

// The ghosts' and starburst's pipeline: quads added into the flare's target.
ID3D12PipelineState* GhostPipeline()
{
    if (!g_state.ghostPipeline) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = g_state.root.Get();
        desc.VS = {g_flareGhostVS, sizeof(g_flareGhostVS)};
        desc.PS = {g_flareGhostPS, sizeof(g_flareGhostPS)};
        D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_ONE;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = SmallHdrFormat();
        desc.SampleDesc.Count = 1;
        const HRESULT hr = GetBackend().Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_state.ghostPipeline));
        if (FAILED(hr)) {
            log::Info("Post effects: creating the lens flare's pipeline failed: {:#010x}", static_cast<unsigned>(hr));
            g_state.failed = true;
            return nullptr;
        }
    }
    return g_state.ghostPipeline.Get();
}

void Use(Target& target, D3D12_RESOURCE_STATES state) { GetBackend().Transition(target.texture, state); }

void UavBarrier(Target& target)
{
    const D3D12_RESOURCE_BARRIER barrier = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {target.texture.resource.Get()}}};
    GetBackend().Commands()->ResourceBarrier(1, &barrier);
}

// Records a compute pass over `x` x `y` x `z` groups; false when its pipeline cannot be made.
bool Dispatch(Compute pass, const PostPass& args, UINT x, UINT y, UINT z = 1)
{
    ID3D12PipelineState* pipeline = Pipeline(pass);
    if (pipeline == nullptr)
        return false;
    ID3D12GraphicsCommandList* commands = GetBackend().Commands();
    commands->SetPipelineState(pipeline);
    commands->SetComputeRoot32BitConstants(1, sizeof(PostPass) / sizeof(UINT), &args, 0);
    commands->Dispatch(x, y, z);
    return true;
}

UINT Groups(UINT size, UINT group) { return (size + group - 1) / group; }

// The camera of the main view, from Source's view-projection rows (clip = dot(row, position)): the w row is the view's
// forward axis, the z row maps view depth z to device depth a + b / z, and the y row's part across the forward axis is
// the projection's vertical scale, 1 / tan(half the vertical field of view).
struct Camera {
    float depthScale;
    float depthBias;
    float tanHalfHeight;
    bool reverse;
};

std::optional<Camera> CameraOf(const gbuffer::Matrix4& viewProj)
{
    const auto dot3 = [](const auto& a, const auto& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto& y = viewProj[1];
    const auto& z = viewProj[2];
    const auto& w = viewProj[3];
    const float length = std::sqrt(dot3(w, w));
    if (length < 1e-6f)
        return std::nullopt;
    const float a = dot3(z, w) / (length * length);
    const float b = z[3] - a * w[3];
    if (std::fabs(b) < 1e-12f)
        return std::nullopt;
    const float along = dot3(y, w) / length;
    const float across = std::sqrt(std::max(dot3(y, y) - along * along, 0.0f));
    if (across < 1e-6f)
        return std::nullopt;
    // 1 / view depth = length * (depth - a) / b, as w / length is the view depth.
    return Camera{length / b, -a * length / b, 1.0f / across, b > 0.0f};
}

}

void BeginEnginePost(bool postVgui)
{
    g_state.bloomRecorded = false;
    g_state.bloomScale = 0.0f;
    g_state.replacingBloom = GetSettings().bloom && !postVgui && !g_state.failed && GetBackend().HasDevice() && Dynamic().colorTargets[0] == kDefaultTarget;
    g_state.bloomReplaced = g_state.replacingBloom;
}

void EndEnginePost() { g_state.replacingBloom = false; }

bool ReplacingBloom() { return g_state.replacingBloom; }

void RecordBloomScale(float scale)
{
    g_state.bloomScale = std::isfinite(scale) ? std::max(scale, 0.0f) : 0.0f;
    g_state.bloomRecorded = true;
}

void PostProcessView(D3D12_RECT rect, bool postVgui)
{
    const Settings& settings = GetSettings();
    // Our bloom scatters the share of light the game's bloom scale asks for; without the game's chain (bloom off, a map
    // without HDR) there is none.
    const bool bloomReplaced = std::exchange(g_state.bloomReplaced, false);
    const float gameBloomScale = std::exchange(g_state.bloomRecorded, false) ? g_state.bloomScale : 0.0f;
    if (bloomReplaced)
        GetStatistics().gameBloomScale = gameBloomScale;
    // The second pass some games run over the menus (the SDK's bPostVGui) sees a view that is tone mapped already.
    if (g_state.failed || postVgui)
        return;
    const std::optional<PostView> post = world::CurrentPostView();
    std::optional<Camera> camera;
    if (post)
        camera = CameraOf(post->viewProj);
    const bool depthOfField = settings.depthOfField && camera;
    const bool motionBlur = settings.motionBlur && camera && post->motionView != kInvalid;
    const bool flare = settings.lensFlare && settings.lensFlareIntensity > 0.0f && camera && post->light && post->flareVisibilityView != kInvalid;
    const float aberration = settings.chromaticAberration ? settings.chromaticAberrationStrength : 0.0f;
    const float distortion = settings.lensDistortion ? settings.lensDistortionAmount : 0.0f;
    const float vignette = settings.vignette ? settings.vignetteAmount : 0.0f;
    const float bloomScatter = bloomReplaced ? std::clamp(kBloomScatterPerScale * gameBloomScale * settings.bloomStrength, 0.0f, kBloomScatterLimit) : 0.0f;
    // Local exposure with every scale at 1 leaves the view as it is.
    const bool localExposure = settings.localExposure &&
        (settings.localExposureHighlights != 1.0f || settings.localExposureShadows != 1.0f || settings.localExposureDetail != 1.0f);
    const bool grain = settings.filmGrain && settings.grainAmount > 0.0f;
    const bool grade = GradingChanges(settings);
    // Frame generation takes the main view's depth, motion and the view without the HUD from here.
    FsrFrameGeneration* fsrGeneration = GetBackend().FsrFrameGenerator();
    // Neither generator runs while the menu is open: it moves without motion vectors (FSR turns off on frames without inputs).
    const bool frameGeneration = camera && post->motionView != kInvalid && !overlay::Open() && (streamline::FrameGenerationWanted() || fsrGeneration != nullptr);
    // The last pass always runs: it fits the 3D view to the display (the tone curve, colour correction) before the HUD.
    if (Dynamic().colorTargets[0] != kDefaultTarget) {
        if (!std::exchange(g_state.loggedTarget, true))
            log::Info("Post effects: skipped a view drawn outside the back buffer");
        return;
    }
    Backend& backend = GetBackend();
    TrackedResource& backBuffer = backend.FrameBuffer();
    const D3D12_RESOURCE_DESC desc = backBuffer.resource->GetDesc();
    rect.left = std::max(rect.left, LONG(0));
    rect.top = std::max(rect.top, LONG(0));
    rect.right = std::min(rect.right, static_cast<LONG>(desc.Width));
    rect.bottom = std::min(rect.bottom, static_cast<LONG>(desc.Height));
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return;
    const UINT width = static_cast<UINT>(rect.right - rect.left);
    const UINT height = static_cast<UINT>(rect.bottom - rect.top);
    if (!EnsureRoot() || !EnsureTargets(width, height) || !EnsureDepthView())
        return;
    const bool bloom = bloomScatter > 0.0f && EnsureBloomTargets();
    const bool local = localExposure && EnsureLocalTargets();
    const bool lens = aberration > 0.0f || distortion != 0.0f || vignette > 0.0f || flare || bloom;

    const auto now = std::chrono::steady_clock::now();
    const double frameSeconds = g_state.last.time_since_epoch().count() != 0 ? std::chrono::duration<double>(now - g_state.last).count() : 1.0 / 60.0;
    g_state.last = now;
    g_state.seconds += frameSeconds;

    PostConstants c = {};
    c.size[0] = width;
    c.size[1] = height;
    c.invSize[0] = 1.0f / static_cast<float>(width);
    c.invSize[1] = 1.0f / static_cast<float>(height);
    c.origin[0] = rect.left;
    c.origin[1] = rect.top;
    const D3D12_RESOURCE_DESC depthDesc = backend.DepthBuffer().resource->GetDesc();
    c.invDepthSize[0] = 1.0f / static_cast<float>(depthDesc.Width);
    c.invDepthSize[1] = 1.0f / static_cast<float>(depthDesc.Height);
    c.depthView = g_state.depthView;
    c.motionView = post ? post->motionView : kInvalid;
    c.motionScale[0] = post ? post->motionScale[0] : 1.0f;
    c.motionScale[1] = post ? post->motionScale[1] : 1.0f;
    c.frameSeconds = static_cast<float>(std::clamp(frameSeconds, 1e-3, 0.25));
    c.cameraView = g_state.camera.uav;
    if (camera) {
        c.depthScale = camera->depthScale;
        c.depthBias = camera->depthBias;
        c.reverseDepth = camera->reverse ? 1u : 0u;
        // Source draws the held weapon in the first tenth of the depth range; where reversed depth would put it is not
        // known, so then nothing counts as the weapon.
        c.viewmodelDepth = camera->reverse ? 2.0f : kViewmodelDepth;
        // A thin lens on a sensor dofSensorSize times a full-frame one's height, whose focal length the field of view
        // sets: the circle of confusion's radius in half-resolution pixels (FidelityFX DoF's unit).
        const float sensor = kFullFrameHeight * settings.dofSensorSize;
        c.focalLength = 0.5f * sensor / camera->tanHalfHeight / kUnitMillimetres;
        c.cocScale = static_cast<float>(height) / (8.0f * settings.dofAperture * camera->tanHalfHeight);
        c.cocLimit = 0.5f * settings.dofMaxBlur * static_cast<float>(height);
        c.maxRings = static_cast<std::uint32_t>(settings.dofRings);
        c.manualFocus = settings.dofFocus == DofFocus::Manual ? settings.dofFocusDistance : 0.0f;
        c.focusTime = settings.dofFocusTime;
    } else {
        c.viewmodelDepth = kViewmodelDepth;
    }
    c.dofBilat = g_state.bilateral.srv;
    std::copy(g_state.bilateralMips.begin(), g_state.bilateralMips.end(), c.dofBilatMips);
    c.dofTileRadiusUav = g_state.tileRadius.uav;
    c.dofTileRadius = g_state.tileRadius.srv;
    c.dofDilatedUav = g_state.dilated.uav;
    c.dofDilated = g_state.dilated.srv;
    c.dofNearUav = g_state.nearField.uav;
    c.dofNear = g_state.nearField.srv;
    c.dofFarUav = g_state.farField.uav;
    c.dofFar = g_state.farField.srv;
    c.dofGlobals = g_state.globals.uav;
    // The shutter is open 1 / motionBlurShutter seconds of a frame's frameSeconds; motion vectors span a frame.
    c.shutterShare = static_cast<float>(std::min(1.0 / (settings.motionBlurShutter * std::max(frameSeconds, 1e-3)), 4.0));
    c.blurTile = g_state.blurTile;
    c.tileCount[0] = Groups(width, g_state.blurTile);
    c.tileCount[1] = Groups(height, g_state.blurTile);
    c.flareVisibilityView = kInvalid;
    if (flare) {
        // The light's direction at infinity through the projection, in the view's pixels.
        const gbuffer::Matrix4& m = post->viewProj;
        const std::array<float, 3>& d = post->lightDirection;
        float clip[4];
        for (size_t row = 0; row < 4; ++row)
            clip[row] = m[row][0] * d[0] + m[row][1] * d[1] + m[row][2] * d[2];
        if (clip[3] > 0.0f) {
            c.flareInFront = 1.0f;
            c.flarePosition[0] = (clip[0] / clip[3] * 0.5f + 0.5f) * static_cast<float>(width);
            c.flarePosition[1] = (0.5f - clip[1] / clip[3] * 0.5f) * static_cast<float>(height);
            c.flareRadius = std::tan(post->lightRadius) / camera->tanHalfHeight * 0.5f * static_cast<float>(height);
            for (size_t i = 0; i < 3; ++i)
                c.flareColor[i] = post->lightColor[i] * settings.lensFlareIntensity;
            c.flareVisibilityView = post->flareVisibilityView;
        }
    }
    c.chromaticAberration = aberration * kDispersionPerUnit;
    c.distortion = distortion;
    c.focalPixels = 0.5f * static_cast<float>(height) / (camera ? camera->tanHalfHeight : kDefaultTanHalfHeight);
    // Pincushion distortion samples beyond the view's corners; the image is scaled so they stay inside.
    const float cornerSquared = (0.25f * static_cast<float>(width) * static_cast<float>(width) + 0.25f * static_cast<float>(height) * static_cast<float>(height)) /
        (c.focalPixels * c.focalPixels);
    c.distortionScale = distortion > 0.0f ? 1.0f / (1.0f + distortion * cornerSquared) : 1.0f;
    c.vignette = vignette;
    c.grainAmount = grain ? settings.grainAmount : 0.0f;
    c.grainScale = settings.grainSize * static_cast<float>(height) / 1080.0f;
    c.grainSeed = static_cast<std::uint32_t>(g_state.seconds * kGrainRate);
    c.sharpness = settings.contrastSharpness;
    c.toneCurve = static_cast<std::uint32_t>(settings.toneCurve);
    c.highlightRolloff = settings.highlightRolloff;
    const display::Mapping output = backend.HudlessMapping();
    c.tonePeak = std::max(output.peak, 1.0f);
    const bool hdrExposure = backend.OutputHdr() && settings.hdrBrightnessBoost > 0.0f && UpdateBrightTarget();
    c.exposureView = hdrExposure ? g_state.exposure.uav : kInvalid;
    c.brightShare = g_state.brightShare;
    c.brightTarget = g_state.brightTarget;
    c.exposureLimit = std::exp2(settings.hdrBrightnessBoost);
    if (backend.OutputHdr()) {
        c.paperWhite = output.paperWhite;
        c.uiWhite = output.uiWhite;
    }
    {
        std::array<UINT, 4> views = {kInvalid, kInvalid, kInvalid, kInvalid};
        std::array<float, 4> weights = {};
        c.ccCount = display::ColorCorrectionViews(views, weights, c.ccDefaultWeight);
        std::copy(views.begin(), views.end(), c.ccViews);
        std::copy(weights.begin(), weights.end(), c.ccWeights);
    }
    c.bloomView = bloom ? g_state.bloomUp[0].srv : kInvalid;
    c.bloomScatter = bloomScatter;
    {
        const std::array<float, kBloomLevels> weights = BloomWeights(settings.bloomSpread);
        std::copy(weights.begin(), weights.begin() + 4, c.bloomWeights);
        std::copy(weights.begin() + 4, weights.end(), c.bloomWeightsHigh);
    }
    c.localGrid = local ? g_state.localGridBlurred.srv : kInvalid;
    c.localBlurred = local ? g_state.localMeanBlurred.srv : kInvalid;
    c.localHighlights = settings.localExposureHighlights;
    c.localShadows = settings.localExposureShadows;
    c.localDetail = settings.localExposureDetail;
    c.localBlend = settings.localExposureBlend;
    c.localMiddleGreyLog = std::log2(kMiddleGrey) + settings.localExposureMiddleGrey;
    c.localGridMinLog = std::log2(kMiddleGrey) - kLocalStopsBelow;
    c.localGridInvRangeLog = 1.0f / (kLocalStopsBelow + kLocalStopsAbove);
    // The wide blur is localExposureBlur percent of the height across: a Gaussian of half that.
    c.localBlurSigma = settings.localExposureBlur * 0.01f * 0.5f * static_cast<float>(height) / static_cast<float>(kLocalCell);
    // The lens flare: the double Gauss lens's ghosts at the depth of field's f-number (src/lens_flare.cpp) and its
    // aperture's diffraction, drawn lensFlareStarburstSize of the height across (the real pattern is smaller than a pixel)
    // with the light's whole power in the view (pi x a white surface facing it x the focal length in pixels squared,
    // the image of a light of that irradiance) spread over it, times lensFlareStarburst.
    std::vector<lens_flare::Ghost> ghosts;
    const bool flareDrawn = c.flareInFront != 0.0f && EnsureApertureTextures() && EnsureGhostBuffer() && EnsureFlareTarget();
    if (flareDrawn) {
        if (GetOptions().devMode && !std::exchange(g_state.loggedLens, true))
            lens_flare::LogPrescription();
        ghosts = lens_flare::Ghosts({static_cast<float>(width), static_cast<float>(height), c.focalPixels, c.flarePosition[0], c.flarePosition[1],
            settings.dofAperture, settings.lensFlareDispersion, 1.0f});
        if (ghosts.size() > kMaxGhosts)
            ghosts.resize(kMaxGhosts);
    }
    const float starburstHalf = 0.5f * settings.lensFlareStarburstSize * static_cast<float>(height);
    c.flareView = flareDrawn ? g_state.flare.srv : kInvalid;
    c.flareGhosts = flareDrawn ? g_state.ghosts.srv : kInvalid;
    c.flareGhostCount = static_cast<std::uint32_t>(ghosts.size());
    c.flareAperture = flareDrawn ? g_state.aperture.srv : kInvalid;
    c.flareStarburstView = flareDrawn ? g_state.starburst.srv : kInvalid;
    c.flareStarburstSize = starburstHalf;
    c.flareStarburst = settings.lensFlareStarburst * kPi * c.focalPixels * c.focalPixels / (4.0f * starburstHalf * starburstHalf);
    c.gradeExposure = settings.grading ? std::exp2(settings.exposure) : 1.0f;
    c.gradeEnabled = grade ? 1u : 0u;
    if (grade) {
        const std::array<float, 3> gain = GradeGain(settings);
        std::copy(gain.begin(), gain.end(), c.gradeGain);
        c.gradeContrast = settings.contrast;
        c.gradeSaturation = settings.saturation;
    }

    ID3D12GraphicsCommandList* commands = backend.Commands();
    const D3D12_GPU_VIRTUAL_ADDRESS constants = UploadConstants(&c, sizeof(c));
    if (constants == 0)
        return;
    ID3D12DescriptorHeap* heaps[] = {backend.Descriptors().Heap(), backend.SamplerHeap()};
    commands->SetDescriptorHeaps(2, heaps);
    commands->SetComputeRootSignature(g_state.root.Get());
    commands->SetComputeRootConstantBufferView(0, constants);
    constexpr D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Leaves the game's bindings to be set again by its next draw, also when a pass fails.
    struct Rebind {
        ~Rebind() { ForgetBindings(); }
    } rebind;

    // Timestamps up to the end of each pass; a pass that does not run takes no time.
    const bool timed = EnsureTimers() && g_state.slotFence[g_state.timingSlot] == 0;
    if (g_state.timestampValues != nullptr)
        ReadTimings();
    const UINT timingSlot = g_state.timingSlot;
    UINT marker = 0;
    const auto markTo = [&](UINT count) {
        if (!timed)
            return;
        for (; marker < count; ++marker)
            commands->EndQuery(g_state.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timingSlot * kTimingMarkers + marker);
    };
    const auto markAfter = [&](PostGpuPass pass) { markTo(static_cast<UINT>(pass) + 2); };
    markTo(1);

    backend.Transition(backend.DepthBuffer(), D3D12_RESOURCE_STATE_DEPTH_READ | read);
    if (frameGeneration) {
        Use(g_state.frameMotion, write);
        Use(g_state.frameDepth, write);
        if (!Dispatch(Compute::FrameMotion, {.output0 = g_state.frameMotion.uav, .output1 = g_state.frameDepth.uav}, Groups(width, 8), Groups(height, 8)))
            return;
        Use(g_state.frameMotion, read);
        Use(g_state.frameDepth, read);
    }
    // Frame generation's view without the HUD: the frame once the effects are in it, mapped as the swap chain's buffer
    // will be ("the same colour space and post-processing as the back buffer", NVIDIA's DLSS-G guide), kept until the
    // present.
    const auto tagFrame = [&] {
        if (!frameGeneration || !EnsureHudless(static_cast<UINT>(desc.Width), desc.Height, backend.OutputFormat()))
            return;
        backend.Transition(backBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        backend.Transition(g_state.hudless, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (!display::Map(commands, backend.FrameBufferTexture(), g_state.hudlessTarget, backend.OutputFormat(), static_cast<UINT>(desc.Width), desc.Height, 0, 0,
                backend.HudlessMapping()))
            return;
        backend.Transition(g_state.hudless, read);
        backend.Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12Resource* depth = g_state.frameDepth.texture.resource.Get();
        ID3D12Resource* motion = g_state.frameMotion.texture.resource.Get();
        ID3D12Resource* hudless = g_state.hudless.resource.Get();
        if (fsrGeneration != nullptr) {
            const display::Mapping mapping = backend.HudlessMapping();
            const FsrFrameGeneration::Inputs inputs = {depth, motion, hudless, rect, static_cast<UINT>(desc.Width), desc.Height,
                static_cast<float>(frameSeconds * 1000.0), backend.OutputFormat(), backend.OutputHdr(), mapping.blackLevel,
                mapping.peak * mapping.paperWhite};
            fsrGeneration->Prepare(commands, inputs, *post);
        } else {
            g_state.uiAlphaWanted = streamline::TagFrame(commands, {depth, motion, hudless, read, rect}, *post);
        }
    };
    backend.Transition(backBuffer, read);
    size_t current = 0;
    Use(g_state.scene[current], write);
    if (!Dispatch(Compute::Prepare, {.input0 = backend.FrameBufferTexture(), .output0 = g_state.scene[current].uav}, Groups(width, 8), Groups(height, 8)))
        return;
    // Measured on the view as Source exposed it, before the effects.
    const bool exposureRan = std::exchange(g_state.exposureRan, hdrExposure);
    if (hdrExposure) {
        Use(g_state.scene[current], read);
        if (!Dispatch(Compute::ExposureHistogram, {.input0 = g_state.scene[current].srv, .output0 = g_state.exposureBins.uav}, Groups(width, 16),
                Groups(height, 16)))
            return;
        UavBarrier(g_state.exposureBins);
        if (!Dispatch(Compute::Exposure, {.input0 = g_state.exposureBins.uav, .output0 = g_state.exposure.uav, .step = exposureRan ? 0u : 1u}, 1, 1))
            return;
        UavBarrier(g_state.exposure);
    }
    markAfter(PostGpuPass::Prepare);
    const auto next = [&](Compute pass, UINT x, UINT y, PostPass args) {
        Use(g_state.scene[current], read);
        Use(g_state.scene[current ^ 1], write);
        args.input0 = g_state.scene[current].srv;
        args.output0 = g_state.scene[current ^ 1].uav;
        if (!Dispatch(pass, args, x, y))
            return false;
        current ^= 1;
        return true;
    };
    if (depthOfField || flare) {
        if (!Dispatch(Compute::Camera, {.input0 = kInvalid, .output0 = g_state.camera.uav}, 1, 1))
            return;
        UavBarrier(g_state.camera);
    }
    if (motionBlur) {
        Use(g_state.tileMax, write);
        if (!Dispatch(Compute::TileMax, {.output0 = g_state.tileMax.uav}, c.tileCount[0], c.tileCount[1]))
            return;
        Use(g_state.tileMax, read);
        Use(g_state.neighborMax, write);
        if (!Dispatch(Compute::NeighborMax, {.input0 = g_state.tileMax.srv, .output0 = g_state.neighborMax.uav}, Groups(c.tileCount[0], 8),
                Groups(c.tileCount[1], 8)))
            return;
        Use(g_state.neighborMax, read);
        if (!next(Compute::MotionBlur, Groups(width, 8), Groups(height, 8), {.input1 = g_state.neighborMax.srv}))
            return;
    }
    markAfter(PostGpuPass::MotionBlur);
    if (depthOfField) {
        // The passes and their dispatch sizes of the SDK's dofDispatch (ffx_dof.cpp).
        const UINT fullTilesX = Groups(width, 8);
        const UINT fullTilesY = Groups(height, 8);
        const UINT halfWidth = (width + 1) / 2;
        const UINT halfHeight = (height + 1) / 2;
        Use(g_state.tileRadius, write);
        Use(g_state.globals, write);
        if (!Dispatch(camera->reverse ? Compute::DofTilesReverse : Compute::DofTiles, {}, Groups(fullTilesX, 8), Groups(fullTilesY, 8)))
            return;
        Use(g_state.scene[current], read);
        Use(g_state.bilateral, write);
        if (!Dispatch(Compute::DofColor, {.input0 = g_state.scene[current].srv}, Groups(width, 64), Groups(height, 64)))
            return;
        Use(g_state.tileRadius, read);
        UavBarrier(g_state.globals);
        Use(g_state.dilated, write);
        if (!Dispatch(Compute::DofDilate, {}, Groups(fullTilesX, 8), Groups(fullTilesY, 8)))
            return;
        Use(g_state.dilated, read);
        Use(g_state.bilateral, read);
        UavBarrier(g_state.globals);
        Use(g_state.nearField, write);
        Use(g_state.farField, write);
        if (!Dispatch(Compute::DofBlur, {}, Groups(halfWidth, 8), Groups(halfHeight, 8)))
            return;
        Use(g_state.nearField, read);
        Use(g_state.farField, read);
        if (!next(Compute::DofComposite, Groups(halfWidth, 8), Groups(halfHeight, 8), {}))
            return;
    }
    markAfter(PostGpuPass::DepthOfField);
    if (bloom) {
        Use(g_state.scene[current], read);
        UINT source = g_state.scene[current].srv;
        for (size_t level = 0; level < kBloomLevels; ++level) {
            Target& down = g_state.bloomDown[level];
            Use(down, write);
            if (!Dispatch(Compute::BloomDown, {.input0 = source, .output0 = down.uav, .step = static_cast<UINT>(level)}, Groups(BloomSize(width, level), 8),
                    Groups(BloomSize(height, level), 8)))
                return;
            Use(down, read);
            source = down.srv;
        }
        for (size_t level = kBloomLevels; level-- > 0;) {
            Target& up = g_state.bloomUp[level];
            Use(up, write);
            const UINT below = level + 1 < kBloomLevels ? g_state.bloomUp[level + 1].srv : kInvalid;
            if (!Dispatch(Compute::BloomUp, {.input0 = g_state.bloomDown[level].srv, .input1 = below, .output0 = up.uav, .step = static_cast<UINT>(level)},
                    Groups(BloomSize(width, level), 8), Groups(BloomSize(height, level), 8)))
                return;
            Use(up, read);
        }
    }
    markAfter(PostGpuPass::Bloom);
    if (flareDrawn) {
        // The aperture and its diffraction, once per blade count.
        if (g_state.apertureBlades != settings.lensFlareBlades) {
            const UINT groups = kApertureSize / 8;
            Use(g_state.aperture, write);
            if (!Dispatch(Compute::FlareAperture, {.output0 = g_state.aperture.uav, .step = static_cast<UINT>(settings.lensFlareBlades)}, groups, groups))
                return;
            Use(g_state.aperture, read);
            Use(g_state.fftRows, write);
            Use(g_state.fftEnergy, write);
            if (!Dispatch(Compute::FlareFftRows, {.input0 = g_state.aperture.srv, .output0 = g_state.fftRows.uav, .output1 = g_state.fftEnergy.uav}, kApertureSize, 1))
                return;
            Use(g_state.fftRows, read);
            UavBarrier(g_state.fftEnergy);
            Use(g_state.fftPower, write);
            if (!Dispatch(Compute::FlareFftColumns, {.input0 = g_state.fftRows.srv, .output0 = g_state.fftPower.uav, .output1 = g_state.fftEnergy.uav},
                    kApertureSize, 1))
                return;
            Use(g_state.fftPower, read);
            UavBarrier(g_state.fftEnergy);
            Use(g_state.starburst, write);
            if (!Dispatch(Compute::FlareStarburst, {.input0 = g_state.fftPower.srv, .input1 = g_state.fftEnergy.uav, .output0 = g_state.starburst.uav}, groups,
                    groups))
                return;
            g_state.apertureBlades = settings.lensFlareBlades;
        }
        constexpr D3D12_RESOURCE_STATES shaders = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Use(g_state.aperture, shaders);
        Use(g_state.starburst, shaders);
        if (!ghosts.empty()) {
            const UINT64 bytes = sizeof(lens_flare::Ghost) * ghosts.size();
            const Backend::Upload upload = backend.AllocateUpload(bytes, 256);
            if (upload.data == nullptr)
                return;
            std::memcpy(upload.data, ghosts.data(), bytes);
            Use(g_state.ghosts, D3D12_RESOURCE_STATE_COPY_DEST);
            commands->CopyBufferRegion(g_state.ghosts.texture.resource.Get(), 0, upload.buffer, upload.offset, bytes);
        }
        Use(g_state.ghosts, shaders);
        ID3D12PipelineState* ghostPipeline = GhostPipeline();
        if (ghostPipeline == nullptr)
            return;
        Use(g_state.flare, D3D12_RESOURCE_STATE_RENDER_TARGET);
        constexpr float kClear[4] = {};
        commands->ClearRenderTargetView(g_state.flareTarget, kClear, 0, nullptr);
        commands->SetGraphicsRootSignature(g_state.root.Get());
        commands->SetPipelineState(ghostPipeline);
        commands->SetGraphicsRootConstantBufferView(0, constants);
        const PostPass ghostArgs = {};
        commands->SetGraphicsRoot32BitConstants(1, sizeof(PostPass) / sizeof(UINT), &ghostArgs, 0);
        commands->OMSetRenderTargets(1, &g_state.flareTarget, FALSE, nullptr);
        const UINT flareWidth = BloomSize(width, 0);
        const UINT flareHeight = BloomSize(height, 0);
        const D3D12_VIEWPORT flareViewport = {0.0f, 0.0f, static_cast<float>(flareWidth), static_cast<float>(flareHeight), 0.0f, 1.0f};
        const D3D12_RECT flareScissor = {0, 0, static_cast<LONG>(flareWidth), static_cast<LONG>(flareHeight)};
        commands->RSSetViewports(1, &flareViewport);
        commands->RSSetScissorRects(1, &flareScissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(6, static_cast<UINT>(ghosts.size()) + 1, 0, 0);
        Use(g_state.flare, read);
    }
    markAfter(PostGpuPass::Flare);
    if (lens && !next(Compute::Lens, Groups(width, 8), Groups(height, 8), {}))
        return;
    markAfter(PostGpuPass::Lens);
    if (settings.contrastSharpening && !next(Compute::Cas, Groups(width, 8), Groups(height, 8), {}))
        return;
    markAfter(PostGpuPass::Sharpening);
    if (local) {
        const UINT cellsX = Groups(width, kLocalCell);
        const UINT cellsY = Groups(height, kLocalCell);
        Use(g_state.scene[current], read);
        Use(g_state.localGrid, write);
        Use(g_state.localMean, write);
        if (!Dispatch(Compute::LocalGrid, {.input0 = g_state.scene[current].srv, .output0 = g_state.localGrid.uav, .output1 = g_state.localMean.uav}, cellsX,
                cellsY))
            return;
        Use(g_state.localGrid, read);
        Use(g_state.localMean, read);
        Use(g_state.localGridBlurred, write);
        if (!Dispatch(Compute::LocalBlurGrid, {.input0 = g_state.localGrid.srv, .output0 = g_state.localGridBlurred.uav}, Groups(cellsX, 4), Groups(cellsY, 4),
                kLocalBins / 4))
            return;
        Use(g_state.localMeanTemp, write);
        if (!Dispatch(Compute::LocalBlurMean, {.input0 = g_state.localMean.srv, .output0 = g_state.localMeanTemp.uav, .step = 1}, Groups(cellsX, 8),
                Groups(cellsY, 8)))
            return;
        Use(g_state.localMeanTemp, read);
        Use(g_state.localMeanBlurred, write);
        if (!Dispatch(Compute::LocalBlurMean, {.input0 = g_state.localMeanTemp.srv, .output0 = g_state.localMeanBlurred.uav, .step = 2}, Groups(cellsX, 8),
                Groups(cellsY, 8)))
            return;
        Use(g_state.localGridBlurred, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Use(g_state.localMeanBlurred, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    markAfter(PostGpuPass::LocalExposure);

    ID3D12PipelineState* final = FinalPipeline();
    if (final == nullptr)
        return;
    Use(g_state.scene[current], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    backend.Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commands->SetGraphicsRootSignature(g_state.root.Get());
    commands->SetPipelineState(final);
    commands->SetGraphicsRootConstantBufferView(0, constants);
    const PostPass finalArgs = {.input0 = g_state.scene[current].srv};
    commands->SetGraphicsRoot32BitConstants(1, sizeof(PostPass) / sizeof(UINT), &finalArgs, 0);
    const D3D12_CPU_DESCRIPTOR_HANDLE target = backend.FrameBufferView();
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(width), static_cast<float>(height), 0, 1};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &rect);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
    markAfter(PostGpuPass::Final);
    if (timed) {
        commands->ResolveQueryData(g_state.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timingSlot * kTimingMarkers, kTimingMarkers,
            g_state.timestampReadback.Get(), sizeof(std::uint64_t) * timingSlot * kTimingMarkers);
        g_state.slotFence[timingSlot] = backend.PendingFence();
        g_state.timingSlot = (timingSlot + 1) % kTimingSlots;
    }
    tagFrame();
}

bool PrepareUiAlpha(display::Mapping& mapping, UINT width, UINT height)
{
    Backend& backend = GetBackend();
    if (!std::exchange(g_state.uiAlphaWanted, false) || !g_state.hudless.resource || g_state.hudlessSize[0] != width || g_state.hudlessSize[1] != height ||
        g_state.hudlessFormat != backend.OutputFormat())
        return false;
    if (!g_state.uiAlpha.resource || g_state.uiAlphaSize[0] != width || g_state.uiAlphaSize[1] != height) {
        if (g_state.uiAlpha.resource)
            backend.Retire(std::move(g_state.uiAlpha.resource));
        g_state.uiAlpha = {};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        const HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&g_state.uiAlpha.resource));
        if (FAILED(hr)) {
            log::Info("Post effects: creating frame generation's UI alpha failed: {:#010x}", static_cast<unsigned>(hr));
            return false;
        }
        g_state.uiAlpha.state = D3D12_RESOURCE_STATE_COMMON;
        if (g_state.uiAlphaTarget.ptr == 0)
            g_state.uiAlphaTarget = backend.RenderTargetViews().Allocate();
        backend.Device()->CreateRenderTargetView(g_state.uiAlpha.resource.Get(), nullptr, g_state.uiAlphaTarget);
        g_state.uiAlphaSize[0] = width;
        g_state.uiAlphaSize[1] = height;
    }
    backend.Transition(g_state.uiAlpha, D3D12_RESOURCE_STATE_RENDER_TARGET);
    backend.Transition(g_state.hudless, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mapping.uiAlphaTarget = g_state.uiAlphaTarget;
    mapping.hudlessView = g_state.hudlessView;
    return true;
}

void TagUiAlpha()
{
    Backend& backend = GetBackend();
    constexpr D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // TagFrame left the view without the HUD in this state until the present.
    backend.Transition(g_state.hudless, read);
    backend.Transition(g_state.uiAlpha, read);
    streamline::TagUiAlpha(backend.Commands(), g_state.uiAlpha.resource.Get(), read, g_state.uiAlphaSize[0], g_state.uiAlphaSize[1]);
}

}
