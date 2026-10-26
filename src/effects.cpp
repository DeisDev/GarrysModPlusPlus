#include "effects.h"

#include "bsp.h"
#include "client_hooks.h"
#include "engine_interfaces.h"
#include "entities.h"
#include "log.h"
#include "mod.h"
#include "renderer.h"
#include "scene.h"
#include "skinned_casters.h"
#include "studio_model.h"
#include "sun_shadows.h"

#include <g_compositePS.h>
#include <g_compositeVS.h>

#include <d3d9on12.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp::effects {

namespace {

constexpr UINT kFrameCount = 3;
constexpr UINT kDescriptorsPerFrame = 16;
constexpr UINT kDescriptorsPerEffect = 3;
constexpr UINT kMapPollInterval = 30;
constexpr int kMaxBones = 256;
constexpr UINT kConstantsStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
static_assert(sizeof(FrameConstants) <= kConstantsStride);
const D3DFORMAT kIntzFormat = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

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

float Luminance(const bsp::Float3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

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

class EffectSystem {
public:
    bool Initialize(Renderer& renderer, IDirect3DDevice9* device);
    void Run(IDirect3DSurface9* color, IDirect3DTexture9* depth, Scene& scene, std::span<const SceneInstance> dynamic,
        const std::vector<SkinnedEntity>& skinned);
    SkinnedCasters& Skinned() { return m_skinned; }
    void WaitForGpu();

private:
    bool EnsureTargets(UINT width, UINT height);
    ComPtr<ID3D12Resource> CreateTexture(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index) const;
    void FillConstants(FrameConstants& constants, const Scene& scene, UINT width, UINT height, bool& valid);

    Renderer* m_renderer = nullptr;
    ID3D12Device5* m_device = nullptr;
    ComPtr<IDirect3DDevice9On12> m_device9On12;
    ComPtr<IDirect3DQuery9> m_flushQuery;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> m_allocators;
    std::array<uint64_t, kFrameCount> m_frameFenceValues = {};
    ComPtr<ID3D12GraphicsCommandList4> m_list;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    ComPtr<ID3D12DescriptorHeap> m_viewHeap;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_viewIncrement = 0;
    UINT m_rtvIncrement = 0;
    ComPtr<ID3D12Resource> m_constants;
    std::byte* m_constantsMapped = nullptr;
    ComPtr<ID3D12RootSignature> m_compositeRoot;
    ComPtr<ID3D12PipelineState> m_compositePipeline;
    ComPtr<ID3D12Resource> m_sceneCopy;
    std::array<ComPtr<ID3D12Resource>, 2> m_pingPong;
    UINT m_width = 0;
    UINT m_height = 0;
    std::vector<std::unique_ptr<Effect>> m_effects;
    SkinnedCasters m_skinned;
    std::vector<SceneInstance> m_frameInstances;
    ComPtr<ID3D12QueryHeap> m_timestamps;
    ComPtr<ID3D12Resource> m_timestampReadback;
    const uint64_t* m_timestampValues = nullptr;
    double m_timestampPeriodMs = 0.0;
    double m_gpuMsTotal = 0.0;
    uint32_t m_gpuSamples = 0;
    uint64_t m_frame = 0;
    bool m_loggedFormats = false;
    bool m_failed = false;
};

struct ModelEntry {
    int sceneModel = -1;
    std::shared_ptr<SkinnedModel> skinned;
};

struct State {
    IDirect3DDevice9* device = nullptr;
    ComPtr<IDirect3DTexture9> depthTexture;
    ComPtr<IDirect3DSurface9> depthSurface;
    bool depthReplaced = false;
    DWORD renderThread = 0;
    uint64_t frame = 0;
    uint64_t lastRunFrame = UINT64_MAX;
    bool warnedQueuedRendering = false;
    std::unique_ptr<EffectSystem> system;
    Scene scene;
    bool sceneInitialized = false;
    std::string failedMap;
    uint32_t debugView = 0;
    std::unordered_map<std::string, ModelEntry> models;
    std::unordered_map<std::string, bool> seeThroughMaterials;
    uint32_t modelErrors = 0;
    std::vector<entities::ModelEntity> entities;
    std::vector<SceneInstance> dynamicInstances;
    std::vector<SkinnedEntity> skinnedEntities;
    std::vector<studio::Matrix3x4> bones;
};

State g_state;

bool EffectSystem::Initialize(Renderer& renderer, IDirect3DDevice9* device)
{
    m_renderer = &renderer;
    m_device = renderer.Device();
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
        D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kFrameCount * kDescriptorsPerFrame, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_viewHeap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kFrameCount};
        hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kConstantsStride * kFrameCount;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constants));
    }
    if (SUCCEEDED(hr))
        hr = m_constants->Map(0, nullptr, reinterpret_cast<void**>(&m_constantsMapped));
    if (SUCCEEDED(hr)) {
        D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kFrameCount * 2};
        hr = m_device->CreateQueryHeap(&desc, IID_PPV_ARGS(&m_timestamps));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeof(uint64_t) * 2 * kFrameCount;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_timestampReadback));
    }
    if (SUCCEEDED(hr))
        hr = m_timestampReadback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint64_t**>(&m_timestampValues)));
    if (SUCCEEDED(hr)) {
        UINT64 frequency = 0;
        hr = renderer.Queue()->GetTimestampFrequency(&frequency);
        m_timestampPeriodMs = frequency ? 1000.0 / static_cast<double>(frequency) : 0.0;
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
    if (FAILED(hr)) {
        log::Info("Effects: initialization failed ({:#010x})", Hex(hr));
        return false;
    }
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_viewIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_rtvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    m_skinned.Initialize(m_device);
    m_effects.push_back(std::make_unique<SunShadows>());
    for (auto& effect : m_effects) {
        if (!effect->Initialize(m_device)) {
            log::Info("Effects: '{}' failed to initialize", effect->Name());
            return false;
        }
    }
    if (m_effects.size() * kDescriptorsPerEffect + 1 > kDescriptorsPerFrame) {
        log::Info("Effects: too many effects for the descriptor budget");
        return false;
    }
    log::Info("Effects: initialized with {} effect(s)", m_effects.size());
    return true;
}

ComPtr<ID3D12Resource> EffectSystem::CreateTexture(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
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
    desc.Flags = flags;
    ComPtr<ID3D12Resource> texture;
    HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture));
    if (FAILED(hr))
        log::Info("Effects: {}x{} texture creation failed ({:#010x})", width, height, Hex(hr));
    return texture;
}

bool EffectSystem::EnsureTargets(UINT width, UINT height)
{
    if (m_sceneCopy && width == m_width && height == m_height)
        return true;
    WaitForGpu();
    m_sceneCopy = CreateTexture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    for (auto& texture : m_pingPong)
        texture = CreateTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_width = width;
    m_height = height;
    return m_sceneCopy && m_pingPong[0] && m_pingPong[1];
}

D3D12_CPU_DESCRIPTOR_HANDLE EffectSystem::Cpu(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_viewHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_viewIncrement;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE EffectSystem::Gpu(UINT index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_viewHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * m_viewIncrement;
    return handle;
}

void EffectSystem::WaitForGpu()
{
    if (!m_fence)
        return;
    m_renderer->Queue()->Signal(m_fence.Get(), ++m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void EffectSystem::FillConstants(FrameConstants& constants, const Scene& scene, UINT width, UINT height, bool& valid)
{
    const CameraConstants& camera = m_renderer->Camera();
    constants = {};
    valid = Invert(camera.viewProj, constants.invViewProj);
    std::memcpy(constants.eyePosition, camera.eyePos, sizeof(constants.eyePosition));
    const bsp::SunLight& sun = scene.Sun();
    constants.sunEnabled = sun.present ? 1.0f : 0.0f;
    float length = std::sqrt(sun.direction.x * sun.direction.x + sun.direction.y * sun.direction.y + sun.direction.z * sun.direction.z);
    if (length > 0.0f) {
        constants.sunDirection[0] = sun.direction.x / length;
        constants.sunDirection[1] = sun.direction.y / length;
        constants.sunDirection[2] = sun.direction.z / length;
    }
    float sunLuminance = Luminance(sun.color);
    constants.ambientRatio = sunLuminance > 0.0f ? std::max(Luminance(sun.ambient) / sunLuminance, 0.15f) : 0.3f;
    constants.invSize[0] = 1.0f / static_cast<float>(width);
    constants.invSize[1] = 1.0f / static_cast<float>(height);
    constants.size[0] = width;
    constants.size[1] = height;
    constants.shadowStrength = 1.0f;
    constants.bakedRadius = 12.0f;
    constants.normalBias = 2.0f;
    constants.debugView = g_state.debugView;
}

void EffectSystem::Run(IDirect3DSurface9* color9, IDirect3DTexture9* depth9, Scene& scene, std::span<const SceneInstance> dynamic,
    const std::vector<SkinnedEntity>& skinned)
{
    if (m_failed)
        return;
    D3DSURFACE_DESC surfaceDesc;
    color9->GetDesc(&surfaceDesc);
    if (!EnsureTargets(surfaceDesc.Width, surfaceDesc.Height)) {
        m_failed = true;
        return;
    }

    const UINT slot = static_cast<UINT>(m_frame % kFrameCount);
    if (m_fence->GetCompletedValue() < m_frameFenceValues[slot]) {
        m_fence->SetEventOnCompletion(m_frameFenceValues[slot], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    if (m_frameFenceValues[slot] != 0) {
        const uint64_t begin = m_timestampValues[slot * 2];
        const uint64_t end = m_timestampValues[slot * 2 + 1];
        if (end > begin) {
            m_gpuMsTotal += static_cast<double>(end - begin) * m_timestampPeriodMs;
            if (++m_gpuSamples == 300) {
                log::Info("Effects: {:.3f} ms GPU per frame ({}x{})", m_gpuMsTotal / m_gpuSamples, m_width, m_height);
                m_gpuMsTotal = 0.0;
                m_gpuSamples = 0;
            }
        }
    }

    FrameConstants& constants = *reinterpret_cast<FrameConstants*>(m_constantsMapped + slot * kConstantsStride);
    bool cameraValid = false;
    FillConstants(constants, scene, m_width, m_height, cameraValid);
    if (!cameraValid)
        return;

    m_flushQuery->Issue(D3DISSUE_END);
    m_flushQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);

    ID3D12CommandQueue* queue = m_renderer->Queue();
    ComPtr<ID3D12Resource> color;
    ComPtr<ID3D12Resource> depth;
    HRESULT hr = m_device9On12->UnwrapUnderlyingResource(color9, queue, IID_PPV_ARGS(&color));
    if (FAILED(hr)) {
        log::Info("Effects: unwrapping the color target failed ({:#010x})", Hex(hr));
        m_failed = true;
        return;
    }
    hr = m_device9On12->UnwrapUnderlyingResource(depth9, queue, IID_PPV_ARGS(&depth));
    if (FAILED(hr)) {
        log::Info("Effects: unwrapping the depth texture failed ({:#010x})", Hex(hr));
        UINT64 value = m_fenceValue;
        ID3D12Fence* fence = m_fence.Get();
        m_device9On12->ReturnUnderlyingResource(color9, 0, &value, &fence);
        m_failed = true;
        return;
    }

    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
    const DXGI_FORMAT depthView = DepthViewFormat(depthDesc.Format);
    if (!m_loggedFormats) {
        m_loggedFormats = true;
        log::Info("Effects: color {}x{} format {} flags {:#x}; depth format {} flags {:#x}", colorDesc.Width, colorDesc.Height,
            static_cast<int>(colorDesc.Format), static_cast<unsigned>(colorDesc.Flags), static_cast<int>(depthDesc.Format),
            static_cast<unsigned>(depthDesc.Flags));
    }

    const UINT base = slot * kDescriptorsPerFrame;
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = {};
    depthSrv.Format = depthView;
    depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Texture2D.MipLevels = 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC colorSrv = depthSrv;
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav = {};
    outputUav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    outputUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    for (UINT i = 0; i < m_effects.size(); ++i) {
        const UINT table = base + i * kDescriptorsPerEffect;
        ID3D12Resource* input = i == 0 ? m_sceneCopy.Get() : m_pingPong[(i - 1) % 2].Get();
        colorSrv.Format = i == 0 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        m_device->CreateShaderResourceView(depth.Get(), &depthSrv, Cpu(table));
        m_device->CreateShaderResourceView(input, &colorSrv, Cpu(table + 1));
        m_device->CreateUnorderedAccessView(m_pingPong[i % 2].Get(), nullptr, &outputUav, Cpu(table + 2));
    }
    ID3D12Resource* result = m_pingPong[(m_effects.size() - 1) % 2].Get();
    const UINT resultIndex = base + kDescriptorsPerFrame - 1;
    colorSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_device->CreateShaderResourceView(result, &colorSrv, Cpu(resultIndex));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(slot) * m_rtvIncrement;
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = ColorViewFormat(colorDesc.Format);
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_device->CreateRenderTargetView(color.Get(), &rtvDesc, rtv);

    m_allocators[slot]->Reset();
    m_list->Reset(m_allocators[slot].Get(), nullptr);
    m_list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    {
        D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        m_list->ResourceBarrier(2, barriers);
    }
    m_list->CopyResource(m_sceneCopy.Get(), color.Get());
    {
        D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(m_sceneCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        m_list->ResourceBarrier(2, barriers);
    }

    m_frameInstances.assign(dynamic.begin(), dynamic.end());
    m_skinned.Record(m_list.Get(), slot, skinned, m_frameInstances);
    const D3D12_GPU_VIRTUAL_ADDRESS tlas = scene.RecordFrameTlas(m_list.Get(), slot, m_frameInstances);
    ID3D12DescriptorHeap* heaps[] = {m_viewHeap.Get()};
    m_list->SetDescriptorHeaps(1, heaps);
    for (UINT i = 0; i < m_effects.size(); ++i) {
        if (i > 0) {
            D3D12_RESOURCE_BARRIER barrier = Transition(m_pingPong[(i - 1) % 2].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_list->ResourceBarrier(1, &barrier);
        }
        if (tlas == 0)
            break;
        EffectFrame frame{m_list.Get(), m_constants->GetGPUVirtualAddress() + slot * kConstantsStride, tlas,
            Gpu(base + i * kDescriptorsPerEffect), m_width, m_height, scene};
        m_effects[i]->Record(frame);
    }

    {
        D3D12_RESOURCE_BARRIER barrier = Transition(result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_list->ResourceBarrier(1, &barrier);
    }
    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    m_list->SetGraphicsRootSignature(m_compositeRoot.Get());
    m_list->SetPipelineState(m_compositePipeline.Get());
    m_list->SetGraphicsRootDescriptorTable(0, Gpu(resultIndex));
    m_list->RSSetViewports(1, &viewport);
    m_list->RSSetScissorRects(1, &scissor);
    m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_list->DrawInstanced(3, 1, 0, 0);

    std::vector<D3D12_RESOURCE_BARRIER> restore = {
        Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON),
        Transition(depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
        Transition(m_sceneCopy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        Transition(result, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    if (m_effects.size() > 1) {
        ID3D12Resource* other = m_pingPong[m_effects.size() % 2].Get();
        restore.push_back(Transition(other, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    }
    m_list->ResourceBarrier(static_cast<UINT>(restore.size()), restore.data());
    m_list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    m_list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, m_timestampReadback.Get(), slot * 2 * sizeof(uint64_t));
    hr = m_list->Close();
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {m_list.Get()};
        queue->ExecuteCommandLists(1, lists);
    } else {
        log::Info("Effects: closing the frame command list failed ({:#010x})", Hex(hr));
        m_failed = true;
    }
    queue->Signal(m_fence.Get(), ++m_fenceValue);
    m_frameFenceValues[slot] = m_fenceValue;

    UINT64 value = m_fenceValue;
    ID3D12Fence* fence = m_fence.Get();
    m_device9On12->ReturnUnderlyingResource(color9, 1, &value, &fence);
    m_device9On12->ReturnUnderlyingResource(depth9, 1, &value, &fence);
    ++m_frame;
}

void UpdateDebugView()
{
    const Options& options = GetOptions();
    if (!options.devMode)
        return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, (options.gameDirectory / L"gmodpp_debugview.txt").c_str(), L"r") != 0)
        return;
    unsigned view = 0;
    if (fscanf_s(file, "%u", &view) == 1 && view != g_state.debugView) {
        g_state.debugView = view;
        log::Info("Effects: debug view {}", view);
    }
    std::fclose(file);
}

void RunConsoleRequests()
{
    const Options& options = GetOptions();
    if (!options.devMode || !g_state.scene.Ready())
        return;
    const std::filesystem::path path = options.gameDirectory / L"gmodpp_console.txt";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"r") != 0)
        return;
    std::string commands;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), file) != nullptr)
        commands += buffer;
    std::fclose(file);
    DeleteFileW(path.c_str());
    if (!commands.empty() && commands.back() != '\n')
        commands += '\n';
    log::Info("Console request: {}", commands);
    engine::ExecuteCommand(commands);
}

std::vector<std::byte> ReadOptional(const std::string& path)
{
    std::vector<std::byte> data;
    if (!engine::ReadGameFile(path, data))
        data.clear();
    return data;
}

// Loads a model once: a BLAS in its bind pose for rigid use, plus skinning data for multi-bone models.
const ModelEntry& ModelFor(const std::string& mdlPath)
{
    auto cached = g_state.models.find(mdlPath);
    if (cached != g_state.models.end())
        return cached->second;
    ModelEntry& entry = g_state.models[mdlPath];

    const std::string base = mdlPath.size() > 4 ? mdlPath.substr(0, mdlPath.size() - 4) : mdlPath;
    std::vector<std::byte> mdl = ReadOptional(mdlPath);
    std::vector<std::byte> vvd = ReadOptional(base + ".vvd");
    std::vector<std::byte> vtx = ReadOptional(base + ".dx90.vtx");
    std::vector<std::vector<std::string>> texturePaths;
    studio::Geometry geometry;
    std::string error;
    if (mdl.empty() || vvd.empty() || vtx.empty()) {
        error = "model files are missing";
    } else if (studio::ReadMaterialPaths(mdl, texturePaths, error)) {
        std::vector<bool> skip(texturePaths.size(), false);
        for (size_t t = 0; t < texturePaths.size(); ++t) {
            for (const std::string& vmtPath : texturePaths[t]) {
                auto material = g_state.seeThroughMaterials.find(vmtPath);
                if (material == g_state.seeThroughMaterials.end()) {
                    std::vector<std::byte> vmt = ReadOptional(vmtPath);
                    if (vmt.empty())
                        continue;
                    material = g_state.seeThroughMaterials.emplace(vmtPath, studio::MaterialIsSeeThrough(
                        std::string_view(reinterpret_cast<const char*>(vmt.data()), vmt.size()))).first;
                }
                skip[t] = material->second;
                break;
            }
        }
        if (studio::BuildGeometry(mdl, vvd, vtx, skip, geometry, error) && !geometry.indices.empty()) {
            entry.sceneModel = g_state.scene.AddModel(geometry);
            if (studio::BoneCount(mdl) > 1) {
                auto skinned = std::make_shared<SkinnedModel>();
                skinned->geometry = std::move(geometry);
                if (studio::ReadPoseToBone(mdl, skinned->poseToBone) && g_state.system->Skinned().PrepareModel(*skinned))
                    entry.skinned = std::move(skinned);
            }
            return entry;
        }
    }
    if (!error.empty() && ++g_state.modelErrors <= 10)
        log::Info("Scene: model '{}' skipped: {}", mdlPath, error);
    return entry;
}

bool IsFirstPersonModel(const std::string& path)
{
    return path.starts_with("models/weapons/v_") || path.starts_with("models/weapons/c_");
}

void UpdateScene()
{
    if (!engine::Ready())
        return;
    if (!g_state.sceneInitialized) {
        Renderer* renderer = GetRenderer();
        g_state.sceneInitialized = g_state.scene.Initialize(renderer->Device(), renderer->Queue());
        if (!g_state.sceneInitialized)
            return;
    }
    std::string level = engine::LevelName();
    if (level.empty()) {
        if (g_state.scene.Ready()) {
            g_state.system->WaitForGpu();
            g_state.scene.Clear();
            g_state.models.clear();
            g_state.system->Skinned().Clear();
            log::Info("Scene cleared");
        }
        return;
    }
    if (level == g_state.scene.Name() || level == g_state.failedMap)
        return;

    std::vector<std::byte> file;
    bsp::WorldGeometry world;
    std::string error;
    if (!engine::ReadGameFile(level, file)) {
        error = "the game filesystem could not read it";
    } else if (bsp::LoadWorld(file, world, error)) {
        g_state.system->WaitForGpu();
        g_state.models.clear();
        g_state.system->Skinned().Clear();
        if (g_state.scene.LoadMap(level, world)) {
            std::vector<SceneInstance> instances;
            for (const bsp::StaticProp& prop : world.props) {
                SceneInstance& instance = instances.emplace_back();
                instance.model = ModelFor(world.propModels[prop.model]).sceneModel;
                std::memcpy(instance.transform, prop.transform, sizeof(instance.transform));
            }
            std::erase_if(instances, [](const SceneInstance& instance) { return instance.model < 0; });
            log::Info("Scene '{}': {} faces, {} displacements, {} skybox faces skipped, {} world triangles, {} of {} static props; "
                      "sun {} dir ({:.2f} {:.2f} {:.2f})",
                level, world.faceCount, world.displacementCount, world.skyboxFaceCount, world.indices.size() / 3, instances.size(),
                world.props.size(), world.sun.present ? "present" : "missing", world.sun.direction.x, world.sun.direction.y, world.sun.direction.z);
            g_state.scene.SetStaticInstances(std::move(instances));
            return;
        }
        error = "building acceleration structures failed";
    }
    log::Info("Scene: cannot load '{}': {}", level, error);
    g_state.failedMap = level;
}

}

bool PrepareDeviceParameters(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS& params)
{
    D3DDISPLAYMODE mode;
    HRESULT hr = d3d->GetAdapterDisplayMode(adapter, &mode);
    if (SUCCEEDED(hr))
        hr = d3d->CheckDeviceFormat(adapter, D3DDEVTYPE_HAL, mode.Format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, kIntzFormat);
    if (FAILED(hr)) {
        log::Info("Effects disabled: INTZ depth textures are not supported ({:#010x})", Hex(hr));
        g_state.depthReplaced = false;
        return false;
    }
    if (params.MultiSampleType != D3DMULTISAMPLE_NONE)
        log::Info("Effects: disabling {}x MSAA on the back buffer so depth can be read", static_cast<int>(params.MultiSampleType));
    params.MultiSampleType = D3DMULTISAMPLE_NONE;
    params.MultiSampleQuality = 0;
    params.EnableAutoDepthStencil = FALSE;
    g_state.depthReplaced = true;
    return true;
}

void OnDeviceReady(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& params)
{
    g_state.device = device;
    g_state.renderThread = GetCurrentThreadId();
    if (!g_state.depthReplaced)
        return;

    HRESULT hr = device->CreateTexture(params.BackBufferWidth, params.BackBufferHeight, 1, D3DUSAGE_DEPTHSTENCIL, kIntzFormat,
        D3DPOOL_DEFAULT, &g_state.depthTexture, nullptr);
    if (SUCCEEDED(hr))
        hr = g_state.depthTexture->GetSurfaceLevel(0, &g_state.depthSurface);
    if (SUCCEEDED(hr))
        hr = device->SetDepthStencilSurface(g_state.depthSurface.Get());
    if (FAILED(hr)) {
        log::Info("Effects: creating the INTZ depth buffer failed ({:#010x})", Hex(hr));
        g_state.depthTexture.Reset();
        g_state.depthSurface.Reset();
        return;
    }
    log::Info("Effects: INTZ depth buffer {}x{} bound", params.BackBufferWidth, params.BackBufferHeight);

    Renderer* renderer = GetRenderer();
    if (!g_state.system && renderer != nullptr && renderer->SupportsRaytracing()) {
        auto system = std::make_unique<EffectSystem>();
        if (system->Initialize(*renderer, device))
            g_state.system = std::move(system);
    }
}

void OnBeforeReset()
{
    if (g_state.system)
        g_state.system->WaitForGpu();
    g_state.depthSurface.Reset();
    g_state.depthTexture.Reset();
}

void OnPresent()
{
    ++g_state.frame;
    client::TryInstall();
    engine::TryInitialize();
    entities::TryInitialize();
    if (g_state.system && g_state.frame % kMapPollInterval == 0) {
        UpdateScene();
        UpdateDebugView();
        RunConsoleRequests();
    }
}

void OnOpaqueViewFinished()
{
    IDirect3DDevice9* device = g_state.device;
    if (device == nullptr || !g_state.system || !g_state.scene.Ready() || !g_state.depthSurface)
        return;
    if (GetCurrentThreadId() != g_state.renderThread) {
        if (!g_state.warnedQueuedRendering) {
            g_state.warnedQueuedRendering = true;
            log::Info("Effects: views are drawn on a different thread than Direct3D (mat_queue_mode != 0); effects disabled");
        }
        return;
    }
    if (g_state.lastRunFrame == g_state.frame)
        return;

    ComPtr<IDirect3DSurface9> renderTarget;
    ComPtr<IDirect3DSurface9> backBuffer;
    ComPtr<IDirect3DSurface9> depthStencil;
    if (FAILED(device->GetRenderTarget(0, &renderTarget)) || FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) ||
        FAILED(device->GetDepthStencilSurface(&depthStencil)))
        return;
    if (renderTarget != backBuffer || depthStencil != g_state.depthSurface)
        return;

    g_state.lastRunFrame = g_state.frame;
    entities::Collect(g_state.entities);
    g_state.dynamicInstances.clear();
    g_state.skinnedEntities.clear();
    for (const entities::ModelEntity& entity : g_state.entities) {
        if (IsFirstPersonModel(entity.model))
            continue;
        const ModelEntry& model = ModelFor(entity.model);
        if (model.skinned) {
            g_state.bones.resize(kMaxBones);
            if (!entities::SetupBones(entity.renderable, reinterpret_cast<float (*)[3][4]>(g_state.bones.data()), kMaxBones))
                continue;
            g_state.bones.resize(std::min<size_t>(model.skinned->poseToBone.size(), kMaxBones));
            g_state.skinnedEntities.push_back({entity.index, model.skinned, g_state.bones});
            continue;
        }
        if (model.sceneModel < 0)
            continue;
        SceneInstance& instance = g_state.dynamicInstances.emplace_back();
        instance.model = model.sceneModel;
        std::memcpy(instance.transform, entity.transform, sizeof(instance.transform));
    }
    g_state.system->Run(backBuffer.Get(), g_state.depthTexture.Get(), g_state.scene, g_state.dynamicInstances, g_state.skinnedEntities);
}

}
