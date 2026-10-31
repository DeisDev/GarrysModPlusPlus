#include "lighting.h"

#include "light_queries.h"
#include "log.h"
#include "options.h"
#include "settings.h"

#include <g_aoCS.h>
#include <g_bakedResidualCS.h>
#include <g_aoOmmCS.h>
#include <g_compositeCoveredPS.h>
#include <g_compositeDepthPS.h>
#include <g_compositePS.h>
#include <g_compositeVS.h>
#include <g_deferredLightingCS.h>
#include <g_directCS.h>
#include <g_directOmmCS.h>
#include <g_directWideCS.h>
#include <g_directWideOmmCS.h>
#include <g_dlssInputsCS.h>
#include <g_dlssResolveCS.h>
#include <g_giFilterCS.h>
#include <g_giTemporalCS.h>
#include <g_giTraceCS.h>
#include <g_giTraceOmmCS.h>
#include <g_translucentDepthCS.h>
#include <g_viewRectCS.h>
#include <g_primaryCS.h>
#include <g_primaryOmmCS.h>
#include <g_probesCS.h>
#include <g_probesOmmCS.h>
#include <g_reflectionTemporalCS.h>
#include <g_reflectionsCS.h>
#include <g_reflectionsOmmCS.h>
#include <g_skyCS.h>
#include <g_surfaceCS.h>
#include <g_swayCS.h>
#include <g_taaCS.h>
#include <g_taaSharpenCS.h>
#include <g_volumetricsCS.h>
#include <g_volumetricsOmmCS.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <tuple>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

constexpr UINT kConstantsStride = 4 * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
static_assert(sizeof(FrameConstants) <= kConstantsStride);
static_assert(offsetof(FrameConstants, lightWarpViews) % 16 == 0);
static_assert(offsetof(FrameConstants, skyboxFogParams) % 16 == 0);
// Each slot has the view's constants and a copy for the passes Ray Reconstruction runs at render resolution.
constexpr UINT kConstantsPerSlot = 2;
// Per slot: colour copy, depth, G-buffer normal, flags, light, albedo and self-illumination views, then projector cookies.
constexpr UINT kInputViews = 7;
constexpr UINT kMaxProjectors = 8;
constexpr UINT kViewsPerSlot = kInputViews + kMaxProjectors;
constexpr UINT kInvalidView = ~0u;
// Texels on a side of each face of the sky light (kSize in shaders/sky.hlsl).
constexpr UINT kSkyLightSize = 8;

// Mirrors Projector in shaders/scene.hlsli.
struct GpuProjector {
    float worldToTexture[4][4];
    float origin[3];
    float farZ;
    float color[3];
    uint32_t cookieView;
    float attenuation[3];
    float fullDistance;
    uint32_t flags;
    uint32_t padding[3];
};
static_assert(sizeof(GpuProjector) == 128);
// kProjectorLocalFlashlight in shaders/scene.hlsli.
constexpr uint32_t kProjectorLocalFlashlight = 0x01;

// GMod's flashlight (CFlashlightEffect::UpdateLightNew) sits r_flashlightoffsety (-20) units below the eye along the
// view's up axis and aims at the point r_flashlightfar ahead of the eye, but within 128 units of a wall it backs away
// along its axis so that its texture does not shrink, and then shines through the player's body. A projector whose axis
// follows the view this closely and passes the eye this near is taken to be it, and moved back to where it is held.
constexpr float kFlashlightAxisAlignment = 0.99f;
constexpr float kFlashlightEyeDistance = 32.0f;
constexpr float kFlashlightMaxPullBack = 160.0f;
// In third person GMod's flashlight sits on the local player's model (the hand holding the weapon), which the view's
// axis does not pass. A projector this close to the middle of the player's body (kPlayerCentreHeight above its feet) is
// taken to be it; it stays where it is.
constexpr float kPlayerCentreHeight = 36.0f;
constexpr float kThirdPersonFlashlightReach = 56.0f;

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// How far the local player's flashlight was pulled back from where it is held, or nothing when the projector is not it.
std::optional<float> FlashlightPullBack(const GpuProjector& projector, const float eye[3], const float viewForward[3])
{
    const float* axisRow = projector.worldToTexture[3];
    const float length = std::sqrt(Dot(axisRow, axisRow));
    if (length <= 0.0f)
        return std::nullopt;
    const float axis[3] = {axisRow[0] / length, axisRow[1] / length, axisRow[2] / length};
    if (Dot(axis, viewForward) < kFlashlightAxisAlignment)
        return std::nullopt;
    const float toEye[3] = {eye[0] - projector.origin[0], eye[1] - projector.origin[1], eye[2] - projector.origin[2]};
    const float along = Dot(toEye, axis);
    const float off = std::sqrt(std::max(Dot(toEye, toEye) - along * along, 0.0f));
    if (along < -1.0f || along > kFlashlightMaxPullBack || off > kFlashlightEyeDistance)
        return std::nullopt;
    return std::max(along, 0.0f);
}

// Moves a projector's apex `distance` along its axis, keeping its orientation and field of view: each texture
// coordinate row is evaluated at the point that far behind.
void AdvanceApex(GpuProjector& projector, float distance)
{
    const float* axisRow = projector.worldToTexture[3];
    const float length = std::sqrt(Dot(axisRow, axisRow));
    const float axis[3] = {axisRow[0] / length, axisRow[1] / length, axisRow[2] / length};
    for (auto& row : projector.worldToTexture)
        row[3] -= distance * Dot(row, axis);
    for (int i = 0; i < 3; ++i)
        projector.origin[i] += distance * axis[i];
}

// Where Source's attenuation c + l/d + q/d² first reaches full strength (1), or 0 when it never falls below it
// or never reaches it.
float FullStrengthDistance(const std::array<float, 3>& attenuation)
{
    const auto [c, l, q] = attenuation;
    if (c >= 1.0f)
        return 0.0f;
    // With x = 1/d: q x² + l x + (c - 1) = 0.
    float x = 0.0f;
    if (q > 0.0f)
        x = (-l + std::sqrt(l * l - 4.0f * q * (c - 1.0f))) / (2.0f * q);
    else if (l > 0.0f)
        x = (1.0f - c) / l;
    return x > 0.0f ? 1.0f / x : 0.0f;
}
constexpr UINT kTraceMask = kWorldMask | kStaticPropMask | kShadowlessPropMask | kLampShellMask | kDynamicMask | kLocalPlayerMask;
// The viewmodel is drawn inside the local player's body, which would shadow it.
constexpr UINT kViewmodelTraceMask = kTraceMask & ~kLocalPlayerMask;
constexpr UINT kFilterSteps[] = {1, 2, 4, 8, 16};
// The viewmodel has no history, so it takes several indirect samples per pixel.
constexpr UINT kViewmodelIndirectSamples = 8;
constexpr UINT kPassCount = static_cast<UINT>(GpuPass::Count);
// A timestamp before the first pass and after each one.
constexpr UINT kMarkers = kPassCount + 1;
constexpr uint32_t kTimingSamples = 120;
constexpr UINT kProbeBytes = 32;
// Luxels the residual bake handles per main view, which keeps each part well below the GPU timeout.
constexpr UINT kBakeLuxelsPerView = 1 << 17;
// VRAD's extrapasses: rounds of supersampling at light gradients.
constexpr UINT kSupersamplePasses = 4;
constexpr UINT kMaxGroups = 65535;

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

bool Invert(const float m[4][4], float out[4][4])
{
    const float* a = &m[0][0];
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-20f)
        return false;
    for (int i = 0; i < 16; ++i)
        (&out[0][0])[i] = inv[i] / det;
    return true;
}

// Splits a view-projection into a rigid world-to-view transform (x right, y up, z forward from the eye) and the
// projection after it, both row-major for row vectors (v' = v M) as Ray Reconstruction takes them.
bool SplitViewProjection(const float viewProj[4][4], const float eye[3], float worldToView[16], float viewToClip[16])
{
    auto normalize = [](float v[3]) {
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length < 1e-12f)
            return false;
        for (int i = 0; i < 3; ++i)
            v[i] /= length;
        return true;
    };
    auto dot = [](const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    float axes[3][3];
    for (int i = 0; i < 3; ++i)
        axes[2][i] = viewProj[3][i];
    if (!normalize(axes[2]))
        return false;
    // Right and up from clip x and y, made orthogonal to forward and to each other.
    for (int row = 0; row < 2; ++row) {
        for (int i = 0; i < 3; ++i)
            axes[row][i] = viewProj[row][i];
        for (int other : {2, 0}) {
            if (other == row)
                continue;
            const float d = dot(axes[row], axes[other]);
            for (int i = 0; i < 3; ++i)
                axes[row][i] -= d * axes[other][i];
        }
        if (!normalize(axes[row]))
            return false;
    }
    // Column-vector forms: view = [axes, -axes * eye], its inverse = [axes^T, eye].
    float view[4][4] = {};
    float inverseView[4][4] = {};
    for (int a = 0; a < 3; ++a) {
        for (int i = 0; i < 3; ++i) {
            view[a][i] = axes[a][i];
            inverseView[i][a] = axes[a][i];
        }
        view[a][3] = -dot(axes[a], eye);
        inverseView[a][3] = eye[a];
    }
    view[3][3] = inverseView[3][3] = 1.0f;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float projection = 0.0f;
            for (int k = 0; k < 4; ++k)
                projection += viewProj[i][k] * inverseView[k][j];
            viewToClip[j * 4 + i] = projection;
            worldToView[j * 4 + i] = view[i][j];
        }
    }
    return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

DXGI_FORMAT ColorViewFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
    default: return format;
    }
}

// The sRGB view of a texture the game samples with D3DSAMP_SRGBTEXTURE, or its plain view when there is none.
DXGI_FORMAT SrgbViewFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM_SRGB;
    case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM_SRGB;
    case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM_SRGB;
    default: return ColorViewFormat(format);
    }
}

DXGI_FORMAT DepthStencilFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;
    default: return format;
    }
}

DXGI_FORMAT DepthViewFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES properties = {heap};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer))))
        return nullptr;
    return buffer;
}

}

bool Lighting::Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue, DescriptorHeap& descriptors, bool opacityMicromaps,
    const std::filesystem::path& dlssDirectory)
{
    m_device = device;
    m_queue = queue;
    m_descriptors = &descriptors;
    m_opacityMicromaps = opacityMicromaps;
    HRESULT hr = S_OK;
    for (auto& allocator : m_allocators) {
        if (SUCCEEDED(hr))
            hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    }
    if (SUCCEEDED(hr))
        hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].Get(), nullptr, IID_PPV_ARGS(&m_list));
    if (SUCCEEDED(hr))
        hr = m_list->Close();
    if (SUCCEEDED(hr))
        hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kSlots};
        hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
        hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_dsvHeap));
    }
    if (SUCCEEDED(hr)) {
        m_constants = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, kConstantsStride * kConstantsPerSlot * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
        hr = m_constants ? m_constants->Map(0, nullptr, reinterpret_cast<void**>(&m_constantsMapped)) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        m_forwardConstants = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, kConstantsStride * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
        hr = m_forwardConstants ? m_forwardConstants->Map(0, nullptr, reinterpret_cast<void**>(&m_forwardConstantsMapped)) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        m_bakeConstants = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, kConstantsStride, D3D12_RESOURCE_STATE_GENERIC_READ);
        hr = m_bakeConstants ? m_bakeConstants->Map(0, nullptr, reinterpret_cast<void**>(&m_bakeConstantsMapped)) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        m_bakeReadback = CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
        hr = m_bakeReadback ? S_OK : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        m_projectors = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, sizeof(GpuProjector) * kMaxProjectors * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
        hr = m_projectors ? m_projectors->Map(0, nullptr, reinterpret_cast<void**>(&m_projectorsMapped)) : E_OUTOFMEMORY;
    }
    for (UINT slot = 0; slot < kSlots && SUCCEEDED(hr); ++slot) {
        m_projectorViews[slot] = m_descriptors->Allocate();
        if (m_projectorViews[slot] == DescriptorHeap::kInvalid) {
            hr = E_OUTOFMEMORY;
            break;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = slot * kMaxProjectors * sizeof(GpuProjector) / 4;
        srv.Buffer.NumElements = kMaxProjectors * sizeof(GpuProjector) / 4;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        m_device->CreateShaderResourceView(m_projectors.Get(), &srv, m_descriptors->Cpu(m_projectorViews[slot]));
    }
    if (SUCCEEDED(hr) && GetOptions().devMode) {
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kProbeBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_probeBuffer));
        m_probeReadback = SUCCEEDED(hr) ? CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, kProbeBytes, D3D12_RESOURCE_STATE_COPY_DEST) : nullptr;
        m_probeUav = m_descriptors->Allocate();
        if (SUCCEEDED(hr) && (!m_probeReadback || m_probeUav == DescriptorHeap::kInvalid))
            hr = E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = kProbeBytes / 4;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            m_device->CreateUnorderedAccessView(m_probeBuffer.Get(), nullptr, &uav, m_descriptors->Cpu(m_probeUav));
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * kMarkers};
        hr = m_device->CreateQueryHeap(&desc, IID_PPV_ARGS(&m_timestamps));
    }
    if (SUCCEEDED(hr)) {
        m_timestampReadback = CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, sizeof(uint64_t) * kMarkers * kSlots, D3D12_RESOURCE_STATE_COPY_DEST);
        hr = m_timestampReadback ? m_timestampReadback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint64_t**>(&m_timestampValues))) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        UINT64 frequency = 0;
        hr = m_queue->GetTimestampFrequency(&frequency);
        m_timestampPeriodMs = frequency ? 1000.0 / static_cast<double>(frequency) : 0.0;
    }
    if (SUCCEEDED(hr))
        hr = m_device->CreateRootSignature(0, g_surfaceCS, sizeof(g_surfaceCS), IID_PPV_ARGS(&m_computeRoot));
    const bool micromaps = opacityMicromaps;
    const std::span<const BYTE> shaders[] = {g_surfaceCS, g_primaryCS, g_probesCS, g_aoCS, g_volumetricsCS, g_directCS, g_giTraceCS, g_giTemporalCS,
        g_giFilterCS, g_deferredLightingCS, g_taaCS, g_taaSharpenCS, g_reflectionsCS, g_reflectionTemporalCS, g_dlssInputsCS, g_dlssResolveCS, g_directWideCS, g_swayCS, g_translucentDepthCS, g_viewRectCS, g_bakedResidualCS, g_skyCS};
    static_assert(std::size(shaders) == static_cast<size_t>(Pass::Count));
    for (size_t i = 0; i < m_pipelines.size() && SUCCEEDED(hr); ++i) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_computeRoot.Get();
        desc.CS = {shaders[i].data(), shaders[i].size()};
        hr = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_pipelines[i]));
    }
    // Variants of the passes that trace rays whose ray queries let opacity micromaps settle alpha tests.
    const std::pair<Pass, std::span<const BYTE>> micromapShaders[] = {{Pass::Primary, g_primaryOmmCS}, {Pass::Probes, g_probesOmmCS}, {Pass::Ao, g_aoOmmCS},
        {Pass::Volumetrics, g_volumetricsOmmCS}, {Pass::Direct, g_directOmmCS}, {Pass::GiTrace, g_giTraceOmmCS}, {Pass::Reflections, g_reflectionsOmmCS},
        {Pass::DirectWide, g_directWideOmmCS}};
    for (size_t i = 0; i < std::size(micromapShaders) && SUCCEEDED(hr) && micromaps; ++i) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_computeRoot.Get();
        desc.CS = {micromapShaders[i].second.data(), micromapShaders[i].second.size()};
        hr = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_micromapPipelines[static_cast<size_t>(micromapShaders[i].first)]));
    }
    if (SUCCEEDED(hr))
        hr = m_device->CreateRootSignature(0, g_compositeVS, sizeof(g_compositeVS), IID_PPV_ARGS(&m_compositeRoot));
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_compositeRoot.Get();
        desc.VS = {g_compositeVS, sizeof(g_compositeVS)};
        desc.PS = {g_compositePS, sizeof(g_compositePS)};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_compositePipeline));
        if (SUCCEEDED(hr)) {
            desc.PS = {g_compositeCoveredPS, sizeof(g_compositeCoveredPS)};
            hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_compositeCoveredPipeline));
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_INDIRECT_ARGUMENT_DESC argument = {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
        D3D12_COMMAND_SIGNATURE_DESC desc = {sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument, 0};
        hr = m_device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&m_dispatchSignature));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 64;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_viewRect.resource));
        if (SUCCEEDED(hr)) {
            m_viewRect.srv = m_descriptors->CreateRawView(m_viewRect.resource.Get());
            m_viewRect.uav = m_descriptors->CreateRawUav(m_viewRect.resource.Get());
            if (m_viewRect.srv == DescriptorHeap::kInvalid || m_viewRect.uav == DescriptorHeap::kInvalid)
                hr = E_OUTOFMEMORY;
        }
    }
    if (SUCCEEDED(hr)) {
        m_slotViews = m_descriptors->Reserve(kSlots * kViewsPerSlot);
        m_rrInputUavs = m_descriptors->Reserve(RrInputCount);
        if (m_slotViews == DescriptorHeap::kInvalid || m_rrInputUavs == DescriptorHeap::kInvalid)
            hr = E_OUTOFMEMORY;
    }
    if (FAILED(hr)) {
        log::Info("Lighting: initialization failed ({:#010x})", Hex(hr));
        return false;
    }
    const UINT* grid = gbuffer::kProbeGrid;
    bool volume = true;
    for (auto& set : m_probeHistory) {
        for (size_t i = 0; i < set.size(); ++i)
            volume = volume && CreateTarget(set[i], grid[0], grid[1], i < 3 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16B16A16_SINT, grid[2]);
    }
    for (Texture& texture : m_probeVolume)
        volume = volume && CreateTarget(texture, grid[0], grid[1], DXGI_FORMAT_R16G16B16A16_FLOAT, grid[2]);
    const UINT indirectUavs = m_descriptors->Reserve(static_cast<UINT>(m_probeIndirect.size()));
    for (size_t i = 0; i < m_probeIndirect.size() && indirectUavs != DescriptorHeap::kInvalid; ++i) {
        m_probeIndirect[i].uav = indirectUavs + static_cast<UINT>(i);
        if (!CreateTarget(m_probeIndirect[i], grid[0], grid[1], DXGI_FORMAT_R16G16B16A16_FLOAT, grid[2]))
            m_probeIndirect[0].resource.Reset();
    }
    if (!volume) {
        log::Info("Lighting: creating the irradiance volume failed; translucent surfaces keep their baked light");
        m_probeVolume = {};
    } else {
        const D3D12_RESOURCE_DESC desc = m_probeVolume[0].resource->GetDesc();
        UINT64 bytes = 0;
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &m_volumeFootprints[0], nullptr, nullptr, &bytes);
        bytes = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        for (size_t i = 1; i < m_volumeFootprints.size(); ++i) {
            m_volumeFootprints[i] = m_volumeFootprints[0];
            m_volumeFootprints[i].Offset = bytes * i;
        }
        for (auto& readback : m_volumeReadback)
            readback = CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, bytes * m_volumeFootprints.size(), D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (!CreateSkyLight())
        log::Info("Lighting: creating the sky light failed; indirect light takes the map's flat sky light");
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_rtvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_skinned.Initialize(m_device, *m_descriptors);
    m_sway.Initialize(m_device, *m_descriptors);
    for (UINT i = 0; i < RrInputCount; ++i)
        m_rrInputs[i].uav = m_rrInputUavs + i;
    GetStatistics().rayReconstructionAvailable = m_rayReconstruction.Initialize(m_device, dlssDirectory, GetOptions().gameDirectory);
    GetStatistics().opacityMicromapsAvailable = micromaps;
    log::Info("Lighting: initialized");
    return true;
}

bool Lighting::CreateTarget(Texture& texture, UINT width, UINT height, DXGI_FORMAT format, UINT depth)
{
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = depth != 0 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = static_cast<UINT16>(depth != 0 ? depth : 1);
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&texture.resource));
    if (FAILED(hr)) {
        log::Info("Lighting: {}x{} target creation failed ({:#010x})", width, height, Hex(hr));
        return false;
    }
    texture.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (texture.srv == DescriptorHeap::kInvalid)
        texture.srv = m_descriptors->Allocate();
    if (texture.uav == DescriptorHeap::kInvalid)
        texture.uav = m_descriptors->Allocate();
    if (texture.srv == DescriptorHeap::kInvalid || texture.uav == DescriptorHeap::kInvalid)
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = format;
    srv.ViewDimension = depth != 0 ? D3D12_SRV_DIMENSION_TEXTURE3D : D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(texture.resource.Get(), &srv, m_descriptors->Cpu(texture.srv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = format;
    uav.ViewDimension = depth != 0 ? D3D12_UAV_DIMENSION_TEXTURE3D : D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture3D.WSize = depth != 0 ? depth : 0;
    m_device->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &uav, m_descriptors->Cpu(texture.uav));
    return true;
}

bool Lighting::CreateSkyLight()
{
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kSkyLightSize;
    desc.Height = kSkyLightSize;
    desc.DepthOrArraySize = 6;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_skyLight.resource))))
        return false;
    m_skyLight.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_skyLight.srv = m_descriptors->Allocate();
    m_skyLight.uav = m_descriptors->Allocate();
    if (m_skyLight.srv == DescriptorHeap::kInvalid || m_skyLight.uav == DescriptorHeap::kInvalid) {
        m_skyLight = {};
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MipLevels = 1;
    m_device->CreateShaderResourceView(m_skyLight.resource.Get(), &srv, m_descriptors->Cpu(m_skyLight.srv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = desc.Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray.ArraySize = 6;
    m_device->CreateUnorderedAccessView(m_skyLight.resource.Get(), nullptr, &uav, m_descriptors->Cpu(m_skyLight.uav));
    return true;
}

bool Lighting::EnsureTargets(UINT width, UINT height)
{
    if (m_colorCopy && width == m_width && height == m_height)
        return true;
    WaitForGpu();
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    m_colorCopy.Reset();
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_colorCopy))))
        return false;
    bool ok = true;
    for (Texture& texture : m_surface)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R32G32B32A32_FLOAT);
    ok = ok && CreateTarget(m_visibility, width, height, DXGI_FORMAT_R32G32B32A32_UINT);
    ok = ok && CreateTarget(m_motion, width, height, DXGI_FORMAT_R16G16_FLOAT);
    ok = ok && CreateTarget(m_previousDistance, width, height, DXGI_FORMAT_R32_FLOAT);
    ok = ok && CreateTarget(m_pixelFlags, width, height, DXGI_FORMAT_R8_UINT);
    ok = ok && CreateTarget(m_geometricNormal, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_direct)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_directRemainder, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_specular, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_giRaw, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_giHistory)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // Squared luminance of fireflies overflows FP16.
    for (Texture& texture : m_giMoments)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R32G32_FLOAT);
    for (Texture& texture : m_giFiltered)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // Linear (Ray Reconstruction) or encoded shaded colour.
    for (Texture& texture : m_lit)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_reflections, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_aoHistory)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16_FLOAT);
    ok = ok && CreateTarget(m_ao, width, height, DXGI_FORMAT_R16_FLOAT);
    ok = ok && CreateTarget(m_volumetricMarch, (width + 1) / 2, (height + 1) / 2, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_volumetricHistory)
        ok = ok && CreateTarget(texture, (width + 1) / 2, (height + 1) / 2, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_reflectionHistory)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_taaHistory)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_viewmodelIndirect)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_resolved, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    ok = ok && CreateTarget(m_translucentDepth, width, height, DXGI_FORMAT_R32_FLOAT);
    // Ray Reconstruction's inputs use the top-left render-size part of these.
    constexpr DXGI_FORMAT kRrFormats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
    static_assert(std::size(kRrFormats) == RrInputCount);
    for (UINT i = 0; i < RrInputCount; ++i)
        ok = ok && CreateTarget(m_rrInputs[i], width, height, kRrFormats[i]);
    ok = ok && CreateTarget(m_rrSpecularAlbedo, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_rrRoughness, width, height, DXGI_FORMAT_R16_FLOAT);
    ok = ok && CreateTarget(m_rrHitDistance, width, height, DXGI_FORMAT_R16_FLOAT);
    ok = ok && CreateTarget(m_rrOutput, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_rayReconstruction.ReleaseFeature();
    m_width = width;
    m_height = height;
    m_historyValid = false;
    m_volumetricHistoryValid = false;
    m_lastDenoised = nullptr;
    return ok;
}

void Lighting::Use(Texture& texture, D3D12_RESOURCE_STATES state)
{
    if (texture.state == state)
        return;
    D3D12_RESOURCE_BARRIER barrier = Transition(texture.resource.Get(), texture.state, state);
    m_list->ResourceBarrier(1, &barrier);
    texture.state = state;
}

void Lighting::Dispatch(Pass pass, const PassArgs& args)
{
    static_assert(sizeof(PassArgs) == 12 * sizeof(UINT));
    m_list->SetPipelineState(m_pipelines[static_cast<size_t>(pass)].Get());
    m_list->SetComputeRoot32BitConstants(2, sizeof(PassArgs) / sizeof(UINT), &args, 0);
    if (m_rectDispatch)
        m_list->ExecuteIndirect(m_dispatchSignature.Get(), 1, m_viewRect.resource.Get(), 16, nullptr, 0);
    else
        m_list->Dispatch((m_gridWidth + 7) / 8, (m_gridHeight + 7) / 8, 1);
}

void Lighting::DispatchGroups(Pass pass, const PassArgs& args, UINT x, UINT y, UINT z)
{
    ID3D12PipelineState* micromapPipeline = m_micromapPipelines[static_cast<size_t>(pass)].Get();
    m_list->SetPipelineState(m_useMicromaps && micromapPipeline != nullptr ? micromapPipeline : m_pipelines[static_cast<size_t>(pass)].Get());
    m_list->SetComputeRoot32BitConstants(2, sizeof(PassArgs) / sizeof(UINT), &args, 0);
    m_list->Dispatch(x, y, z);
}

bool Lighting::EnsureDepthPipeline(DXGI_FORMAT format)
{
    if (m_depthPipeline && m_depthPipelineFormat == format)
        return true;
    m_depthPipeline.Reset();
    m_depthPipelineFormat = format;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_compositeRoot.Get();
    desc.VS = {g_compositeVS, sizeof(g_compositeVS)};
    desc.PS = {g_compositeDepthPS, sizeof(g_compositeDepthPS)};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.DSVFormat = format;
    desc.SampleDesc.Count = 1;
    if (FAILED(m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_depthPipeline)))) {
        log::Info("Lighting: creating the depth pipeline for format {} failed; translucent surfaces keep the jittered depth", static_cast<int>(format));
        return false;
    }
    return true;
}

void Lighting::UpdateSway(const Scene& scene, UINT slot, const gbuffer::SwayClock& clock, const float (&eye)[3], D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    if (m_sway.Empty() || !clock.known)
        return;
    // The game's sway time runs with real time; it is extrapolated for frames that drew no swaying model.
    LARGE_INTEGER now;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float time = clock.time + static_cast<float>(static_cast<double>(now.QuadPart - clock.ticks) / static_cast<double>(frequency.QuadPart));
    if (!m_swayKnown) {
        m_swayTime = time;
        std::copy_n(clock.wind, 2, m_swayWind);
        log::Info("Lighting: swaying props follow the game's wind ({:.2f} {:.2f}) from time {:.2f}", clock.wind[0], clock.wind[1], time);
    }
    if (const std::optional<SwayingProps::Jobs> jobs = m_sway.BeginUpdate(m_list.Get(), scene, slot, eye, time, m_swayTime, clock.wind, m_swayWind)) {
        ID3D12DescriptorHeap* heaps[] = {m_descriptors->Heap()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetComputeRootSignature(m_computeRoot.Get());
        m_list->SetComputeRootConstantBufferView(0, frameConstants);
        // The pass traces nothing, so the scene's root slot holds any valid address.
        m_list->SetComputeRootShaderResourceView(1, frameConstants);
        DispatchGroups(Pass::Sway, {.input0 = jobs->jobsView, .input1 = jobs->materialsView, .output0 = jobs->currentUav, .output1 = jobs->previousUav},
            jobs->groupsX, jobs->count, 1);
        m_sway.FinishUpdate(m_list.Get(), scene);
    }
    m_swayKnown = true;
    m_swayTime = time;
    std::copy_n(clock.wind, 2, m_swayWind);
}

void Lighting::PublishVolume(UINT slot)
{
    const UINT* grid = gbuffer::kProbeGrid;
    const size_t texels = static_cast<size_t>(grid[0]) * grid[1] * grid[2];
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, static_cast<SIZE_T>(m_volumeReadback[slot]->GetDesc().Width)};
    if (FAILED(m_volumeReadback[slot]->Map(0, &range, &mapped)))
        return;
    // Interleaved per texel: red, green and blue RGBA16F texels, each four halves.
    m_volumeTexels.resize(texels * 12);
    for (size_t channel = 0; channel < m_volumeFootprints.size(); ++channel) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = m_volumeFootprints[channel];
        const auto* base = static_cast<const std::byte*>(mapped) + footprint.Offset;
        for (UINT z = 0; z < grid[2]; ++z) {
            for (UINT y = 0; y < grid[1]; ++y) {
                const std::byte* row = base + (static_cast<size_t>(z) * footprint.Footprint.Height + y) * footprint.Footprint.RowPitch;
                for (UINT x = 0; x < grid[0]; ++x)
                    std::memcpy(m_volumeTexels.data() + ((static_cast<size_t>(z) * grid[1] + y) * grid[0] + x) * 12 + channel * 4, row + x * 8, 8);
            }
        }
    }
    const D3D12_RANGE written = {0, 0};
    m_volumeReadback[slot]->Unmap(0, &written);
    light_queries::Publish(m_volumeReadbackOrigin[slot], {grid[0], grid[1], grid[2]}, gbuffer::kProbeSpacing, m_volumeTexels);
}

void Lighting::WaitForGpu()
{
    if (!m_fence)
        return;
    m_queue->Signal(m_fence.Get(), ++m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

bool Lighting::FillConstants(FrameConstants& constants, const Scene& scene, const ViewInputs& inputs)
{
    constants = {};
    std::memcpy(constants.viewProj, inputs.viewProj.data(), sizeof(constants.viewProj));
    // The depth was drawn with the jittered projection; reconstructing with the plain one shifts positions by up to
    // half a pixel, which puts points by a crease behind the other surface.
    float jittered[4][4];
    std::memcpy(jittered, constants.viewProj, sizeof(jittered));
    const std::array<float, 2>& jitter = inputs.jitter;
    for (int c = 0; c < 4; ++c) {
        jittered[0][c] += jitter[0] * constants.viewProj[3][c];
        jittered[1][c] += jitter[1] * constants.viewProj[3][c];
    }
    if (!Invert(jittered, constants.invViewProj))
        return false;
    // The eye is the point the projection sends to w = 0: the inverse maps clip-space (0, 0, 1, 0) to it.
    const float (*inv)[4] = constants.invViewProj;
    if (std::fabs(inv[3][2]) < 1e-12f)
        return false;
    for (int i = 0; i < 3; ++i)
        constants.eyePosition[i] = inv[i][2] / inv[3][2];
    const bsp::SunLight& sun = scene.Sun();
    constants.sunEnabled = sun.present ? 1.0f : 0.0f;
    float length = std::sqrt(sun.direction.x * sun.direction.x + sun.direction.y * sun.direction.y + sun.direction.z * sun.direction.z);
    if (length > 0.0f) {
        constants.sunDirection[0] = sun.direction.x / length;
        constants.sunDirection[1] = sun.direction.y / length;
        constants.sunDirection[2] = sun.direction.z / length;
    }
    constants.sunColor[0] = sun.color.x;
    constants.sunColor[1] = sun.color.y;
    constants.sunColor[2] = sun.color.z;
    constants.skyColor[0] = sun.ambient.x;
    constants.skyColor[1] = sun.ambient.y;
    constants.skyColor[2] = sun.ambient.z;
    constants.skyCameraX = sun.skyCameraOrigin.x;
    constants.skyCameraY = sun.skyCameraOrigin.y;
    constants.skyCameraZ = sun.skyCameraOrigin.z;
    constants.worldToSky = sun.skyCameraScale > 0.0f ? 1.0f / sun.skyCameraScale : 0.0f;
    const Settings& settings = GetSettings();
    const float sunStyle = scene.LightStyle(sun.style);
    const float skyStyle = scene.LightStyle(sun.ambientStyle);
    for (int i = 0; i < 3; ++i) {
        constants.sunColor[i] *= settings.sun ? settings.sunIntensity * sunStyle : 0.0f;
        constants.skyColor[i] *= settings.skyIntensity * skyStyle;
    }
    constants.lightScale = settings.lightIntensity;
    constants.indirectScale = settings.indirectLight ? settings.indirectIntensity : 0.0f;
    constants.taaHistoryWeight = settings.taaHistoryWeight;
    constants.sharpening = settings.sharpening;
    constants.tonemapScale = inputs.tonemapScale;
    constants.outputSrgb = inputs.outputSrgb ? 1u : 0u;
    constants.invSize[0] = 1.0f / static_cast<float>(m_width);
    constants.invSize[1] = 1.0f / static_cast<float>(m_height);
    constants.size[0] = m_width;
    constants.size[1] = m_height;
    constants.normalBias = 2.0f;
    constants.frameIndex = static_cast<uint32_t>(inputs.frame);
    constants.debugView = static_cast<uint32_t>(settings.debugView);
    constants.materialsView = scene.MaterialsView();
    constants.reflectionScale = settings.reflections ? settings.reflectionIntensity : 0.0f;
    constants.exactLights = static_cast<uint32_t>(settings.exactLights);
    constants.sampledLightRays = static_cast<uint32_t>(settings.sampledLightRays);
    constants.indirectBounces = static_cast<uint32_t>(settings.indirectBounces);
    constants.aoRays = static_cast<uint32_t>(settings.aoRays);
    constants.volumeRays = static_cast<uint32_t>(settings.volumeRays);
    constants.volumetricSteps = static_cast<uint32_t>(settings.volumetricSteps);
    constants.indirectRayFlags = m_useMicromaps && settings.fastAlphaTests ? D3D12_RAY_FLAG_FORCE_OMM_2_STATE : D3D12_RAY_FLAG_NONE;
    constants.dispatchOriginView = inputs.kind == gbuffer::ViewKind::Viewmodel ? m_viewRect.srv : kInvalidView;
    constants.bakedTrianglesView = kInvalidView;
    constants.bakedFacesView = kInvalidView;
    constants.bakedResidualView = kInvalidView;
    constants.leafAmbientView = kInvalidView;
    constants.ambientSamplesView = kInvalidView;
    const bool volumeLight = inputs.kind == gbuffer::ViewKind::Viewmodel && m_volumeReady;
    constants.probeRedView = volumeLight ? m_probeVolume[0].srv : kInvalidView;
    constants.probeGreenView = volumeLight ? m_probeVolume[1].srv : kInvalidView;
    constants.probeBlueView = volumeLight ? m_probeVolume[2].srv : kInvalidView;
    constants.depthMin = inputs.depthMin;
    constants.depthScale = inputs.depthMax - inputs.depthMin;
    const gbuffer::Fog& fog = inputs.fog;
    constants.fogEnabled = fog.known && settings.fog ? 1u : 0u;
    std::memcpy(constants.fogParams, fog.params.data(), sizeof(constants.fogParams));
    std::memcpy(constants.fogColor, fog.color.data(), sizeof(constants.fogColor));
    const gbuffer::Fog& skyboxFog = inputs.skyboxFog;
    constants.skyboxFogEnabled = skyboxFog.known && settings.fog ? 1u : 0u;
    std::memcpy(constants.skyboxFogParams, skyboxFog.params.data(), sizeof(constants.skyboxFogParams));
    std::memcpy(constants.skyboxFogColor, skyboxFog.color.data(), sizeof(constants.skyboxFogColor));
    constants.skyCubeView = inputs.skyCube != nullptr ? inputs.skyCubeView : kInvalidView;
    constants.skyLightView = m_skyLightReady ? m_skyLight.srv : kInvalidView;
    // The 3D skybox is a scaled-down scene elsewhere in the map: the map's lights do not belong to it.
    const bool skybox = inputs.kind == gbuffer::ViewKind::Skybox;
    constants.lightCount = settings.mapLights && !skybox ? scene.LightCount() : 0u;
    constants.lightsView = scene.LightsView();
    const Scene::LightLists& lists = scene.Lights();
    constants.lightRangesView = lists.rangesView;
    constants.lightIndicesView = lists.indicesView;
    constants.bspNodesView = lists.nodesView;
    constants.leafClustersView = lists.leafClustersView;
    constants.sunClustersView = lists.sunClustersView;
    constants.lightWarpCount = static_cast<uint32_t>(std::min(inputs.lightWarps.size(), std::size(constants.lightWarpViews)));
    std::copy_n(inputs.lightWarps.begin(), constants.lightWarpCount, constants.lightWarpViews);
    constants.clusterCount = lists.clusterCount;
    std::memcpy(constants.gridOrigin, &lists.gridOrigin, sizeof(constants.gridOrigin));
    constants.gridCellSize = lists.gridCellSize;
    std::copy_n(lists.gridDims, 3, constants.gridDims);
    constants.gridCellsView = lists.gridCellsView;
    constants.gridIndicesView = lists.gridIndicesView;
    constants.gridTailView = lists.gridTailView;
    constants.emissiveLighting = static_cast<uint32_t>(settings.emissiveLighting);
    constants.emittersView = scene.EmittersView();
    constants.emitterCount = settings.emissiveLighting == EmissiveLighting::Full && !skybox ? scene.EmitterCount() : 0u;
    constants.viewmodel = inputs.kind == gbuffer::ViewKind::Viewmodel ? 1u : 0u;
    // The skybox view shares the main view's orientation, so it finds last frame's sky (reflections.hlsl).
    if (inputs.kind != gbuffer::ViewKind::Viewmodel) {
        constants.historyValid = m_historyValid && inputs.frame - m_lastMainFrame <= 2 ? 1u : 0u;
        std::memcpy(constants.prevViewProj, m_prevViewProj, sizeof(constants.prevViewProj));
        std::memcpy(constants.prevEyePosition, m_prevEye, sizeof(constants.prevEyePosition));
    }
    return true;
}

UINT Lighting::WriteProjectors(UINT slot, std::span<const gbuffer::ProjectedLight> projected, std::span<ID3D12Resource* const> cookies, UINT cookieViews,
    const FrameConstants& constants, const std::optional<std::array<float, 3>>& localPlayer)
{
    const float scale = GetSettings().projectedIntensity;
    const float* forwardRow = constants.viewProj[3];
    const float forwardLength = std::sqrt(Dot(forwardRow, forwardRow));
    const float viewForward[3] = {forwardRow[0] / forwardLength, forwardRow[1] / forwardLength, forwardRow[2] / forwardLength};
    auto* out = reinterpret_cast<GpuProjector*>(m_projectorsMapped) + slot * kMaxProjectors;
    UINT count = 0;
    for (size_t i = 0; i < projected.size() && count < kMaxProjectors; ++i) {
        const gbuffer::ProjectedLight& light = projected[i];
        GpuProjector& gpu = out[count];
        std::memcpy(gpu.worldToTexture, light.worldToTexture.data(), sizeof(gpu.worldToTexture));
        std::memcpy(gpu.origin, light.origin.data(), sizeof(gpu.origin));
        gpu.farZ = light.farZ;
        for (int c = 0; c < 3; ++c)
            gpu.color[c] = light.color[c] * scale;
        std::memcpy(gpu.attenuation, light.attenuation.data(), sizeof(gpu.attenuation));
        gpu.fullDistance = FullStrengthDistance(light.attenuation);
        gpu.flags = 0;
        if (forwardLength > 0.0f) {
            if (const std::optional<float> pullBack = FlashlightPullBack(gpu, constants.eyePosition, viewForward)) {
                AdvanceApex(gpu, *pullBack);
                gpu.flags = kProjectorLocalFlashlight;
            }
        }
        if (gpu.flags == 0 && localPlayer) {
            const float centre[3] = {(*localPlayer)[0], (*localPlayer)[1], (*localPlayer)[2] + kPlayerCentreHeight};
            const float offset[3] = {gpu.origin[0] - centre[0], gpu.origin[1] - centre[1], gpu.origin[2] - centre[2]};
            if (Dot(offset, offset) <= kThirdPersonFlashlightReach * kThirdPersonFlashlightReach)
                gpu.flags = kProjectorLocalFlashlight;
        }
        gpu.cookieView = kInvalidView;
        if (i < cookies.size() && cookies[i] != nullptr) {
            const D3D12_RESOURCE_DESC desc = cookies[i]->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = light.cookieSrgb ? SrgbViewFormat(desc.Format) : ColorViewFormat(desc.Format);
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = desc.MipLevels;
            m_device->CreateShaderResourceView(cookies[i], &srv, m_descriptors->Cpu(cookieViews + count));
            gpu.cookieView = cookieViews + count;
        }
        ++count;
    }
    return count;
}

void Lighting::BakeStep(Scene& scene, const FrameConstants& constants, D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    BakedBuffers& baked = scene.Baked();
    // The lights at the strength VRAD baked them: style-0 lights alone (the shader skips the others), no settings.
    FrameConstants& bake = *reinterpret_cast<FrameConstants*>(m_bakeConstantsMapped);
    bake = constants;
    bake.lightScale = 1.0f;
    bake.lightStylesView = kInvalidView;
    bake.lightCount = scene.LightCount();
    const bsp::SunLight& sun = scene.Sun();
    bake.sunEnabled = sun.present && sun.style == 0 ? 1.0f : 0.0f;
    bake.sunColor[0] = sun.color.x;
    bake.sunColor[1] = sun.color.y;
    bake.sunColor[2] = sun.color.z;
    m_list->SetComputeRootConstantBufferView(0, m_bakeConstants->GetGPUVirtualAddress());
    const D3D12_RESOURCE_BARRIER countersBarrier = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {scene.BakedCounters()}}};
    // Until the gather, the filtered buffer holds each sample's direct light and the residual buffer the supersampling
    // state.
    const D3D12_RESOURCE_BARRIER bakeBarriers[] = {
        {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {scene.BakedFiltered()}}},
        {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {scene.BakedResidual()}}},
    };
    const D3D12_RESOURCE_BARRIER& directBarrier = bakeBarriers[0];
    if (baked.stage == BakedBuffers::Stage::Direct || baked.stage == BakedBuffers::Stage::Supersample) {
        const bool direct = baked.stage == BakedBuffers::Stage::Direct;
        if (baked.bakedLuxels == 0)
            m_list->ResourceBarrier(2, bakeBarriers);
        const UINT count = std::min(kBakeLuxelsPerView, baked.luxelCount - baked.bakedLuxels);
        DispatchGroups(Pass::Baked, {.input0 = baked.luxelsView, .input2 = baked.facesView, .input3 = baked.bakedLuxels, .input4 = count,
            .output0 = baked.filteredUav, .output1 = baked.residualUav, .output2 = baked.countersUav, .step = direct ? 0u : 5u}, (count + 63) / 64, 1, 1);
        baked.bakedLuxels += count;
        if (baked.bakedLuxels == baked.luxelCount) {
            baked.bakedLuxels = 0;
            baked.stage = direct || ++baked.supersamplePass < kSupersamplePasses ? BakedBuffers::Stage::Mark : BakedBuffers::Stage::Gather;
        }
    } else if (baked.stage == BakedBuffers::Stage::Mark) {
        m_list->ResourceBarrier(2, bakeBarriers);
        for (UINT first = 0; first < baked.luxelCount; first += kMaxGroups * 64) {
            const UINT count = std::min(kMaxGroups * 64, baked.luxelCount - first);
            DispatchGroups(Pass::Baked, {.input0 = baked.luxelsView, .input2 = baked.facesView, .input3 = first, .input4 = count,
                .output0 = baked.filteredUav, .output1 = baked.residualUav, .step = 4}, (count + 63) / 64, 1, 1);
        }
        baked.stage = BakedBuffers::Stage::Supersample;
    } else {
        m_list->ResourceBarrier(2, bakeBarriers);
        m_list->ResourceBarrier(1, &countersBarrier);
        for (UINT first = 0; first < baked.luxelCount; first += kMaxGroups * 64) {
            const UINT count = std::min(kMaxGroups * 64, baked.luxelCount - first);
            DispatchGroups(Pass::Baked, {.input0 = baked.luxelsView, .input1 = baked.samplesView, .input2 = baked.facesView, .input3 = first, .input4 = count,
                .output0 = baked.residualUav, .output1 = baked.countersUav, .output2 = baked.filteredUav, .output3 = baked.neighboursView, .step = 3},
                (count + 63) / 64, 1, 1);
        }
        const D3D12_RESOURCE_BARRIER gathered[] = {
            Transition(scene.BakedResidual(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            directBarrier,
            countersBarrier,
        };
        m_list->ResourceBarrier(3, gathered);
        for (UINT first = 0; first < baked.luxelCount; first += kMaxGroups * 64) {
            const UINT count = std::min(kMaxGroups * 64, baked.luxelCount - first);
            DispatchGroups(Pass::Baked, {.input0 = baked.luxelsView, .input1 = baked.residualView, .input2 = baked.facesView, .input3 = first, .input4 = count,
                .output0 = baked.filteredUav, .output1 = baked.countersUav, .step = 1}, (count + 63) / 64, 1, 1);
        }
        ID3D12Resource* ambient = scene.BakedAmbient();
        if (ambient != nullptr) {
            for (UINT first = 0; first < baked.ambientSampleCount; first += kMaxGroups * 64) {
                const UINT count = std::min(kMaxGroups * 64, baked.ambientSampleCount - first);
                DispatchGroups(Pass::Baked, {.input0 = baked.ambientLightsView, .input1 = baked.ambientLightCount, .input3 = first, .input4 = count,
                    .output0 = baked.ambientSamplesUav, .output1 = baked.countersUav, .step = 2}, (count + 63) / 64, 1, 1);
            }
        }
        D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(scene.BakedFiltered(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(scene.BakedCounters(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(ambient, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        m_list->ResourceBarrier(ambient != nullptr ? 3 : 2, barriers);
        const UINT64 counterBytes = scene.BakedCounters()->GetDesc().Width;
        if (!m_bakeReadback || m_bakeReadback->GetDesc().Width < counterBytes)
            m_bakeReadback = CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, counterBytes, D3D12_RESOURCE_STATE_COPY_DEST);
        m_list->CopyBufferRegion(m_bakeReadback.Get(), 0, scene.BakedCounters(), 0, counterBytes);
        if (!baked.cachePath.empty()) {
            const UINT64 residualBytes = scene.BakedFiltered()->GetDesc().Width;
            const UINT64 ambientBytes = ambient != nullptr ? ambient->GetDesc().Width : 0;
            m_bakeCacheReadback = CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, residualBytes + ambientBytes, D3D12_RESOURCE_STATE_COPY_DEST);
            if (m_bakeCacheReadback) {
                m_list->CopyBufferRegion(m_bakeCacheReadback.Get(), 0, scene.BakedFiltered(), 0, residualBytes);
                if (ambient != nullptr)
                    m_list->CopyBufferRegion(m_bakeCacheReadback.Get(), residualBytes, ambient, 0, ambientBytes);
            }
        }
        D3D12_RESOURCE_BARRIER readable[] = {
            Transition(scene.BakedFiltered(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(ambient, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        m_list->ResourceBarrier(ambient != nullptr ? 2 : 1, readable);
        baked.filtered = true;
        m_bakeLogPending = true;
    }
    m_list->SetComputeRootConstantBufferView(0, frameConstants);
}

bool Lighting::RayReconstructionSize(UINT outputWidth, UINT outputHeight, UINT& width, UINT& height)
{
    const Settings& settings = GetSettings();
    if (settings.antiAliasing != AntiAliasing::RayReconstruction || settings.debugView != DebugView::Final || !m_rayReconstruction.Usable())
        return false;
    if (m_rrSizeMode != settings.upscaling || m_rrSize[0] != outputWidth || m_rrSize[1] != outputHeight) {
        if (!m_rayReconstruction.RenderSize(settings.upscaling, outputWidth, outputHeight, m_rrSize[2], m_rrSize[3]))
            return false;
        m_rrSizeMode = settings.upscaling;
        m_rrSize[0] = outputWidth;
        m_rrSize[1] = outputHeight;
    }
    width = m_rrSize[2];
    height = m_rrSize[3];
    return true;
}

void Lighting::Run(const ViewInputs& inputs, Scene& scene, std::span<const SceneInstance> dynamic, const std::vector<SkinnedEntity>& skinned)
{
    if (m_failed)
        return;
    const D3D12_RESOURCE_DESC colorDesc = inputs.color->GetDesc();
    if (!EnsureTargets(static_cast<UINT>(colorDesc.Width), colorDesc.Height)) {
        log::Info("Lighting: creating render targets failed; lighting disabled");
        m_failed = true;
        return;
    }

    const UINT slot = static_cast<UINT>(m_run % kSlots);
    if (m_fence->GetCompletedValue() < m_slotFenceValues[slot]) {
        m_fence->SetEventOnCompletion(m_slotFenceValues[slot], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    if (m_volumeReadbackPending[slot]) {
        m_volumeReadbackPending[slot] = false;
        PublishVolume(slot);
    }
    // Timings of the view that last used this slot: the main view's per pass, the others' whole runs.
    if (m_slotFenceValues[slot] != 0 && !m_slotWasMain[slot]) {
        const uint64_t* markers = m_timestampValues + slot * kMarkers;
        if (markers[kPassCount] > markers[0])
            m_otherViewsMsTotal += static_cast<double>(markers[kPassCount] - markers[0]) * m_timestampPeriodMs;
    }
    if (m_slotFenceValues[slot] != 0 && m_slotWasMain[slot]) {
        const uint64_t* markers = m_timestampValues + slot * kMarkers;
        if (markers[kPassCount] > markers[0]) {
            for (UINT pass = 0; pass < kPassCount; ++pass)
                m_passMsTotal[pass] += static_cast<double>(markers[pass + 1] - markers[pass]) * m_timestampPeriodMs;
            if (++m_gpuSamples == kTimingSamples) {
                Statistics& statistics = GetStatistics();
                statistics.lightingMs = 0.0;
                for (UINT pass = 0; pass < kPassCount; ++pass) {
                    statistics.passMs[pass] = m_passMsTotal[pass] / m_gpuSamples;
                    statistics.lightingMs += statistics.passMs[pass];
                }
                statistics.otherViewsMs = m_otherViewsMsTotal / m_gpuSamples;
                statistics.lightingMs += statistics.otherViewsMs;
                m_otherViewsMsTotal = 0.0;
                m_passMsTotal = {};
                m_gpuSamples = 0;
                if (++m_timingReports % (GetOptions().devMode ? 1 : 10) == 0) {
                    constexpr size_t kRecentFrames = 240;
                    const size_t frames = std::min(statistics.frameCount, kRecentFrames);
                    double frameMs = 0.0;
                    for (size_t i = 1; i <= frames; ++i)
                        frameMs += statistics.frameMs[(statistics.nextFrame + statistics.frameMs.size() - i) % statistics.frameMs.size()];
                    const auto& p = statistics.passMs;
                    log::Info("Lighting: {:.3f} ms GPU per frame ({}x{} traced at {}x{}; scene {:.2f}, surface {:.2f}, volume {:.2f}, direct {:.2f}, "
                              "ao {:.2f}, indirect {:.2f}, accumulation {:.2f}, denoiser {:.2f}, reflections {:.2f}, lighting {:.2f}, resolve {:.2f}; "
                              "3D skybox and viewmodel {:.2f}); frame time {:.2f} ms, tonemap scale {:.3f}",
                        statistics.lightingMs, m_width, m_height, statistics.renderWidth, statistics.renderHeight, p[0], p[1], p[2], p[3], p[4],
                        p[5], p[6], p[7], p[8], p[9], p[10], statistics.otherViewsMs, frames ? frameMs / frames : 0.0, inputs.tonemapScale);
                }
            }
        }
    }
    m_slotWasMain[slot] = inputs.kind == gbuffer::ViewKind::Main;
    const bool main = inputs.kind == gbuffer::ViewKind::Main;
    UINT renderWidth = m_width;
    UINT renderHeight = m_height;
    const bool rayReconstruction = main && scene.Ready() && RayReconstructionSize(m_width, m_height, renderWidth, renderHeight);
    if (main) {
        // Histories made at another resolution or by the other pipeline cannot be reused.
        if (rayReconstruction != m_lastMainUsedRr) {
            m_historyValid = false;
            m_taaHistoryValid = false;
        }
        m_lastMainUsedRr = rayReconstruction;
        Statistics& statistics = GetStatistics();
        statistics.renderWidth = renderWidth;
        statistics.renderHeight = renderHeight;
    }
    if (rayReconstruction && m_rayReconstruction.NeedsNewFeature(GetSettings().upscaling, renderWidth, renderHeight, m_width, m_height)) {
        WaitForGpu();
        m_rayReconstruction.ReleaseFeature();
        m_historyValid = false;
    }

    std::byte* slotConstants = m_constantsMapped + slot * kConstantsPerSlot * kConstantsStride;
    const D3D12_GPU_VIRTUAL_ADDRESS slotConstantsGpu = m_constants->GetGPUVirtualAddress() + slot * kConstantsPerSlot * kConstantsStride;
    FrameConstants& constants = *reinterpret_cast<FrameConstants*>(slotConstants);
    // Ray queries that allow opacity micromaps cost a little even where there are none, so maps without them, and the
    // setting that turns them off, use the plain passes; instances then ignore their micromaps (Scene::RecordFrame).
    m_useMicromaps = m_opacityMicromaps && GetSettings().opacityMicromaps && scene.HasMicromaps();
    if (!FillConstants(constants, scene, inputs)) {
        if (++m_cameraFailures % 300 == 1)
            log::Info("Lighting: the camera of view kind {} cannot be inverted; view left unlit ({} times)", static_cast<int>(inputs.kind), m_cameraFailures);
        return;
    }
    // The irradiance volume follows the main view's camera in whole cells.
    const bool volume = main && scene.Ready() && GetSettings().translucentLighting && m_probeVolume[0].resource;
    if (main)
        m_volumeReady = false;
    for (int i = 0; i < 3; ++i) {
        if (volume)
            m_probeOrigin[i] = static_cast<int>(std::floor(constants.eyePosition[i] / gbuffer::kProbeSpacing)) - static_cast<int>(gbuffer::kProbeGrid[i] / 2);
        constants.probeOrigin[i] = m_probeOrigin[i];
    }
    constants.probeSpacing = gbuffer::kProbeSpacing;
    constants.aoView = kInvalidView;
    constants.aoIntensity = GetSettings().aoIntensity;
    constants.volumetricView = kInvalidView;
    // Haze thin enough that light crosses about 6,000 units at the default intensity.
    // 1/6000 per unit washed every distant surface out in the sun (gm_construct, 2026-10-31).
    constexpr float kVolumetricDensity = 1.0f / 20000.0f;
    constants.volumetricDensity = kVolumetricDensity * GetSettings().volumetricIntensity;
    const bool volumetrics = main && GetSettings().volumetricLight && GetSettings().volumetricIntensity > 0.0f;
    // indirect_lighting = 1: VRAD's bounce light replaces traced indirect light once the map's residual is baked.
    const bool bakedIndirect = GetSettings().indirectLighting == IndirectLighting::Baked && scene.HasBaked();
    if (bakedIndirect && scene.Baked().Ready()) {
        const BakedBuffers& baked = scene.Baked();
        constants.bakedTrianglesView = baked.trianglesView;
        constants.bakedFacesView = baked.facesView;
        constants.bakedResidualView = baked.filteredView;
        constants.leafAmbientView = baked.leafAmbientView;
        constants.ambientSamplesView = baked.ambientSamplesView;
    }
    // Ambient occlusion darkens only the baked indirect light (shaders/deferred_lighting.hlsl): traced light holds its
    // own occlusion. The 3D skybox's scaled-down scene has no contact shadows worth tracing.
    const bool ambientOcclusion = GetSettings().ambientOcclusion && inputs.kind != gbuffer::ViewKind::Skybox && constants.bakedTrianglesView != kInvalidView;

    ID3D12CommandQueue* queue = m_queue;
    const bool projectedLights = inputs.kind != gbuffer::ViewKind::Skybox && GetSettings().projectedLights;
    std::vector<ID3D12Resource*> resources = {inputs.color, inputs.depth, inputs.normal, inputs.flags, inputs.light, inputs.albedo, inputs.emissive};
    // Cookie textures of the projected lights, each transitioned once.
    std::vector<ID3D12Resource*> cookies;
    if (projectedLights) {
        for (size_t i = 0; i < inputs.projected.size() && i < kMaxProjectors; ++i) {
            ID3D12Resource* cookie = i < inputs.cookies.size() ? inputs.cookies[i] : nullptr;
            if (cookie != nullptr && std::find(resources.begin() + kInputViews, resources.end(), cookie) == resources.end())
                resources.push_back(cookie);
            cookies.push_back(cookie);
        }
    }
    if (inputs.skyCube != nullptr)
        resources.push_back(inputs.skyCube);
    // The game's probe volume textures, filled by copies at the end of the irradiance volume passes.
    const std::array<ID3D12Resource*, 3>& probeTargets = inputs.probes;
    const bool copyVolume = volume && probeTargets[0] != nullptr && probeTargets[1] != nullptr && probeTargets[2] != nullptr;
    ID3D12Resource* color = inputs.color;
    ID3D12Resource* depth = inputs.depth;
    ID3D12Resource* normal = inputs.normal;
    ID3D12Resource* flags = inputs.flags;
    ID3D12Resource* light = inputs.light;
    ID3D12Resource* albedo = inputs.albedo;
    ID3D12Resource* emissive = inputs.emissive;
    if (!m_loggedFormats) {
        m_loggedFormats = true;
        log::Info("Lighting: color format {}, depth format {}, normal format {}, flags format {}", static_cast<int>(colorDesc.Format),
            static_cast<int>(depth->GetDesc().Format), static_cast<int>(normal->GetDesc().Format), static_cast<int>(flags->GetDesc().Format));
    }

    const UINT views = m_slotViews + slot * kViewsPerSlot;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    m_device->CreateShaderResourceView(m_colorCopy.Get(), &srv, m_descriptors->Cpu(views));
    srv.Format = DepthViewFormat(depth->GetDesc().Format);
    m_device->CreateShaderResourceView(depth, &srv, m_descriptors->Cpu(views + 1));
    srv.Format = ColorViewFormat(normal->GetDesc().Format);
    m_device->CreateShaderResourceView(normal, &srv, m_descriptors->Cpu(views + 2));
    srv.Format = ColorViewFormat(flags->GetDesc().Format);
    m_device->CreateShaderResourceView(flags, &srv, m_descriptors->Cpu(views + 3));
    srv.Format = ColorViewFormat(light->GetDesc().Format);
    m_device->CreateShaderResourceView(light, &srv, m_descriptors->Cpu(views + 4));
    srv.Format = ColorViewFormat(albedo->GetDesc().Format);
    m_device->CreateShaderResourceView(albedo, &srv, m_descriptors->Cpu(views + 5));
    srv.Format = ColorViewFormat(emissive->GetDesc().Format);
    m_device->CreateShaderResourceView(emissive, &srv, m_descriptors->Cpu(views + 6));
    constants.colorView = views;
    constants.depthView = views + 1;
    constants.gbufferNormalView = views + 2;
    constants.flagsView = views + 3;
    constants.lightView = views + 4;
    constants.albedoView = views + 5;
    constants.emissiveView = views + 6;
    if (projectedLights) {
        constants.projectorCount = WriteProjectors(slot, inputs.projected, cookies, views + kInputViews, constants, inputs.localPlayer);
        constants.projectorsView = m_projectorViews[slot];
    }
    // The 3D skybox is drawn before the main view of the same frame, so it may use the surface the main
    // view is about to overwrite; the previous frame's main-view data stays intact.
    const UINT current = static_cast<UINT>(m_mainRuns % 2);
    const UINT previous = 1 - current;
    constants.surfaceView = m_surface[current].srv;
    constants.prevSurfaceView = m_surface[previous].srv;
    constants.previousLitView = main && m_historyValid ? m_lit[previous].srv : kInvalidView;
    constants.visibilityView = m_visibility.srv;
    constants.motionView = m_motion.srv;
    constants.previousDistanceView = m_previousDistance.srv;
    constants.pixelFlagsView = m_pixelFlags.srv;
    constants.geometricNormalView = m_geometricNormal.srv;
    constants.specularView = m_specular.srv;
    const bool probe = main && m_probeRequested && m_probeBuffer;
    constants.probeView = probe ? m_probeUav : kInvalidView;
    constants.probePixel = m_probePixel;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(slot) * m_rtvIncrement;
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = ColorViewFormat(colorDesc.Format);
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_device->CreateRenderTargetView(color, &rtvDesc, rtv);

    m_allocators[slot]->Reset();
    m_list->Reset(m_allocators[slot].Get(), nullptr);
    UINT marker = slot * kMarkers;
    m_list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, marker++);
    {
        D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(normal, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(flags, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(light, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(albedo, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(emissive, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        m_list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
        for (size_t i = kInputViews; i < resources.size(); ++i) {
            D3D12_RESOURCE_BARRIER barrier = Transition(resources[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_list->ResourceBarrier(1, &barrier);
        }
    }
    m_list->CopyResource(m_colorCopy.Get(), color);
    {
        D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(m_colorCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        m_list->ResourceBarrier(2, barriers);
    }

    m_frameInstances.assign(dynamic.begin(), dynamic.end());
    m_skinned.Record(m_list.Get(), scene, skinned, m_frameInstances);
    if (main)
        UpdateSway(scene, slot, inputs.sway, constants.eyePosition, slotConstantsGpu);
    m_sway.Append(m_frameInstances);
    const SceneFrame frame = scene.RecordFrame(m_list.Get(), slot, m_frameInstances, m_useMicromaps);
    constants.instancesView = frame.instancesView;
    constants.dynamicLightsView = frame.dynamicLightsView;
    constants.lightStylesView = frame.lightStylesView;
    constants.dynamicLightCount = GetSettings().dynamicLights && inputs.kind != gbuffer::ViewKind::Skybox ? frame.dynamicLightCount : 0u;
    Statistics& statistics = GetStatistics();
    if (main) {
        statistics.mapLights = constants.lightCount;
        statistics.dynamicLights = constants.dynamicLightCount;
        statistics.projectedLights = constants.projectorCount;
        statistics.sceneInstances = static_cast<uint32_t>(1 + m_frameInstances.size());
        std::tie(statistics.micromapTriangles, statistics.micromaps) = scene.MicromapCounts();
    }
    if (frame.tlas == 0 || (inputs.kind == gbuffer::ViewKind::Skybox && !scene.HasSkybox()))
        constants.traceMask = 0;
    else if (inputs.kind == gbuffer::ViewKind::Skybox)
        constants.traceMask = kSkyboxMask;
    else
        constants.traceMask = inputs.kind == gbuffer::ViewKind::Viewmodel ? kViewmodelTraceMask : kTraceMask;
    const FoliageShadows foliage = GetSettings().foliageShadows;
    const auto withFoliage = [&](UINT mask, FoliageShadows from) { return foliage >= from ? mask : mask & ~kShadowlessPropMask; };
    constants.sunShadowMask = withFoliage(constants.traceMask, FoliageShadows::Sun);
    const UINT indirectMask = constants.traceMask & ~kLocalPlayerMask;
    constants.indirectMask = withFoliage(indirectMask, FoliageShadows::SunAndIndirect);
    constants.bounceMask = withFoliage(indirectMask, FoliageShadows::All);
    constants.lightShadowMask = withFoliage(constants.traceMask, FoliageShadows::All);
    // The baked light holds the occlusion by static geometry, so ambient occlusion counts only what moves.
    if (constants.bakedTrianglesView != kInvalidView)
        constants.indirectMask = constants.traceMask & (kDynamicMask | kLocalPlayerMask);

    auto bindCompute = [&](D3D12_GPU_VIRTUAL_ADDRESS frameConstants) {
        ID3D12DescriptorHeap* heaps[] = {m_descriptors->Heap()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetComputeRootSignature(m_computeRoot.Get());
        m_list->SetComputeRootConstantBufferView(0, frameConstants);
        m_list->SetComputeRootShaderResourceView(1, frame.tlas);
    };
    bindCompute(slotConstantsGpu);
    if (bakedIndirect && main && constants.traceMask != 0 && !scene.Baked().Ready())
        BakeStep(scene, constants, slotConstantsGpu);
    if (main && inputs.skyCube != nullptr && m_skyLight.resource) {
        Use(m_skyLight, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        DispatchGroups(Pass::Sky, {.input0 = inputs.skyFacesView, .output0 = m_skyLight.uav, .step = 0}, kSkyLightSize, kSkyLightSize, 6);
        const D3D12_RESOURCE_BARRIER between = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_skyLight.resource.Get()}}};
        m_list->ResourceBarrier(1, &between);
        DispatchGroups(Pass::Sky, {.output0 = m_skyLight.uav, .step = 1}, 1, 1, 1);
        Use(m_skyLight, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_skyLightReady = true;
        constants.skyLightView = m_skyLight.srv;
    } else if (main) {
        // Another map's sky, until this one's is drawn.
        m_skyLightReady = false;
        constants.skyLightView = kInvalidView;
    }
    m_gridWidth = m_width;
    m_gridHeight = m_height;
    // Additive draws (glows, sprites) skip the temporal resolve, which would jitter them and, with Ray Reconstruction,
    // denoise them at render resolution as if they were lit surfaces.
    const bool temporalResolve = rayReconstruction || (main && GetSettings().antiAliasing == AntiAliasing::Temporal);
    constants.additiveAfterResolve = temporalResolve ? 1u : 0u;
    // The jitter's image shift in output pixels.
    const float jitterScale = rayReconstruction ? static_cast<float>(m_width) / static_cast<float>(renderWidth) : 1.0f;
    const UINT shiftX = std::bit_cast<UINT>(inputs.jitterX * jitterScale);
    const UINT shiftY = std::bit_cast<UINT>(inputs.jitterY * jitterScale);

    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto mark = [&] { m_list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, marker++); };
    if (rayReconstruction) {
        // The passes before Ray Reconstruction run at render resolution on copies of the G-buffer.
        for (Texture& texture : m_rrInputs)
            Use(texture, write);
        Dispatch(Pass::DlssInputs, {.input0 = renderWidth, .input1 = renderHeight, .output0 = m_rrInputUavs});
        for (Texture& texture : m_rrInputs)
            Use(texture, read);
        FrameConstants& render = *reinterpret_cast<FrameConstants*>(slotConstants + kConstantsStride);
        render = constants;
        render.size[0] = renderWidth;
        render.size[1] = renderHeight;
        render.invSize[0] = 1.0f / static_cast<float>(renderWidth);
        render.invSize[1] = 1.0f / static_cast<float>(renderHeight);
        render.colorView = m_rrInputs[RrColor].srv;
        render.depthView = m_rrInputs[RrDepth].srv;
        render.gbufferNormalView = m_rrInputs[RrNormal].srv;
        render.flagsView = m_rrInputs[RrFlags].srv;
        render.lightView = m_rrInputs[RrLight].srv;
        render.albedoView = m_rrInputs[RrAlbedo].srv;
        render.emissiveView = m_rrInputs[RrEmissive].srv;
        render.outputLinear = 1;
        m_list->SetComputeRootConstantBufferView(0, slotConstantsGpu + kConstantsStride);
        m_gridWidth = renderWidth;
        m_gridHeight = renderHeight;
    }
    // The viewmodel covers a small part of the screen; its passes run over a rectangle around it.
    if (inputs.kind == gbuffer::ViewKind::Viewmodel) {
        Use(m_viewRect, write);
        const D3D12_RESOURCE_BARRIER between = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_viewRect.resource.Get()}}};
        DispatchGroups(Pass::ViewRect, {.output0 = m_viewRect.uav, .step = 0}, 1, 1, 1);
        m_list->ResourceBarrier(1, &between);
        Dispatch(Pass::ViewRect, {.output0 = m_viewRect.uav, .step = 1});
        m_list->ResourceBarrier(1, &between);
        DispatchGroups(Pass::ViewRect, {.output0 = m_viewRect.uav, .step = 2}, 1, 1, 1);
        Use(m_viewRect, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_rectDispatch = true;
    }
    mark();
    Use(m_surface[current], write);
    Dispatch(Pass::Surface, {.output0 = m_surface[current].uav});
    Use(m_surface[current], read);
    Use(m_surface[previous], read);
    for (Texture* texture : {&m_visibility, &m_motion, &m_previousDistance, &m_pixelFlags, &m_geometricNormal})
        Use(*texture, write);
    Dispatch(Pass::Primary, {.output0 = m_visibility.uav, .output1 = m_motion.uav, .output2 = m_previousDistance.uav, .output3 = m_pixelFlags.uav,
        .output4 = m_geometricNormal.uav});
    for (Texture* texture : {&m_visibility, &m_motion, &m_previousDistance, &m_pixelFlags, &m_geometricNormal})
        Use(*texture, read);
    mark();
    if (copyVolume && constants.traceMask != 0) {
        auto& last = m_probeHistory[previous];
        auto& next = m_probeHistory[current];
        for (Texture& texture : last)
            Use(texture, read);
        for (Texture& texture : next)
            Use(texture, write);
        // A thread group of 32 per probe to trace, a thread per probe to fill (shaders/probes.hlsl).
        const UINT probes = gbuffer::kProbeGrid[0] * gbuffer::kProbeGrid[1] * gbuffer::kProbeGrid[2];
        if (constants.bakedTrianglesView != kInvalidView) {
            for (Texture& texture : m_probeVolume)
                Use(texture, write);
            for (Texture& texture : m_probeIndirect)
                if (texture.resource)
                    Use(texture, write);
            DispatchGroups(Pass::Probes, {.output0 = m_probeVolume[0].uav, .output1 = m_probeVolume[1].uav, .output2 = m_probeVolume[2].uav,
                .output3 = m_probeIndirect[0].resource ? m_probeIndirect[0].uav : kInvalidView, .step = 2},
                probes / 32, 1, 1);
        } else {
            DispatchGroups(Pass::Probes, {.input0 = last[0].srv, .input1 = last[1].srv, .input2 = last[2].srv, .input3 = last[3].srv,
                .input4 = m_probeHistoryValid ? 1u : 0u, .output0 = next[0].uav, .output1 = next[1].uav, .output2 = next[2].uav, .output3 = next[3].uav,
                .step = 0}, probes, 1, 1);
            for (Texture& texture : next)
                Use(texture, read);
            for (Texture& texture : m_probeVolume)
                Use(texture, write);
            for (Texture& texture : m_probeIndirect)
                if (texture.resource)
                    Use(texture, write);
            DispatchGroups(Pass::Probes, {.input0 = next[0].srv, .input1 = next[1].srv, .input2 = next[2].srv, .input3 = next[3].srv,
                .output0 = m_probeVolume[0].uav, .output1 = m_probeVolume[1].uav, .output2 = m_probeVolume[2].uav,
                .output3 = m_probeIndirect[0].resource ? m_probeIndirect[0].uav : kInvalidView, .step = 1}, probes / 32, 1, 1);
        }
        for (size_t i = 0; i < probeTargets.size(); ++i) {
            Use(m_probeVolume[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_RESOURCE_BARRIER barrier = Transition(probeTargets[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            m_list->ResourceBarrier(1, &barrier);
            m_list->CopyResource(probeTargets[i], m_probeVolume[i].resource.Get());
            barrier = Transition(probeTargets[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            m_list->ResourceBarrier(1, &barrier);
            if (m_volumeReadback[slot]) {
                D3D12_TEXTURE_COPY_LOCATION destination = {m_volumeReadback[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                destination.PlacedFootprint = m_volumeFootprints[i];
                D3D12_TEXTURE_COPY_LOCATION source = {m_probeVolume[i].resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
        }
        m_volumeReadbackPending[slot] = m_volumeReadback[slot] != nullptr;
        m_volumeReadbackOrigin[slot] = m_probeOrigin;
        // The baked fill leaves the traced history behind.
        m_probeHistoryValid = constants.bakedTrianglesView == kInvalidView;
        m_volumeReady = true;
    }
    mark();
    Texture& direct = m_direct[current];
    Use(direct, write);
    Use(m_directRemainder, write);
    Use(m_specular, write);
    const bool wideLightSets = constants.exactLights > 4 || constants.sampledLightRays > 2;
    Dispatch(wideLightSets ? Pass::DirectWide : Pass::Direct, {.output0 = direct.uav, .output1 = m_directRemainder.uav, .output2 = m_specular.uav});
    Use(m_specular, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (probe) {
        D3D12_RESOURCE_BARRIER barrier = Transition(m_probeBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_list->ResourceBarrier(1, &barrier);
        m_list->CopyResource(m_probeReadback.Get(), m_probeBuffer.Get());
        barrier = Transition(m_probeBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_list->ResourceBarrier(1, &barrier);
    }
    Use(direct, read);
    Use(m_directRemainder, read);
    Use(m_direct[previous], read);
    mark();
    if (ambientOcclusion && constants.traceMask != 0) {
        Use(m_aoHistory[previous], read);
        Use(m_aoHistory[current], write);
        Dispatch(Pass::Ao, {.input0 = main ? m_aoHistory[previous].srv : kInvalidView, .output0 = m_aoHistory[current].uav, .step = 0});
        Use(m_aoHistory[current], read);
        Use(m_ao, write);
        Dispatch(Pass::Ao, {.input0 = m_aoHistory[current].srv, .output0 = m_ao.uav, .step = 1});
        Use(m_ao, read);
        constants.aoView = m_ao.srv;
        if (rayReconstruction)
            reinterpret_cast<FrameConstants*>(slotConstants + kConstantsStride)->aoView = m_ao.srv;
    }
    mark();
    PassArgs trace = {.input1 = m_directRemainder.srv, .input2 = m_direct[previous].srv, .output0 = m_giRaw.uav};
    Texture* indirect = nullptr;
    // Untraced views and the distant 3D skybox (traced for reflections and sun shadows only) take the sky's
    // light as their indirect light.
    if (constants.traceMask == 0 || inputs.kind == gbuffer::ViewKind::Skybox) {
        mark();
        mark();
        mark();
    } else if (!main) {
        if (constants.probeRedView != kInvalidView) {
            for (Texture& texture : m_probeVolume)
                Use(texture, read);
        }
        Use(m_giRaw, write);
        trace.step = kViewmodelIndirectSamples;
        Dispatch(Pass::GiTrace, trace);
        mark();
        mark();
        Texture* source = &m_giRaw;
        for (size_t i = 0; i < std::size(kFilterSteps) && GetSettings().denoiser; ++i) {
            Texture* target = &m_viewmodelIndirect[i % 2];
            Use(*source, read);
            Use(*target, write);
            Dispatch(Pass::GiFilter, {.input0 = source->srv, .input2 = i == 0 ? 1u : 0u, .output0 = target->uav, .step = kFilterSteps[i]});
            source = target;
        }
        indirect = source;
        mark();
    } else {
        if (m_lastDenoised != nullptr) {
            Use(*m_lastDenoised, read);
            trace.input0 = m_lastDenoised->srv;
        }
        trace.step = static_cast<UINT>(GetSettings().indirectRays);
        Use(m_giRaw, write);
        Dispatch(Pass::GiTrace, trace);
        mark();
        Use(m_giRaw, read);
        Use(m_giHistory[previous], read);
        Use(m_giMoments[previous], read);
        Use(m_giHistory[current], write);
        Use(m_giMoments[current], write);
        Dispatch(Pass::GiTemporal, {.input0 = m_giRaw.srv, .input1 = m_giHistory[previous].srv, .input2 = m_giMoments[previous].srv,
            .output0 = m_giHistory[current].uav, .output1 = m_giMoments[current].uav});
        Use(m_giMoments[current], read);
        mark();
        Texture* source = &m_giHistory[current];
        // Ray Reconstruction denoises the raw estimate itself; the accumulation still serves indirect rays and
        // reflections.
        for (size_t i = 0; i < std::size(kFilterSteps) && GetSettings().denoiser && !rayReconstruction; ++i) {
            Texture* target = &m_giFiltered[i % 2];
            Use(*source, read);
            Use(*target, write);
            Dispatch(Pass::GiFilter, {.input0 = source->srv, .input1 = i == 0 ? m_giMoments[current].srv : kInvalidView, .input2 = i == 0 ? 1u : 0u,
                .output0 = target->uav, .step = kFilterSteps[i]});
            source = target;
        }
        indirect = source;
        mark();
    }
    if (main)
        m_lastDenoised = indirect;
    Texture* shaded = indirect != nullptr && rayReconstruction ? &m_giRaw : indirect;
    UINT reflections = kInvalidView;
    if (constants.traceMask != 0) {
        Use(m_lit[previous], read);
        Use(m_reflections, write);
        Use(m_direct[current], read);
        PassArgs args = {.input0 = m_lit[previous].srv, .input2 = m_direct[current].srv, .output0 = m_reflections.uav};
        if (indirect != nullptr) {
            Use(*indirect, read);
            args.input1 = indirect->srv;
        }
        if (rayReconstruction) {
            Use(m_rrSpecularAlbedo, write);
            Use(m_rrRoughness, write);
            Use(m_rrHitDistance, write);
            args.output1 = m_rrSpecularAlbedo.uav;
            args.output2 = m_rrRoughness.uav;
            args.output3 = m_rrHitDistance.uav;
        }
        Dispatch(Pass::Reflections, args);
        Use(m_reflections, read);
        reflections = m_reflections.srv;
        // Ray Reconstruction takes the raw reflections with their guides.
        if (main && !rayReconstruction) {
            Use(m_reflectionHistory[previous], read);
            Use(m_reflectionHistory[current], write);
            Dispatch(Pass::ReflectionTemporal, {.input0 = m_reflections.srv, .input1 = m_reflectionHistory[previous].srv,
                .output0 = m_reflectionHistory[current].uav, .step = m_reflectionHistoryValid ? 1u : 0u});
            Use(m_reflectionHistory[current], read);
            reflections = m_reflectionHistory[current].srv;
        }
    }
    if (main)
        m_reflectionHistoryValid = reflections != kInvalidView && !rayReconstruction;
    mark();
    if (volumetrics && constants.traceMask != 0) {
        const UINT halfWidth = (m_gridWidth + 1) / 2;
        const UINT halfHeight = (m_gridHeight + 1) / 2;
        Use(m_volumetricMarch, write);
        DispatchGroups(Pass::Volumetrics, {.output0 = m_volumetricMarch.uav, .step = 0}, (halfWidth + 7) / 8, (halfHeight + 7) / 8, 1);
        Use(m_volumetricMarch, read);
        Use(m_volumetricHistory[previous], read);
        Use(m_volumetricHistory[current], write);
        DispatchGroups(Pass::Volumetrics, {.input0 = m_volumetricMarch.srv, .input1 = m_volumetricHistory[previous].srv,
            .input2 = m_volumetricHistoryValid ? 1u : 0u, .output0 = m_volumetricHistory[current].uav, .step = 1}, (halfWidth + 7) / 8, (halfHeight + 7) / 8, 1);
        Use(m_volumetricHistory[current], read);
        constants.volumetricView = m_volumetricHistory[current].srv;
        if (rayReconstruction)
            reinterpret_cast<FrameConstants*>(slotConstants + kConstantsStride)->volumetricView = m_volumetricHistory[current].srv;
    }
    if (main)
        m_volumetricHistoryValid = volumetrics && constants.traceMask != 0 && constants.historyValid != 0;
    if (shaded != nullptr)
        Use(*shaded, read);
    Use(m_giRaw, read);
    if (constants.previousLitView != kInvalidView)
        Use(m_lit[previous], read);
    Use(m_lit[current], write);
    PassArgs lighting = {.input0 = shaded != nullptr ? shaded->srv : kInvalidView, .input1 = m_giRaw.srv, .input2 = reflections, .input3 = direct.srv,
        .input4 = m_directRemainder.srv, .output0 = m_lit[current].uav};
    if (rayReconstruction) {
        // Water pixels overwrite the reflection guides the reflections pass wrote.
        Use(m_rrSpecularAlbedo, write);
        Use(m_rrRoughness, write);
        Use(m_rrHitDistance, write);
        D3D12_RESOURCE_BARRIER guides[3] = {};
        ID3D12Resource* guideResources[] = {m_rrSpecularAlbedo.resource.Get(), m_rrRoughness.resource.Get(), m_rrHitDistance.resource.Get()};
        for (size_t i = 0; i < std::size(guides); ++i) {
            guides[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            guides[i].UAV.pResource = guideResources[i];
        }
        m_list->ResourceBarrier(static_cast<UINT>(std::size(guides)), guides);
        lighting.output1 = m_rrSpecularAlbedo.uav;
        lighting.output2 = m_rrRoughness.uav;
        lighting.output3 = m_rrHitDistance.uav;
    }
    Dispatch(Pass::Lighting, lighting);
    mark();
    // The 3D skybox is drawn before the main view, which covers it again, so only the main view is resolved.
    Texture* output = &m_lit[current];
    bool rayReconstructed = false;
    if (rayReconstruction) {
        Use(m_lit[current], read);
        Use(m_surface[current], read);
        Use(m_rrSpecularAlbedo, read);
        Use(m_rrRoughness, read);
        Use(m_rrHitDistance, read);
        Use(m_rrOutput, write);
        SplitViewProjection(constants.viewProj, constants.eyePosition, m_worldToView, m_viewToClip);
        const RayReconstruction::Inputs rr{m_lit[current].resource.Get(), m_rrInputs[RrDepth].resource.Get(), m_motion.resource.Get(),
            m_surface[current].resource.Get(), m_rrInputs[RrAlbedo].resource.Get(), m_rrSpecularAlbedo.resource.Get(), m_rrRoughness.resource.Get(),
            m_rrHitDistance.resource.Get(), m_rrOutput.resource.Get(), m_worldToView, m_viewToClip, renderWidth, renderHeight, inputs.jitterX,
            inputs.jitterY, !m_historyValid};
        rayReconstructed = m_rayReconstruction.Evaluate(m_list.Get(), GetSettings().upscaling, m_width, m_height, rr);
        // NGX leaves its own descriptor heap and root signature bound.
        bindCompute(slotConstantsGpu);
        m_gridWidth = m_width;
        m_gridHeight = m_height;
        if (rayReconstructed) {
            Use(m_rrOutput, read);
            Use(m_resolved, write);
            Dispatch(Pass::DlssResolve, {.input0 = m_rrOutput.srv, .input1 = shiftX, .input2 = shiftY, .output0 = m_resolved.uav});
            output = &m_resolved;
        }
    } else if (main && GetSettings().antiAliasing == AntiAliasing::Temporal) {
        Use(m_lit[current], read);
        Use(m_taaHistory[previous], read);
        Use(m_taaHistory[current], write);
        Dispatch(Pass::TemporalAntiAliasing, {.input0 = m_lit[current].srv, .input1 = m_taaHistory[previous].srv, .output0 = m_taaHistory[current].uav,
            .step = m_taaHistoryValid ? 1u : 0u});
        Use(m_taaHistory[current], read);
        Use(m_resolved, write);
        Dispatch(Pass::Sharpen, {.input0 = m_taaHistory[current].srv, .input1 = shiftX, .input2 = shiftY, .output0 = m_resolved.uav});
        output = &m_resolved;
    }
    m_taaHistoryValid = main ? output == &m_resolved && !rayReconstruction : m_taaHistoryValid;
    // The game draws the view's translucent surfaces next, tested against this depth, which edges keep jittered.
    const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
    const bool stableDepth = main && output == &m_resolved && (depthDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 &&
        EnsureDepthPipeline(DepthStencilFormat(depthDesc.Format));
    if (stableDepth) {
        // The jitter in output pixels. It moves an edge up to half a render pixel (scale / 2) either way, so a pixel the
        // edge can cross may see the other side's samples up to `scale` pixels away.
        const float scale = static_cast<float>(m_width) / static_cast<float>(renderWidth);
        const float shift[2] = {inputs.jitterX * scale, inputs.jitterY * scale};
        Use(m_translucentDepth, write);
        Dispatch(Pass::TranslucentDepth, {.input0 = std::bit_cast<UINT>(shift[0]), .input1 = std::bit_cast<UINT>(shift[1]), .output0 = m_translucentDepth.uav,
            .step = static_cast<UINT>(std::floor(scale)) + 1});
        Use(m_translucentDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    Use(*output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    m_rectDispatch = false;
    m_list->SetGraphicsRootSignature(m_compositeRoot.Get());
    // The viewmodel's run lit only a rectangle, so its composite keeps to the pixels its G-buffer covers.
    const bool covered = inputs.kind == gbuffer::ViewKind::Viewmodel;
    const D3D12_RESOURCE_STATES pixelRead = read | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (covered) {
        const D3D12_RESOURCE_BARRIER barrier = Transition(flags, read, pixelRead);
        m_list->ResourceBarrier(1, &barrier);
    }
    m_list->SetPipelineState(covered ? m_compositeCoveredPipeline.Get() : m_compositePipeline.Get());
    m_list->SetGraphicsRootDescriptorTable(0, m_descriptors->Gpu(output->srv));
    if (covered)
        m_list->SetGraphicsRootDescriptorTable(1, m_descriptors->Gpu(constants.flagsView));
    m_list->RSSetViewports(1, &viewport);
    m_list->RSSetScissorRects(1, &scissor);
    m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_list->DrawInstanced(3, 1, 0, 0);
    if (covered) {
        const D3D12_RESOURCE_BARRIER barrier = Transition(flags, pixelRead, read);
        m_list->ResourceBarrier(1, &barrier);
    }
    if (stableDepth) {
        D3D12_RESOURCE_BARRIER barrier = Transition(depth, read, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_list->ResourceBarrier(1, &barrier);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = m_depthPipelineFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateDepthStencilView(depth, &dsvDesc, dsv);
        m_list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        m_list->SetPipelineState(m_depthPipeline.Get());
        m_list->SetGraphicsRootDescriptorTable(0, m_descriptors->Gpu(m_translucentDepth.srv));
        m_list->DrawInstanced(3, 1, 0, 0);
        barrier = Transition(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, read);
        m_list->ResourceBarrier(1, &barrier);
    }

    D3D12_RESOURCE_BARRIER restore[] = {
        Transition(color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON),
        Transition(depth, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(normal, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(flags, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(light, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(albedo, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(emissive, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(m_colorCopy.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    m_list->ResourceBarrier(static_cast<UINT>(std::size(restore)), restore);
    for (size_t i = kInputViews; i < resources.size(); ++i) {
        D3D12_RESOURCE_BARRIER barrier = Transition(resources[i], read, D3D12_RESOURCE_STATE_COMMON);
        m_list->ResourceBarrier(1, &barrier);
    }
    mark();
    if (main && m_volumeReady && m_probeIndirect[0].resource) {
        // Forward draws read the indirect volume in their own command lists.
        for (Texture& texture : m_probeIndirect)
            Use(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        FrameConstants& forward = *reinterpret_cast<FrameConstants*>(m_forwardConstantsMapped + slot * kConstantsStride);
        forward = constants;
        forward.probeRedView = m_probeIndirect[0].srv;
        forward.probeGreenView = m_probeIndirect[1].srv;
        forward.probeBlueView = m_probeIndirect[2].srv;
        m_forwardFrame = m_forwardConstants->GetGPUVirtualAddress() + slot * kConstantsStride;
        m_forwardScene = frame.tlas;
    }
    m_list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kMarkers, kMarkers, m_timestampReadback.Get(),
        slot * kMarkers * sizeof(uint64_t));
    HRESULT hr = m_list->Close();
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {m_list.Get()};
        queue->ExecuteCommandLists(1, lists);
    } else {
        log::Info("Lighting: closing the command list failed ({:#010x})", Hex(hr));
        m_failed = true;
    }
    queue->Signal(m_fence.Get(), ++m_fenceValue);
    m_slotFenceValues[slot] = m_fenceValue;
    if (m_bakeLogPending) {
        m_bakeLogPending = false;
        WaitForGpu();
        uint32_t counters[8] = {};
        const std::vector<Scene::BakedFaceInfo>& faces = scene.BakedFaces();
        std::vector<uint32_t> faceClamps(faces.size());
        void* mapped = nullptr;
        const D3D12_RANGE range = {0, static_cast<SIZE_T>(m_bakeReadback->GetDesc().Width)};
        if (SUCCEEDED(m_bakeReadback->Map(0, &range, &mapped))) {
            std::memcpy(counters, mapped, sizeof(counters));
            if (32 + faceClamps.size() * 4 <= range.End)
                std::memcpy(faceClamps.data(), static_cast<const std::byte*>(mapped) + 32, faceClamps.size() * 4);
            const D3D12_RANGE written = {0, 0};
            m_bakeReadback->Unmap(0, &written);
        }
        const UINT luxels = std::max(scene.Baked().luxelCount, 1u);
        const UINT sampled = std::max(scene.Baked().luxelCount - std::min(counters[2], scene.Baked().luxelCount), 1u);
        log::Info("Baked light: {} of {} sampled luxels clamped where the recomputed direct light exceeded the baked light ({:.1f}%), {} without a VRAD "
                  "sample, {} samples supersampled; the filter changed {} ({:.1f}%) by more than a tenth; {} of {} leaf ambient samples clamped after "
                  "taking out {} surface lights",
            counters[0], sampled, 100.0 * counters[0] / sampled, counters[2], counters[4], counters[1], 100.0 * counters[1] / luxels, counters[3],
            scene.Baked().ambientSampleCount, scene.Baked().ambientLightCount);
        std::vector<uint32_t> order(faceClamps.size());
        std::iota(order.begin(), order.end(), 0u);
        const size_t shown = std::min<size_t>(order.size(), 8);
        std::partial_sort(order.begin(), order.begin() + shown, order.end(), [&](uint32_t a, uint32_t b) { return faceClamps[a] > faceClamps[b]; });
        for (size_t i = 0; i < shown && faceClamps[order[i]] > 0; ++i) {
            const Scene::BakedFaceInfo& face = faces[order[i]];
            log::Info("Baked light: lightmap {} has {} clamped luxels, first luxel at ({:.0f} {:.0f} {:.0f}), normal ({:.2f} {:.2f} {:.2f})", order[i],
                faceClamps[order[i]], face.position.x, face.position.y, face.position.z, face.normal.x, face.normal.y, face.normal.z);
        }
        if (m_bakeCacheReadback) {
            const std::filesystem::path& path = scene.Baked().cachePath;
            const UINT64 bytes = m_bakeCacheReadback->GetDesc().Width;
            const D3D12_RANGE whole = {0, static_cast<SIZE_T>(bytes)};
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            std::ofstream file(path, std::ios::binary);
            if (file && SUCCEEDED(m_bakeCacheReadback->Map(0, &whole, &mapped))) {
                file.write(reinterpret_cast<const char*>(&kBakeVersion), sizeof(kBakeVersion));
                file.write(static_cast<const char*>(mapped), static_cast<std::streamsize>(bytes));
                const D3D12_RANGE written = {0, 0};
                m_bakeCacheReadback->Unmap(0, &written);
            }
            if (!file)
                log::Info("Baked light: writing {} failed", path.string());
            m_bakeCacheReadback.Reset();
        }
    }
    if (probe) {
        m_probeRequested = false;
        WaitForGpu();
        uint32_t data[kProbeBytes / 4] = {};
        void* mapped = nullptr;
        const D3D12_RANGE range = {0, kProbeBytes};
        if (SUCCEEDED(m_probeReadback->Map(0, &range, &mapped))) {
            std::memcpy(data, mapped, sizeof(data));
            const D3D12_RANGE written = {0, 0};
            m_probeReadback->Unmap(0, &written);
            ProbeResult result = {data[0], data[1], std::bit_cast<float>(data[2]), std::bit_cast<float>(data[3]),
                {std::bit_cast<float>(data[4]), std::bit_cast<float>(data[5]), std::bit_cast<float>(data[6])}, -1, 0};
            std::tie(result.model, result.mask) = scene.InstanceInfo(result.instance);
            m_probe = result;
        }
    }
    ++m_run;
    if (main) {
        std::memcpy(m_prevViewProj, constants.viewProj, sizeof(m_prevViewProj));
        std::memcpy(m_prevEye, constants.eyePosition, sizeof(m_prevEye));
        m_historyValid = constants.traceMask != 0;
        m_taaHistoryValid = !rayReconstruction && (m_taaHistoryValid && constants.historyValid != 0 ? true : output == &m_resolved);
        m_lastMainFrame = inputs.frame;
        ++m_mainRuns;
    }
}

}
