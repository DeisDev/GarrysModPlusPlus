#include "fsr.h"

#include "anti_lag.h"
#include "command_line.h"
#include "lighting.h"
#include "log.h"

#include <windows.h>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <ffx_denoiser.h>
#include <ffx_framegeneration.h>
#include <ffx_upscale.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace gmodpp {

namespace fsr {

namespace {

ffxFunctions g_functions = {};
bool g_loaded = false;
bool g_attempted = false;

void LogMessage(uint32_t type, const wchar_t* message)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
    std::string text(length > 0 ? static_cast<size_t>(length - 1) : 0, '\0');
    if (length > 1)
        WideCharToMultiByte(CP_UTF8, 0, message, -1, text.data(), length, nullptr, nullptr);
    log::Info("FSR {}: {}", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

}

const ffxFunctions* Load(const std::filesystem::path& directory)
{
    if (std::exchange(g_attempted, true))
        return g_loaded ? &g_functions : nullptr;
    const std::filesystem::path path = directory / L"amd_fidelityfx_loader_dx12.dll";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        log::Info("FSR: loading {} failed ({}); AMD FSR is unavailable", path.string(), GetLastError());
        return nullptr;
    }
    ffxLoadFunctions(&g_functions, module);
    if (g_functions.CreateContext == nullptr || g_functions.DestroyContext == nullptr || g_functions.Configure == nullptr ||
        g_functions.Query == nullptr || g_functions.Dispatch == nullptr) {
        log::Info("FSR: {} lacks the FSR API's functions; AMD FSR is unavailable", path.string());
        return nullptr;
    }
    // Errors and warnings of every effect go to the log rather than to the debugger.
    ffxConfigureDescGlobalDebug debug = {};
    debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    debug.fpMessage = &LogMessage;
    debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    for (const uint64_t effect : {FFX_API_EFFECT_ID_UPSCALE, FFX_API_EFFECT_ID_DENOISER, FFX_API_EFFECT_ID_FRAMEGENERATION,
             FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN}) {
        debug.effectId = effect;
        g_functions.Configure(nullptr, &debug.header);
    }
    g_loaded = true;
    return &g_functions;
}

bool FrameGenerationSupported(ID3D12Device* device, const std::filesystem::path& directory)
{
    const ffxFunctions* functions = Load(directory);
    if (functions == nullptr)
        return false;
    uint64_t versions = 0;
    ffxQueryDescGetVersions query = {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    query.device = device;
    query.outputCount = &versions;
    const bool supported = functions->Query(nullptr, &query.header) == FFX_API_RETURN_OK && versions > 0;
    log::Info("FSR: frame generation {}", supported ? "available" : "unavailable on this device");
    return supported;
}

}

namespace {

uint32_t QualityMode(UpscalingMode mode)
{
    switch (mode) {
    case UpscalingMode::Native: return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
    case UpscalingMode::Quality: return FFX_UPSCALE_QUALITY_MODE_QUALITY;
    case UpscalingMode::Balanced: return FFX_UPSCALE_QUALITY_MODE_BALANCED;
    case UpscalingMode::Performance: return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
    default: return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
    }
}

}

bool FsrUpscaler::Initialize(ID3D12Device* device, const std::filesystem::path& directory)
{
    m_device = device;
    m_functions = fsr::Load(directory);
    if (m_functions == nullptr)
        return false;
    uint64_t versions = 0;
    ffxQueryDescGetVersions query = {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.device = device;
    query.outputCount = &versions;
    if (m_functions->Query(nullptr, &query.header) != FFX_API_RETURN_OK || versions == 0) {
        log::Info("FSR: no upscaler supports this device");
        m_functions = nullptr;
        return false;
    }
    const char* name = nullptr;
    uint64_t id = 0;
    uint64_t first = 1;
    query.outputCount = &first;
    query.versionIds = &id;
    query.versionNames = &name;
    if (m_functions->Query(nullptr, &query.header) == FFX_API_RETURN_OK && name != nullptr)
        log::Info("FSR: upscaling available ({} versions, newest {})", versions, name);
    return true;
}

bool FsrUpscaler::RenderSize(UpscalingMode mode, UINT outputWidth, UINT outputHeight, UINT& width, UINT& height)
{
    if (!Usable())
        return false;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query = {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
    query.displayWidth = outputWidth;
    query.displayHeight = outputHeight;
    query.qualityMode = QualityMode(mode);
    query.pOutRenderWidth = &renderWidth;
    query.pOutRenderHeight = &renderHeight;
    // A null-context query is routed to the device's provider through a chained backend description (ffx-api.md, Query).
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = m_device;
    query.header.pNext = &backend.header;
    if (m_functions->Query(nullptr, &query.header) != FFX_API_RETURN_OK || renderWidth == 0 || renderHeight == 0)
        return false;
    width = renderWidth;
    height = renderHeight;
    return true;
}

bool FsrUpscaler::EnsureContext(UINT outputWidth, UINT outputHeight, bool reversedDepth)
{
    if (m_context != nullptr && m_contextSize[0] == outputWidth && m_contextSize[1] == outputHeight && m_contextReversed == reversedDepth)
        return true;
    Release();
    // The light is linear HDR before Source's tonemap scale, which FSR's own exposure then follows.
    ffxCreateContextDescUpscale create = {};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | (reversedDepth ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u);
    create.maxRenderSize = {outputWidth, outputHeight};
    create.maxUpscaleSize = {outputWidth, outputHeight};
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = m_device;
    create.header.pNext = &backend.header;
    ffxContext context = nullptr;
    const ffxReturnCode_t result = m_functions->CreateContext(&context, &create.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        log::Info("FSR: creating the upscaler ({}x{}) failed ({}); falling back to TAA", outputWidth, outputHeight, result);
        m_failed = true;
        return false;
    }
    ffxQueryGetProviderVersion version = {};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (m_functions->Query(&context, &version.header) == FFX_API_RETURN_OK && version.versionName != nullptr)
        log::Info("FSR: upscaler {} at {}x{}", version.versionName, outputWidth, outputHeight);
    m_context = context;
    m_contextSize[0] = outputWidth;
    m_contextSize[1] = outputHeight;
    m_contextReversed = reversedDepth;
    return true;
}

bool FsrUpscaler::Evaluate(ID3D12GraphicsCommandList* list, const Inputs& inputs)
{
    if (!Usable() || !EnsureContext(inputs.outputWidth, inputs.outputHeight, inputs.reversedDepth))
        return false;
    ffxDispatchDescUpscale dispatch = {};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = list;
    dispatch.color = ffxApiGetResourceDX12(inputs.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = ffxApiGetResourceDX12(inputs.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = ffxApiGetResourceDX12(inputs.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.output = ffxApiGetResourceDX12(inputs.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = {inputs.jitterX, inputs.jitterY};
    // Motion is in render pixels already.
    dispatch.motionVectorScale = {1.0f, 1.0f};
    dispatch.renderSize = {inputs.renderWidth, inputs.renderHeight};
    dispatch.upscaleSize = {inputs.outputWidth, inputs.outputHeight};
    dispatch.frameTimeDelta = inputs.frameMilliseconds;
    dispatch.preExposure = 1.0f;
    dispatch.reset = inputs.reset;
    dispatch.cameraNear = inputs.cameraNear;
    dispatch.cameraFar = inputs.cameraFar;
    dispatch.cameraFovAngleVertical = inputs.verticalFov;
    // A Hammer unit is about an inch (Valve Developer Community, "Hammer units").
    dispatch.viewSpaceToMetersFactor = 0.0254f;
    const ffxReturnCode_t result = m_functions->Dispatch(&m_context, &dispatch.header);
    if (result != FFX_API_RETURN_OK) {
        log::Info("FSR: upscaling failed ({}); falling back to TAA", result);
        m_failed = true;
        return false;
    }
    return true;
}

void FsrUpscaler::Release()
{
    if (m_context != nullptr && m_functions != nullptr)
        m_functions->DestroyContext(&m_context, nullptr);
    m_context = nullptr;
}

bool FsrRayRegeneration::Initialize(ID3D12Device* device, const std::filesystem::path& directory)
{
    m_device = device;
    m_functions = fsr::Load(directory);
    if (m_functions == nullptr)
        return false;
    // No version means no provider for this device (denoising.md, 2.3. Querying support).
    uint64_t versions = 0;
    ffxQueryDescGetVersions query = {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    query.device = device;
    query.outputCount = &versions;
    if (m_functions->Query(nullptr, &query.header) != FFX_API_RETURN_OK || versions == 0) {
        log::Info("FSR: Ray Regeneration needs a Radeon RX 9000 GPU; unavailable");
        m_functions = nullptr;
        return false;
    }
    log::Info("FSR: Ray Regeneration available");
    return true;
}

bool FsrRayRegeneration::Evaluate(ID3D12GraphicsCommandList* list, const Inputs& inputs)
{
    if (!Usable())
        return false;
    if (m_context == nullptr || m_contextSize[0] != inputs.maxWidth || m_contextSize[1] != inputs.maxHeight) {
        Release();
        ffxCreateContextDescDenoiser create = {};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
        create.version = FFX_DENOISER_VERSION;
        create.maxRenderSize = {inputs.maxWidth, inputs.maxHeight};
        create.signalFlags = FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE | FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
        ffxCreateBackendDX12Desc backend = {};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = m_device;
        create.header.pNext = &backend.header;
        ffxContext context = nullptr;
        const ffxReturnCode_t result = m_functions->CreateContext(&context, &create.header, nullptr);
        if (result != FFX_API_RETURN_OK) {
            log::Info("FSR: creating Ray Regeneration ({}x{}) failed ({}); NRD denoises instead", inputs.maxWidth, inputs.maxHeight, result);
            m_failed = true;
            return false;
        }
        m_context = context;
        m_contextSize[0] = inputs.maxWidth;
        m_contextSize[1] = inputs.maxHeight;
    }
    ffxDispatchDescDenoiser dispatch = {};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    dispatch.commandList = list;
    dispatch.linearDepth = ffxApiGetResourceDX12(inputs.linearDepth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = ffxApiGetResourceDX12(inputs.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.normals = ffxApiGetResourceDX12(inputs.normals, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.specularAlbedo = ffxApiGetResourceDX12(inputs.specularAlbedo, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.diffuseAlbedo = ffxApiGetResourceDX12(inputs.diffuseAlbedo, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    // Motion is in texture coordinates and view depth already.
    dispatch.motionVectorScale = {1.0f, 1.0f, 1.0f};
    dispatch.jitterOffsets = {inputs.jitterX, inputs.jitterY};
    dispatch.cameraPositionDelta = {inputs.cameraDelta[0], inputs.cameraDelta[1], inputs.cameraDelta[2]};
    static_assert(sizeof(FfxApiMatrix4x4) == 16 * sizeof(float));
    std::memcpy(&dispatch.view, inputs.worldToView, sizeof(FfxApiMatrix4x4));
    std::memcpy(&dispatch.projection, inputs.viewToClip, sizeof(FfxApiMatrix4x4));
    // The sky and water hold 0, below the bounds, and pass through.
    dispatch.linearDepthBounds = {1e-3f, inputs.maxDepth};
    dispatch.renderSize = {inputs.width, inputs.height};
    dispatch.frameIndex = inputs.frame;
    dispatch.flags = FFX_DENOISER_DISPATCH_NON_ENCODED_ALBEDO | (inputs.reset ? FFX_DENOISER_DISPATCH_RESET : 0u);
    ffxDispatchDescDenoiserIndirectDiffuse diffuse = {};
    diffuse.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;
    diffuse.signal.input = ffxApiGetResourceDX12(inputs.diffuse, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    diffuse.signal.output = ffxApiGetResourceDX12(inputs.diffuseOut, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    ffxDispatchDescDenoiserIndirectSpecular specular = {};
    specular.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
    specular.signal.input = ffxApiGetResourceDX12(inputs.specular, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    specular.signal.output = ffxApiGetResourceDX12(inputs.specularOut, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.header.pNext = &diffuse.header;
    diffuse.header.pNext = &specular.header;
    const ffxReturnCode_t result = m_functions->Dispatch(&m_context, &dispatch.header);
    if (result != FFX_API_RETURN_OK) {
        log::Info("FSR: Ray Regeneration failed ({}); NRD denoises instead", result);
        m_failed = true;
        return false;
    }
    return true;
}

void FsrRayRegeneration::Release()
{
    if (m_context != nullptr && m_functions != nullptr)
        m_functions->DestroyContext(&m_context, nullptr);
    m_context = nullptr;
}

namespace {

D3D12_RESOURCE_STATES D3D12State(uint32_t state)
{
    switch (state) {
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ: return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_API_RESOURCE_STATE_GENERIC_READ: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

void AddTransition(D3D12_RESOURCE_BARRIER* barriers, UINT& count, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER& barrier = barriers[count++];
    barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
}

// Each presented frame, rendered or generated, on FSR's present thread: the frame into the swap chain's buffer as FSR's
// own composition does without a callback (there is no separate UI to compose), and its kind to Anti-Lag 2.
ffxReturnCode_t PresentFrame(ffxCallbackDescFrameGenerationPresent* params, void*)
{
    anti_lag::SetFrameType(params->isGeneratedFrame);
    auto* list = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
    auto* source = static_cast<ID3D12Resource*>(params->currentBackBuffer.resource);
    auto* target = static_cast<ID3D12Resource*>(params->outputSwapChainBuffer.resource);
    const D3D12_RESOURCE_STATES sourceState = D3D12State(params->currentBackBuffer.state);
    const D3D12_RESOURCE_STATES targetState = D3D12State(params->outputSwapChainBuffer.state);
    D3D12_RESOURCE_BARRIER barriers[2];
    UINT count = 0;
    AddTransition(barriers, count, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    AddTransition(barriers, count, target, targetState, D3D12_RESOURCE_STATE_COPY_DEST);
    if (count > 0)
        list->ResourceBarrier(count, barriers);
    list->CopyResource(target, source);
    count = 0;
    AddTransition(barriers, count, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    AddTransition(barriers, count, target, D3D12_RESOURCE_STATE_COPY_DEST, targetState);
    if (count > 0)
        list->ResourceBarrier(count, barriers);
    return FFX_API_RETURN_OK;
}

// Generation's work, recorded by FSR's swap chain during Present on the render thread (frame-interpolation-api.md,
// Dispatch frame generation).
ffxReturnCode_t GenerateFrame(ffxDispatchDescFrameGeneration* params, void* context)
{
    return fsr::g_functions.Dispatch(static_cast<ffxContext*>(context), &params->header);
}

}

Microsoft::WRL::ComPtr<IDXGISwapChain4> FsrFrameGeneration::CreateSwapChain(ID3D12Device* device, IDXGIFactory* factory, ID3D12CommandQueue* queue,
    HWND window, const DXGI_SWAP_CHAIN_DESC1& desc)
{
    m_device = device;
    m_functions = fsr::Load(ModDirectory());
    if (m_functions == nullptr)
        return nullptr;
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = desc;
    IDXGISwapChain4* swapChain = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create = {};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    create.swapchain = &swapChain;
    create.hwnd = window;
    create.desc = &swapChainDesc;
    create.dxgiFactory = factory;
    create.gameQueue = queue;
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version = {};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    create.header.pNext = &version.header;
    ffxContext context = nullptr;
    const ffxReturnCode_t result = m_functions->CreateContext(&context, &create.header, nullptr);
    if (result != FFX_API_RETURN_OK || swapChain == nullptr) {
        log::Info("FSR: creating frame generation's swap chain failed ({})", result);
        return nullptr;
    }
    m_swapChainContext = context;
    m_swapChain = swapChain;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> owned;
    owned.Attach(swapChain);
    ffxQueryGetProviderVersion provider = {};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (m_functions->Query(&context, &provider.header) == FFX_API_RETURN_OK && provider.versionName != nullptr)
        log::Info("FSR: frame generation's swap chain {}", provider.versionName);
    return owned;
}

bool FsrFrameGeneration::EnsureContext(UINT width, UINT height, bool reversedDepth)
{
    if (m_context != nullptr && m_contextSize[0] == width && m_contextSize[1] == height && m_contextReversed == reversedDepth)
        return true;
    ReleaseContext();
    // The back buffer holds 8-bit sRGB-encoded colour, not HDR; depth and motion come at the view's size.
    ffxCreateContextDescFrameGeneration create = {};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.flags = reversedDepth ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0u;
    create.displaySize = {width, height};
    create.maxRenderSize = {width, height};
    create.backBufferFormat = FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    ffxCreateContextDescFrameGenerationVersion version = {};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = m_device;
    create.header.pNext = &version.header;
    version.header.pNext = &backend.header;
    ffxContext context = nullptr;
    const ffxReturnCode_t result = m_functions->CreateContext(&context, &create.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        log::Info("FSR: creating frame generation ({}x{}) failed ({}); frames are not generated", width, height, result);
        m_failed = true;
        return false;
    }
    ffxQueryGetProviderVersion provider = {};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (m_functions->Query(&context, &provider.header) == FFX_API_RETURN_OK && provider.versionName != nullptr)
        log::Info("FSR: frame generation {} at {}x{}", provider.versionName, width, height);
    m_context = context;
    m_contextSize[0] = width;
    m_contextSize[1] = height;
    m_contextReversed = reversedDepth;
    return true;
}

// Once a frame (frame-interpolation-api.md, Configure frame generation), with frame IDs one apart.
bool FsrFrameGeneration::Configure(bool enabled, ID3D12Resource* hudless, const D3D12_RECT& view)
{
    ffxConfigureDescFrameGeneration config = {};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = m_swapChain;
    config.presentCallback = &PresentFrame;
    config.frameGenerationCallback = enabled ? &GenerateFrame : nullptr;
    config.frameGenerationCallbackUserContext = enabled ? &m_context : nullptr;
    config.frameGenerationEnabled = enabled;
    // On the game's queue, so the inputs can change in the next frame (Resource Lifetime).
    config.allowAsyncWorkloads = false;
    if (enabled && hudless != nullptr)
        config.HUDLessColor = ffxApiGetResourceDX12(hudless, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    config.generationRect = {view.left, view.top, view.right - view.left, view.bottom - view.top};
    config.frameID = m_frameId;
    const ffxReturnCode_t result = m_functions->Configure(&m_context, &config.header);
    if (result != FFX_API_RETURN_OK) {
        log::Info("FSR: configuring frame generation failed ({}); frames are not generated", result);
        m_failed = true;
        return false;
    }
    return true;
}

void FsrFrameGeneration::Prepare(ID3D12GraphicsCommandList* list, const Inputs& inputs, const PostView& post)
{
    if (!Active() || m_prepared)
        return;
    // A D3D projection for row vectors: device depth d = m22 + m32 / z, 0 at the near plane and 1 at the far one
    // (reversed: 1 and 0).
    const float m22 = post.viewToClip[10];
    const float m32 = post.viewToClip[14];
    const bool reversed = m32 > 0.0f;
    if (!EnsureContext(inputs.displayWidth, inputs.displayHeight, reversed) || !Configure(true, inputs.hudless, inputs.view))
        return;
    const UINT width = static_cast<UINT>(inputs.view.right - inputs.view.left);
    const UINT height = static_cast<UINT>(inputs.view.bottom - inputs.view.top);
    ffxDispatchDescFrameGenerationPrepareV2 prepare = {};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = m_frameId;
    prepare.commandList = list;
    prepare.renderSize = {width, height};
    // The resolved view is not jittered, and motion is in its pixels.
    prepare.jitterOffset = {0.0f, 0.0f};
    prepare.motionVectorScale = {1.0f, 1.0f};
    prepare.frameTimeDelta = inputs.frameMilliseconds;
    prepare.reset = !m_preparedLast;
    prepare.cameraNear = std::fabs(m32 / ((reversed ? 1.0f : 0.0f) - m22));
    prepare.cameraFar = std::fabs(m32 / ((reversed ? 0.0f : 1.0f) - m22));
    prepare.cameraFovAngleVertical = 2.0f * std::atan(1.0f / post.viewToClip[5]);
    // A Hammer unit is about an inch (Valve Developer Community, "Hammer units").
    prepare.viewSpaceToMetersFactor = 0.0254f;
    prepare.depth = ffxApiGetResourceDX12(inputs.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prepare.motionVectors = ffxApiGetResourceDX12(inputs.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    // The view's axes are the world-to-view rotation's columns (row vectors: x right, y up, z forward).
    for (int i = 0; i < 3; ++i) {
        prepare.cameraPosition[i] = post.eye[i];
        prepare.cameraRight[i] = post.worldToView[i * 4 + 0];
        prepare.cameraUp[i] = post.worldToView[i * 4 + 1];
        prepare.cameraForward[i] = post.worldToView[i * 4 + 2];
    }
    const ffxReturnCode_t result = m_functions->Dispatch(&m_context, &prepare.header);
    if (result != FFX_API_RETURN_OK) {
        log::Info("FSR: preparing frame generation failed ({}); frames are not generated", result);
        Configure(false, nullptr, inputs.view);
        m_failed = true;
        return;
    }
    m_prepared = true;
}

void FsrFrameGeneration::BeforePresent()
{
    if (m_context != nullptr && !m_prepared && !m_failed)
        Configure(false, nullptr, D3D12_RECT{0, 0, static_cast<LONG>(m_contextSize[0]), static_cast<LONG>(m_contextSize[1])});
    m_preparedLast = m_prepared;
    m_prepared = false;
    ++m_frameId;
}

// Generation is turned off first, which waits for the swap chain's work that uses the context (Shutdown).
void FsrFrameGeneration::ReleaseContext()
{
    if (m_context == nullptr)
        return;
    ffxConfigureDescFrameGeneration config = {};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = m_swapChain;
    config.frameGenerationEnabled = false;
    config.frameID = m_frameId;
    m_functions->Configure(&m_context, &config.header);
    m_functions->DestroyContext(&m_context, nullptr);
    m_context = nullptr;
    m_preparedLast = false;
}

}
