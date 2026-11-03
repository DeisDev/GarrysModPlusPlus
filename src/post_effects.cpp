#include "post_effects.h"

#include "color.h"
#include "log.h"
#include "settings.h"
#include "shaderapi.h"
#include "shaderapi_device.h"
#include "shaderapi_pipelines.h"
#include "world.h"

#include <wrl/client.h>

#include <g_casCS.h>
#include <g_dof0CS.h>
#include <g_dof0ReverseCS.h>
#include <g_dof1CS.h>
#include <g_dof2CS.h>
#include <g_dof3CS.h>
#include <g_dof4CS.h>
#include <g_lensCS.h>
#include <g_postCameraCS.h>
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
#include <cstring>
#include <optional>
#include <span>
#include <utility>

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
    std::uint32_t padding4;
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
    std::uint32_t padding0;
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
    std::uint32_t padding[3];
};
static_assert(sizeof(PostConstants) == 17 * 16);

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

enum class Compute { Prepare, Camera, TileMax, NeighborMax, MotionBlur, DofTiles, DofTilesReverse, DofColor, DofDilate, DofBlur, DofComposite, Cas, Lens, Count };

constexpr std::span<const unsigned char> kComputeShaders[] = {g_postPrepareCS, g_postCameraCS, g_postTileMaxCS, g_postNeighborMaxCS, g_postMotionBlurCS,
    g_dof0CS, g_dof0ReverseCS, g_dof1CS, g_dof2CS, g_dof3CS, g_dof4CS, g_casCS, g_lensCS};
static_assert(std::size(kComputeShaders) == static_cast<size_t>(Compute::Count));

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
    Target tileRadius;
    Target dilated;
    Target nearField;
    Target farField;
    Target globals;
    Target tileMax;
    Target neighborMax;
    UINT blurTile = 0;
    // The camera's state across frames (CameraMain in shaders/post/post.hlsl).
    Target camera;
    ID3D12Resource* depthResource = nullptr;
    UINT depthView = kInvalid;
    std::chrono::steady_clock::time_point last;
    double seconds = 0.0;
};

State g_state;

float Luminance(const std::array<float, 3>& color) { return 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2]; }

// The per-channel gain of exposure and white balance: warmth recolours the view as if lit by a black body of
// 6500 K x 2^-warmth rather than 6500 K (the sRGB white), tint shifts it between green and magenta; neither changes
// its luminance.
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
    const float scale = std::exp2(settings.exposure) / std::max(Luminance(balance), 1e-6f);
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
    for (Target* target : {&g_state.bilateral, &g_state.tileRadius, &g_state.dilated, &g_state.nearField, &g_state.farField, &g_state.globals,
             &g_state.tileMax, &g_state.neighborMax})
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
        CreateTexture(g_state.neighborMax, blurTilesX, blurTilesY, DXGI_FORMAT_R16G16_FLOAT);
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
    return GetBackend().Device() != nullptr && (g_state.camera.texture.resource || CreateBuffer(g_state.camera, 16, 2));
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
        desc.RTVFormats[0] = Backend::kBackBufferFormat;
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

void Use(Target& target, D3D12_RESOURCE_STATES state) { GetBackend().Transition(target.texture, state); }

void UavBarrier(Target& target)
{
    const D3D12_RESOURCE_BARRIER barrier = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {target.texture.resource.Get()}}};
    GetBackend().Commands()->ResourceBarrier(1, &barrier);
}

// Records a compute pass over `x` x `y` x 1 groups; false when its pipeline cannot be made.
bool Dispatch(Compute pass, const PostPass& args, UINT x, UINT y)
{
    ID3D12PipelineState* pipeline = Pipeline(pass);
    if (pipeline == nullptr)
        return false;
    ID3D12GraphicsCommandList* commands = GetBackend().Commands();
    commands->SetPipelineState(pipeline);
    commands->SetComputeRoot32BitConstants(1, sizeof(PostPass) / sizeof(UINT), &args, 0);
    commands->Dispatch(x, y, 1);
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

void PostProcessView(D3D12_RECT rect)
{
    const Settings& settings = GetSettings();
    if (g_state.failed)
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
    const bool lens = aberration > 0.0f || distortion != 0.0f || vignette > 0.0f || flare;
    const bool grain = settings.filmGrain && settings.grainAmount > 0.0f;
    const bool grade = GradingChanges(settings);
    if (!depthOfField && !motionBlur && !lens && !settings.contrastSharpening && !grain && !grade)
        return;
    if (Dynamic().colorTargets[0] != kDefaultTarget) {
        if (!std::exchange(g_state.loggedTarget, true))
            log::Info("Post effects: skipped a view drawn outside the back buffer");
        return;
    }
    Backend& backend = GetBackend();
    TrackedResource& backBuffer = backend.BackBuffer();
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
    c.motionView = motionBlur ? post->motionView : kInvalid;
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
            c.flareStarburst = settings.lensFlareStarburst;
            c.flareVisibilityView = post->flareVisibilityView;
        }
    }
    c.chromaticAberration = aberration;
    c.distortion = distortion;
    // Pincushion distortion samples beyond the view's corners; the image is scaled so they stay inside.
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    c.distortionScale = distortion > 0.0f ? 1.0f / (1.0f + distortion * (0.25f * aspect * aspect + 0.25f)) : 1.0f;
    c.vignette = vignette;
    c.grainAmount = grain ? settings.grainAmount : 0.0f;
    c.grainScale = settings.grainSize * static_cast<float>(height) / 1080.0f;
    c.grainSeed = static_cast<std::uint32_t>(g_state.seconds * kGrainRate);
    c.sharpness = settings.contrastSharpness;
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

    backend.Transition(backBuffer, read);
    backend.Transition(backend.DepthBuffer(), D3D12_RESOURCE_STATE_DEPTH_READ | read);
    size_t current = 0;
    Use(g_state.scene[current], write);
    if (!Dispatch(Compute::Prepare, {.input0 = backend.BackBufferTexture(), .output0 = g_state.scene[current].uav}, Groups(width, 8), Groups(height, 8)))
        return;
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
    if (lens && !next(Compute::Lens, Groups(width, 8), Groups(height, 8), {}))
        return;
    if (settings.contrastSharpening && !next(Compute::Cas, Groups(width, 8), Groups(height, 8), {}))
        return;

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
    const D3D12_CPU_DESCRIPTOR_HANDLE target = backend.BackBufferView(false);
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(width), static_cast<float>(height), 0, 1};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &rect);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
}

}
