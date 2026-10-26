#include "lighting.h"

#include "gbuffer.h"
#include "log.h"
#include "mod.h"
#include "renderer.h"
#include "settings.h"

#include <g_compositePS.h>
#include <g_compositeVS.h>
#include <g_deferredLightingCS.h>
#include <g_dlssInputsCS.h>
#include <g_dlssResolveCS.h>
#include <g_giFilterCS.h>
#include <g_giTemporalCS.h>
#include <g_giTraceCS.h>
#include <g_reflectionsCS.h>
#include <g_surfaceCS.h>
#include <g_taaCS.h>
#include <g_taaSharpenCS.h>

#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

constexpr UINT kConstantsStride = 2 * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
static_assert(sizeof(FrameConstants) <= kConstantsStride);
// Each slot has the view's constants and a copy for the passes Ray Reconstruction runs at render resolution.
constexpr UINT kConstantsPerSlot = 2;
// Per slot: colour copy, depth, G-buffer normal, flags, light, albedo and self-illumination views, then projector cookies.
constexpr UINT kInputViews = 7;
constexpr UINT kMaxProjectors = 8;
constexpr UINT kViewsPerSlot = kInputViews + kMaxProjectors;
constexpr UINT kInvalidView = ~0u;

// Mirrors Projector in shaders/scene.hlsli.
struct GpuProjector {
    float worldToTexture[4][4];
    float origin[3];
    float farZ;
    float color[3];
    uint32_t cookieView;
    float attenuation[3];
    float padding;
};
static_assert(sizeof(GpuProjector) == 112);
constexpr UINT kTraceMask = kWorldMask | kStaticPropMask | kDynamicMask | kLocalPlayerMask;
// The viewmodel is drawn inside the local player's body, which would shadow it.
constexpr UINT kViewmodelTraceMask = kTraceMask & ~kLocalPlayerMask;
constexpr UINT kFilterSteps[] = {1, 2, 4};
// The viewmodel has no history, so it takes several indirect samples per pixel.
constexpr UINT kViewmodelIndirectSamples = 8;
constexpr UINT kPassCount = static_cast<UINT>(GpuPass::Count);
// A timestamp before the first pass and after each one.
constexpr UINT kMarkers = kPassCount + 1;
constexpr uint32_t kTimingSamples = 120;

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

std::filesystem::path ModuleDirectory()
{
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module) ||
        GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return {};
    return std::filesystem::path(path).parent_path();
}

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

bool Lighting::Initialize(Renderer& renderer, IDirect3DDevice9* device)
{
    m_renderer = &renderer;
    m_device = renderer.Device();
    m_descriptors = &renderer.Descriptors();
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&m_device9On12));
    if (SUCCEEDED(hr))
        hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &m_flushQuery);
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
        m_constants = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, kConstantsStride * kConstantsPerSlot * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
        hr = m_constants ? m_constants->Map(0, nullptr, reinterpret_cast<void**>(&m_constantsMapped)) : E_OUTOFMEMORY;
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
        hr = renderer.Queue()->GetTimestampFrequency(&frequency);
        m_timestampPeriodMs = frequency ? 1000.0 / static_cast<double>(frequency) : 0.0;
    }
    if (SUCCEEDED(hr))
        hr = m_device->CreateRootSignature(0, g_surfaceCS, sizeof(g_surfaceCS), IID_PPV_ARGS(&m_computeRoot));
    const std::pair<const BYTE*, size_t> shaders[] = {{g_surfaceCS, sizeof(g_surfaceCS)}, {g_giTraceCS, sizeof(g_giTraceCS)},
        {g_giTemporalCS, sizeof(g_giTemporalCS)}, {g_giFilterCS, sizeof(g_giFilterCS)}, {g_deferredLightingCS, sizeof(g_deferredLightingCS)},
        {g_taaCS, sizeof(g_taaCS)}, {g_taaSharpenCS, sizeof(g_taaSharpenCS)}, {g_reflectionsCS, sizeof(g_reflectionsCS)},
        {g_dlssInputsCS, sizeof(g_dlssInputsCS)}, {g_dlssResolveCS, sizeof(g_dlssResolveCS)}};
    static_assert(std::size(shaders) == static_cast<size_t>(Pass::Count));
    for (size_t i = 0; i < m_pipelines.size() && SUCCEEDED(hr); ++i) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_computeRoot.Get();
        desc.CS = {shaders[i].first, shaders[i].second};
        hr = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_pipelines[i]));
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
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_rtvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_skinned.Initialize(m_device, *m_descriptors);
    for (UINT i = 0; i < RrInputCount; ++i)
        m_rrInputs[i].uav = m_rrInputUavs + i;
    GetStatistics().rayReconstructionAvailable = m_rayReconstruction.Initialize(m_device, ModuleDirectory(), GetOptions().gameDirectory);
    log::Info("Lighting: initialized");
    return true;
}

bool Lighting::CreateTarget(Texture& texture, UINT width, UINT height, DXGI_FORMAT format)
{
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
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
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(texture.resource.Get(), &srv, m_descriptors->Cpu(texture.srv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &uav, m_descriptors->Cpu(texture.uav));
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
    ok = ok && CreateTarget(m_giRaw, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_giHistory)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_giFiltered)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // Linear (Ray Reconstruction) or encoded shaded colour.
    for (Texture& texture : m_lit)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_reflections, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_taaHistory)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_viewmodelIndirect)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_resolved, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    // Ray Reconstruction's inputs use the top-left render-size part of these.
    constexpr DXGI_FORMAT kRrFormats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT};
    static_assert(std::size(kRrFormats) == RrInputCount);
    for (UINT i = 0; i < RrInputCount; ++i)
        ok = ok && CreateTarget(m_rrInputs[i], width, height, kRrFormats[i]);
    ok = ok && CreateTarget(m_rrSpecularAlbedo, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_rrRoughness, width, height, DXGI_FORMAT_R16_FLOAT);
    ok = ok && CreateTarget(m_rrOutput, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_rayReconstruction.ReleaseFeature();
    m_width = width;
    m_height = height;
    m_historyValid = false;
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

void Lighting::Dispatch(Pass pass, const std::array<UINT, 8>& constants)
{
    m_list->SetPipelineState(m_pipelines[static_cast<size_t>(pass)].Get());
    m_list->SetComputeRoot32BitConstants(2, static_cast<UINT>(constants.size()), constants.data(), 0);
    m_list->Dispatch((m_gridWidth + 7) / 8, (m_gridHeight + 7) / 8, 1);
}

void Lighting::WaitForGpu()
{
    if (!m_fence)
        return;
    m_renderer->Queue()->Signal(m_fence.Get(), ++m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

bool Lighting::FillConstants(FrameConstants& constants, const Scene& scene, const ViewInputs& inputs)
{
    constants = {};
    std::memcpy(constants.viewProj, gbuffer::LastCaptureViewProj().data(), sizeof(constants.viewProj));
    if (!Invert(constants.viewProj, constants.invViewProj))
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
    const Settings& settings = GetSettings();
    for (int i = 0; i < 3; ++i) {
        constants.sunColor[i] *= settings.sun ? settings.sunIntensity : 0.0f;
        constants.skyColor[i] *= settings.skyIntensity;
    }
    constants.lightScale = settings.lightIntensity;
    constants.indirectScale = settings.indirectLight ? settings.indirectIntensity : 0.0f;
    constants.taaHistoryWeight = settings.taaHistoryWeight;
    constants.sharpening = settings.sharpening;
    constants.tonemapScale = gbuffer::TonemapScale();
    constants.outputSrgb = gbuffer::OutputIsSrgb() ? 1u : 0u;
    constants.invSize[0] = 1.0f / static_cast<float>(m_width);
    constants.invSize[1] = 1.0f / static_cast<float>(m_height);
    constants.size[0] = m_width;
    constants.size[1] = m_height;
    constants.normalBias = 2.0f;
    constants.frameIndex = static_cast<uint32_t>(inputs.frame);
    constants.debugView = static_cast<uint32_t>(settings.debugView);
    constants.materialsView = scene.MaterialsView();
    constants.reflectionScale = settings.reflections ? settings.reflectionIntensity : 0.0f;
    constants.depthMin = inputs.depthMin;
    constants.depthScale = inputs.depthMax - inputs.depthMin;
    const gbuffer::Fog& fog = gbuffer::ViewFog(inputs.kind);
    constants.fogEnabled = fog.known && settings.fog ? 1u : 0u;
    std::memcpy(constants.fogParams, fog.params.data(), sizeof(constants.fogParams));
    std::memcpy(constants.fogColor, fog.color.data(), sizeof(constants.fogColor));
    // The 3D skybox is a scaled-down scene elsewhere in the map: the map's lights do not belong to it.
    const bool skybox = inputs.kind == gbuffer::ViewKind::Skybox;
    constants.lightCount = settings.mapLights && !skybox ? scene.LightCount() : 0u;
    constants.lightsView = scene.LightsView();
    const Scene::LightLists& lists = scene.Lights();
    constants.lightRangesView = lists.rangesView;
    constants.lightIndicesView = lists.indicesView;
    constants.bspNodesView = lists.nodesView;
    constants.leafClustersView = lists.leafClustersView;
    constants.clusterCount = lists.clusterCount;
    constants.viewmodel = inputs.kind == gbuffer::ViewKind::Viewmodel ? 1u : 0u;
    // The skybox view shares the main view's orientation, so it finds last frame's sky (reflections.hlsl).
    if (inputs.kind != gbuffer::ViewKind::Viewmodel) {
        constants.historyValid = m_historyValid && inputs.frame - m_lastMainFrame <= 2 ? 1u : 0u;
        std::memcpy(constants.prevViewProj, m_prevViewProj, sizeof(constants.prevViewProj));
        std::memcpy(constants.prevEyePosition, m_prevEye, sizeof(constants.prevEyePosition));
    }
    return true;
}

void Lighting::ReturnResources(std::span<IDirect3DResource9* const> resources, UINT count)
{
    UINT64 value = m_fenceValue;
    ID3D12Fence* fence = m_fence.Get();
    for (UINT i = 0; i < count; ++i)
        m_device9On12->ReturnUnderlyingResource(resources[i], 1, &value, &fence);
}

UINT Lighting::WriteProjectors(UINT slot, std::span<const gbuffer::ProjectedLight> projected, std::span<ID3D12Resource* const> cookies, UINT cookieViews)
{
    const float scale = GetSettings().projectedIntensity;
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
        gpu.cookieView = kInvalidView;
        if (i < cookies.size() && cookies[i] != nullptr) {
            const D3D12_RESOURCE_DESC desc = cookies[i]->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = ColorViewFormat(desc.Format);
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

bool Lighting::RayReconstructionSize(UINT outputWidth, UINT outputHeight, UINT& width, UINT& height)
{
    const Settings& settings = GetSettings();
    if (settings.antiAliasing != AntiAliasing::RayReconstruction || settings.debugView != DebugView::Final || !m_rayReconstruction.Usable())
        return false;
    if (m_rrSizeMode != settings.dlssMode || m_rrSize[0] != outputWidth || m_rrSize[1] != outputHeight) {
        if (!m_rayReconstruction.RenderSize(settings.dlssMode, outputWidth, outputHeight, m_rrSize[2], m_rrSize[3]))
            return false;
        m_rrSizeMode = settings.dlssMode;
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
    D3DSURFACE_DESC surfaceDesc;
    inputs.color->GetDesc(&surfaceDesc);
    if (!EnsureTargets(surfaceDesc.Width, surfaceDesc.Height)) {
        log::Info("Lighting: creating render targets failed; lighting disabled");
        m_failed = true;
        return;
    }

    const UINT slot = static_cast<UINT>(m_run % kSlots);
    if (m_fence->GetCompletedValue() < m_slotFenceValues[slot]) {
        m_fence->SetEventOnCompletion(m_slotFenceValues[slot], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    // Timings of the main view that last used this slot; skybox runs reuse the markers but are not counted.
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
                m_passMsTotal = {};
                m_gpuSamples = 0;
                if (++m_timingReports % (GetOptions().devMode ? 1 : 10) == 0) {
                    constexpr size_t kRecentFrames = 240;
                    const size_t frames = std::min(statistics.frameCount, kRecentFrames);
                    double frameMs = 0.0;
                    for (size_t i = 1; i <= frames; ++i)
                        frameMs += statistics.frameMs[(statistics.nextFrame + statistics.frameMs.size() - i) % statistics.frameMs.size()];
                    const auto& p = statistics.passMs;
                    log::Info("Lighting: {:.3f} ms GPU per frame ({}x{} traced at {}x{}; scene {:.2f}, surface {:.2f}, indirect {:.2f}, "
                              "accumulation {:.2f}, denoiser {:.2f}, reflections {:.2f}, lighting {:.2f}, resolve {:.2f}); frame time {:.2f} ms",
                        statistics.lightingMs, m_width, m_height, statistics.renderWidth, statistics.renderHeight, p[0], p[1], p[2], p[3], p[4],
                        p[5], p[6], p[7], frames ? frameMs / frames : 0.0);
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
    if (rayReconstruction && m_rayReconstruction.NeedsNewFeature(GetSettings().dlssMode, renderWidth, renderHeight, m_width, m_height)) {
        WaitForGpu();
        m_rayReconstruction.ReleaseFeature();
        m_historyValid = false;
    }

    std::byte* slotConstants = m_constantsMapped + slot * kConstantsPerSlot * kConstantsStride;
    const D3D12_GPU_VIRTUAL_ADDRESS slotConstantsGpu = m_constants->GetGPUVirtualAddress() + slot * kConstantsPerSlot * kConstantsStride;
    FrameConstants& constants = *reinterpret_cast<FrameConstants*>(slotConstants);
    if (!FillConstants(constants, scene, inputs)) {
        if (++m_cameraFailures % 300 == 1)
            log::Info("Lighting: the camera of view kind {} cannot be inverted; view left unlit ({} times)", static_cast<int>(inputs.kind), m_cameraFailures);
        return;
    }

    m_flushQuery->Issue(D3DISSUE_END);
    m_flushQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);

    ID3D12CommandQueue* queue = m_renderer->Queue();
    const bool projectedLights = inputs.kind != gbuffer::ViewKind::Skybox && GetSettings().projectedLights && GetSettings().flashlightMode == FlashlightMode::RayTraced;
    std::vector<IDirect3DResource9*> resources9 = {inputs.color, inputs.depth, inputs.normal, inputs.flags, inputs.light, inputs.albedo, inputs.emissive};
    std::vector<ComPtr<ID3D12Resource>> resources(resources9.size());
    for (UINT i = 0; i < resources.size(); ++i) {
        HRESULT hr = m_device9On12->UnwrapUnderlyingResource(resources9[i], queue, IID_PPV_ARGS(&resources[i]));
        if (FAILED(hr)) {
            log::Info("Lighting: unwrapping input {} failed ({:#010x})", i, Hex(hr));
            ReturnResources(resources9, i);
            m_failed = true;
            return;
        }
    }
    // Cookie textures of the projected lights; one that cannot be unwrapped falls back to a round spot.
    std::vector<ID3D12Resource*> cookies;
    if (projectedLights) {
        for (size_t i = 0; i < inputs.projected.size() && i < kMaxProjectors; ++i) {
            ID3D12Resource* cookie = nullptr;
            IDirect3DBaseTexture9* texture = inputs.projected[i].cookie.Get();
            for (size_t j = 0; texture != nullptr && j < i; ++j) {
                if (inputs.projected[j].cookie.Get() == texture)
                    cookie = cookies[j];
            }
            if (texture != nullptr && cookie == nullptr) {
                ComPtr<ID3D12Resource> unwrapped;
                if (SUCCEEDED(m_device9On12->UnwrapUnderlyingResource(texture, queue, IID_PPV_ARGS(&unwrapped)))) {
                    resources9.push_back(texture);
                    resources.push_back(unwrapped);
                    cookie = unwrapped.Get();
                }
            }
            cookies.push_back(cookie);
        }
    }
    ID3D12Resource* color = resources[0].Get();
    ID3D12Resource* depth = resources[1].Get();
    ID3D12Resource* normal = resources[2].Get();
    ID3D12Resource* flags = resources[3].Get();
    ID3D12Resource* light = resources[4].Get();
    ID3D12Resource* albedo = resources[5].Get();
    ID3D12Resource* emissive = resources[6].Get();
    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
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
        constants.projectorCount = WriteProjectors(slot, inputs.projected, cookies, views + kInputViews);
        constants.projectorsView = m_projectorViews[slot];
    }
    // The 3D skybox is drawn before the main view of the same frame, so it may use the surface the main
    // view is about to overwrite; the previous frame's main-view data stays intact.
    const UINT current = static_cast<UINT>(m_mainRuns % 2);
    const UINT previous = 1 - current;
    constants.surfaceView = m_surface[current].srv;
    constants.prevSurfaceView = m_surface[previous].srv;

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
            D3D12_RESOURCE_BARRIER barrier = Transition(resources[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
    m_skinned.Record(m_list.Get(), slot, skinned, m_frameInstances);
    const SceneFrame frame = scene.RecordFrame(m_list.Get(), slot, m_frameInstances);
    constants.instancesView = frame.instancesView;
    constants.dynamicLightsView = frame.dynamicLightsView;
    constants.dynamicLightCount = GetSettings().dynamicLights && inputs.kind != gbuffer::ViewKind::Skybox ? frame.dynamicLightCount : 0u;
    Statistics& statistics = GetStatistics();
    if (main) {
        statistics.mapLights = constants.lightCount;
        statistics.dynamicLights = constants.dynamicLightCount;
        statistics.projectedLights = constants.projectorCount;
        statistics.sceneInstances = static_cast<uint32_t>(1 + m_frameInstances.size());
    }
    if (frame.tlas == 0 || (inputs.kind == gbuffer::ViewKind::Skybox && !scene.HasSkybox()))
        constants.traceMask = 0;
    else if (inputs.kind == gbuffer::ViewKind::Skybox)
        constants.traceMask = kSkyboxMask;
    else
        constants.traceMask = inputs.kind == gbuffer::ViewKind::Viewmodel ? kViewmodelTraceMask : kTraceMask;

    auto bindCompute = [&](D3D12_GPU_VIRTUAL_ADDRESS frameConstants) {
        ID3D12DescriptorHeap* heaps[] = {m_descriptors->Heap()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetComputeRootSignature(m_computeRoot.Get());
        m_list->SetComputeRootConstantBufferView(0, frameConstants);
        m_list->SetComputeRootShaderResourceView(1, frame.tlas);
    };
    bindCompute(slotConstantsGpu);
    m_gridWidth = m_width;
    m_gridHeight = m_height;

    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto mark = [&] { m_list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, marker++); };
    if (rayReconstruction) {
        // The passes before Ray Reconstruction run at render resolution on copies of the G-buffer.
        for (Texture& texture : m_rrInputs)
            Use(texture, write);
        Dispatch(Pass::DlssInputs, {renderWidth, renderHeight, 0, m_rrInputUavs});
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
    mark();
    Use(m_surface[current], write);
    Dispatch(Pass::Surface, {0, 0, 0, m_surface[current].uav});
    mark();
    Use(m_surface[current], read);
    Use(m_surface[previous], read);
    Texture* indirect = &m_giFiltered[0];
    // Untraced views and the distant 3D skybox (traced for reflections and sun shadows only) take the sky's
    // light as their indirect light.
    if (constants.traceMask == 0 || inputs.kind == gbuffer::ViewKind::Skybox) {
        indirect = nullptr;
        mark();
        mark();
        mark();
    } else if (constants.traceMask != 0 && !main) {
        Use(m_giFiltered[0], read);
        Use(m_giRaw, write);
        Dispatch(Pass::GiTrace, {kInvalidView, 0, 0, m_giRaw.uav, 0, kViewmodelIndirectSamples});
        mark();
        mark();
        Texture* source = &m_giRaw;
        for (size_t i = 0; i < std::size(kFilterSteps) && GetSettings().denoiser; ++i) {
            Texture* target = &m_viewmodelIndirect[i % 2];
            Use(*source, read);
            Use(*target, write);
            Dispatch(Pass::GiFilter, {source->srv, 0, 0, target->uav, 0, kFilterSteps[i]});
            source = target;
        }
        indirect = source;
        mark();
    } else if (constants.traceMask != 0 && rayReconstruction) {
        // Ray Reconstruction denoises the raw estimate itself.
        Use(m_giRaw, write);
        Dispatch(Pass::GiTrace, {kInvalidView, 0, 0, m_giRaw.uav});
        indirect = &m_giRaw;
        mark();
        mark();
        mark();
    } else if (constants.traceMask != 0) {
        Use(m_giFiltered[0], read);
        Use(m_giRaw, write);
        Dispatch(Pass::GiTrace, {m_giFiltered[0].srv, 0, 0, m_giRaw.uav});
        mark();
        Use(m_giRaw, read);
        Use(m_giHistory[previous], read);
        Use(m_giHistory[current], write);
        Dispatch(Pass::GiTemporal, {m_giRaw.srv, m_giHistory[previous].srv, 0, m_giHistory[current].uav});
        mark();
        Texture* source = &m_giHistory[current];
        for (size_t i = 0; i < std::size(kFilterSteps) && GetSettings().denoiser; ++i) {
            Texture* target = &m_giFiltered[i % 2];
            Use(*source, read);
            Use(*target, write);
            Dispatch(Pass::GiFilter, {source->srv, 0, 0, target->uav, 0, kFilterSteps[i]});
            source = target;
        }
        indirect = source;
        mark();
    }
    UINT reflections = kInvalidView;
    if (constants.traceMask != 0) {
        Use(m_lit[previous], read);
        Use(m_reflections, write);
        if (rayReconstruction) {
            Use(m_rrSpecularAlbedo, write);
            Use(m_rrRoughness, write);
            Dispatch(Pass::Reflections, {m_lit[previous].srv, 0, 0, m_reflections.uav, m_rrSpecularAlbedo.uav, 0, m_rrRoughness.uav});
        } else {
            Dispatch(Pass::Reflections, {m_lit[previous].srv, 0, 0, m_reflections.uav, kInvalidView});
        }
        Use(m_reflections, read);
        reflections = m_reflections.srv;
    }
    mark();
    if (indirect != nullptr)
        Use(*indirect, read);
    Use(m_giRaw, read);
    Use(m_lit[current], write);
    Dispatch(Pass::Lighting, {indirect != nullptr ? indirect->srv : kInvalidView, m_giRaw.srv, reflections, m_lit[current].uav});
    mark();
    // The 3D skybox is drawn before the main view, which covers it again, so only the main view is resolved.
    Texture* output = &m_lit[current];
    bool rayReconstructed = false;
    if (rayReconstruction) {
        Use(m_lit[current], read);
        Use(m_surface[current], read);
        Use(m_rrSpecularAlbedo, read);
        Use(m_rrRoughness, read);
        Use(m_rrOutput, write);
        const RayReconstruction::Inputs rr{m_lit[current].resource.Get(), m_rrInputs[RrDepth].resource.Get(), m_rrInputs[RrMotion].resource.Get(),
            m_surface[current].resource.Get(), m_rrInputs[RrAlbedo].resource.Get(), m_rrSpecularAlbedo.resource.Get(), m_rrRoughness.resource.Get(),
            m_rrOutput.resource.Get(), renderWidth, renderHeight, inputs.jitterX, inputs.jitterY, !m_historyValid};
        rayReconstructed = m_rayReconstruction.Evaluate(m_list.Get(), GetSettings().dlssMode, m_width, m_height, rr);
        // NGX leaves its own descriptor heap and root signature bound.
        bindCompute(slotConstantsGpu);
        m_gridWidth = m_width;
        m_gridHeight = m_height;
        if (rayReconstructed) {
            Use(m_rrOutput, read);
            Use(m_resolved, write);
            Dispatch(Pass::DlssResolve, {m_rrOutput.srv, 0, 0, m_resolved.uav});
            output = &m_resolved;
        }
    } else if (main && GetSettings().antiAliasing == AntiAliasing::Temporal) {
        Use(m_lit[current], read);
        Use(m_taaHistory[previous], read);
        Use(m_taaHistory[current], write);
        Dispatch(Pass::TemporalAntiAliasing, {m_lit[current].srv, m_taaHistory[previous].srv, 0, m_taaHistory[current].uav, 0,
            m_taaHistoryValid ? 1u : 0u});
        Use(m_taaHistory[current], read);
        Use(m_resolved, write);
        Dispatch(Pass::Sharpen, {m_taaHistory[current].srv, 0, 0, m_resolved.uav});
        output = &m_resolved;
    }
    m_taaHistoryValid = main ? output == &m_resolved && !rayReconstruction : m_taaHistoryValid;
    Use(*output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    m_list->SetGraphicsRootSignature(m_compositeRoot.Get());
    m_list->SetPipelineState(m_compositePipeline.Get());
    m_list->SetGraphicsRootDescriptorTable(0, m_descriptors->Gpu(output->srv));
    m_list->RSSetViewports(1, &viewport);
    m_list->RSSetScissorRects(1, &scissor);
    m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_list->DrawInstanced(3, 1, 0, 0);

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
        D3D12_RESOURCE_BARRIER barrier = Transition(resources[i].Get(), read, D3D12_RESOURCE_STATE_COMMON);
        m_list->ResourceBarrier(1, &barrier);
    }
    mark();
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
    ReturnResources(resources9, static_cast<UINT>(resources9.size()));
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
