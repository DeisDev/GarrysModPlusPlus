#include "shaderapi_device.h"

#include "anti_lag.h"
#include "busy_window.h"
#include "command_line.h"
#include "d3d12_device.h"
#include "display_output.h"
#include "log.h"
#include "native_lighting.h"
#include "options.h"
#include "overlay.h"
#include "post_effects.h"
#include "settings.h"
#include "shader_cache.h"
#include "shader_precache.h"
#include "shaderapi.h"
#include "shaderapi_meshes.h"
#include "shaderapi_pipelines.h"
#include "streamline.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp::native {

namespace {

constexpr size_t kDeviceManagerSlots = 17;
constexpr size_t kDeviceSlots = 41;
constexpr int kDxSupportLevel = 95;
// The shader-visible CBV/SRV/UAV heap size every resource binding tier supports.
constexpr UINT kDescriptorCapacity = 1000000;
constexpr UINT64 kUploadPageSize = 16 << 20;
// How often the developer request files are checked, as the shim does.
constexpr unsigned kDevFilePollPresents = 10;
// How long after the first frame earlier sessions' shaders start compiling again (shader_precache::StartReplay).
constexpr auto kReplayDelay = std::chrono::seconds(5);
constexpr UINT64 kStagingPageSize = 32 << 20;
// Staging memory one frame may hold before Flush reclaims it, so loading a map does not keep every upload alive.
constexpr UINT64 kStagingFlushBytes = 256 << 20;
// IShaderUtil (materialsystem.dll), in the SDK's order (shaderapi_slots.cpp).
constexpr unsigned kShaderUtilReleaseShaderObjects = 11;
constexpr unsigned kShaderUtilRestoreShaderObjects = 12;
constexpr unsigned kShaderUtilUncacheUnusedMaterials = 35;

std::optional<ShaderDeviceInfo> g_pendingMode;
constexpr UINT kRenderTargetViews = 4096;
constexpr UINT kDepthStencilViews = 1024;
// The shader-visible sampler heap's maximum size (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE).
constexpr UINT kSamplerHeapSize = 2048;

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

// Direct3D 9 numbers one adapter per display output; DXGI enumerates the adapter holding the primary desktop
// output first, and its outputs in order. Enumerated once, as shaderapidx9.dll does when it starts: the engine asks
// for every mode separately.
struct DisplayAdapter {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    std::vector<ShaderDisplayMode> modes;
};

std::vector<ShaderDisplayMode> EnumerateModes(IDXGIOutput* output)
{
    std::vector<ShaderDisplayMode> modes;
    UINT count = 0;
    output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, nullptr);
    std::vector<DXGI_MODE_DESC> descs(count);
    output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, descs.data());
    std::set<std::tuple<UINT, UINT, UINT>> seen;
    for (const DXGI_MODE_DESC& desc : descs) {
        const UINT hertz = desc.RefreshRate.Denominator != 0 ? (desc.RefreshRate.Numerator + desc.RefreshRate.Denominator / 2) / desc.RefreshRate.Denominator : 0;
        if (seen.insert({ desc.Width, desc.Height, hertz }).second)
            modes.push_back({ kShaderDisplayModeVersion, static_cast<int>(desc.Width), static_cast<int>(desc.Height), kImageFormatBgrx8888,
                static_cast<int>(hertz), 1 });
    }
    return modes;
}

const std::vector<DisplayAdapter>& DisplayAdapters()
{
    static const std::vector<DisplayAdapter> adapters = [] {
        std::vector<DisplayAdapter> result;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return result;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            ComPtr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
                result.push_back({ adapter, output, EnumerateModes(output.Get()) });
        }
        return result;
    }();
    return adapters;
}

std::span<const ShaderDisplayMode> DisplayModes(int adapterIndex)
{
    const std::vector<DisplayAdapter>& adapters = DisplayAdapters();
    if (adapterIndex < 0 || adapterIndex >= static_cast<int>(adapters.size()))
        return {};
    return adapters[static_cast<size_t>(adapterIndex)].modes;
}

struct DeviceManagerObject : NativeObject {
    int adapter = 0;
    std::vector<void (*)()> modeChangeCallbacks;
};

bool Connect(DeviceManagerObject*, CreateInterfaceFn factory)
{
    // The caches are read while the game starts up, so the first draws do not wait for the disk.
    shader_cache::OpenInBackground();
    ReadPipelineLibraryInBackground();
    Backend& backend = GetBackend();
    backend.shaderUtil = factory != nullptr ? factory("VShaderUtil001", nullptr) : nullptr;
    // shaderapidx9.dll asks for VFileSystem022 and reads files through its IBaseFileSystem base, which IFileSystem
    // (public/filesystem.h) puts after IAppSystem's vtable pointer.
    void* fileSystem = factory != nullptr ? factory("VFileSystem022", nullptr) : nullptr;
    backend.fileSystem = fileSystem != nullptr ? static_cast<std::uint8_t*>(fileSystem) + sizeof(void*) : nullptr;
    if (backend.shaderUtil == nullptr || backend.fileSystem == nullptr)
        log::Info("Native: VShaderUtil001 ({}) or VFileSystem022 ({}) is not available", backend.shaderUtil != nullptr, backend.fileSystem != nullptr);
    return backend.shaderUtil != nullptr && backend.fileSystem != nullptr;
}

void Disconnect(DeviceManagerObject*)
{
    GetBackend().shaderUtil = nullptr;
    GetBackend().fileSystem = nullptr;
}
void* QueryInterface(DeviceManagerObject*, const char* name) { return CreateInterface(name, nullptr); }
InitReturnVal Init(DeviceManagerObject*) { return kInitOk; }
void Shutdown(DeviceManagerObject*)
{
    SavePipelineLibrary(true);
    shader_precache::SaveRemembered();
}
int GetAdapterCount(DeviceManagerObject*) { return static_cast<int>(DisplayAdapters().size()); }

void GetAdapterInfo(DeviceManagerObject*, int adapterIndex, MaterialAdapterInfo& info)
{
    std::memset(&info, 0, sizeof(info));
    const std::vector<DisplayAdapter>& adapters = DisplayAdapters();
    if (adapterIndex < 0 || adapterIndex >= static_cast<int>(adapters.size()))
        return;
    DXGI_ADAPTER_DESC1 desc;
    adapters[adapterIndex].adapter->GetDesc1(&desc);
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, info.driverName, sizeof(info.driverName), nullptr, nullptr);
    info.vendorId = desc.VendorId;
    info.deviceId = desc.DeviceId;
    info.subSysId = desc.SubSysId;
    info.revision = desc.Revision;
    info.dxSupportLevel = kDxSupportLevel;
    info.maxDxSupportLevel = kDxSupportLevel;
    LARGE_INTEGER version;
    if (SUCCEEDED(adapters[adapterIndex].adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) {
        info.driverVersionHigh = static_cast<unsigned>(version.HighPart);
        info.driverVersionLow = version.LowPart;
    }
}

bool GetRecommendedConfigurationInfo(DeviceManagerObject*, int, int, void*)
{
    static bool logged = false;
    if (!std::exchange(logged, true))
        log::Info("Native: GetRecommendedConfigurationInfo does not apply dxsupport.cfg yet; the configuration is left as it is");
    return true;
}

int GetModeCount(DeviceManagerObject*, int adapterIndex) { return static_cast<int>(DisplayModes(adapterIndex).size()); }

void GetModeInfo(DeviceManagerObject*, ShaderDisplayMode* info, int adapterIndex, int mode)
{
    const std::span<const ShaderDisplayMode> modes = DisplayModes(adapterIndex);
    if (mode >= 0 && mode < static_cast<int>(modes.size()))
        *info = modes[mode];
}

void GetCurrentModeInfo(DeviceManagerObject*, ShaderDisplayMode* info, int adapterIndex)
{
    const std::vector<DisplayAdapter>& adapters = DisplayAdapters();
    if (adapterIndex < 0 || adapterIndex >= static_cast<int>(adapters.size()))
        return;
    DXGI_OUTPUT_DESC desc;
    adapters[adapterIndex].output->GetDesc(&desc);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
        return;
    *info = { kShaderDisplayModeVersion, static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight), kImageFormatBgrx8888,
        static_cast<int>(mode.dmDisplayFrequency), 1 };
}

bool SetAdapter(DeviceManagerObject* self, int adapterIndex, int)
{
    self->adapter = adapterIndex;
    return true;
}

CreateInterfaceFn SetMode(DeviceManagerObject*, void* window, int, const ShaderDeviceInfo& info)
{
    if (!GetBackend().SetMode(static_cast<HWND>(window), info))
        return nullptr;
    ResetDynamicState();
    return &CreateInterface;
}

void AddModeChangeCallback(DeviceManagerObject* self, void (*callback)()) { self->modeChangeCallbacks.push_back(callback); }

void RemoveModeChangeCallback(DeviceManagerObject* self, void (*callback)())
{
    std::erase(self->modeChangeCallbacks, callback);
}

void ReleaseResources(NativeObject*) {}
void ReacquireResources(NativeObject*) {}
// The engine and client create the render targets derived from the frame in this format (engine.dll 0x180111750:
// _rt_FullFrameFB, _rt_FullFrameFB1 and _rt_gui; 0x1801108a0 and 0x180110940: _rt_SmallFB0-1 and _rt_TeenyFB0-2;
// client.dll 0x180169470: _rt_WaterReflection and _rt_Camera; 0x1800640c0: GMod's s_pMoBlurTex0-1, s_pBloomTex0-1 and
// s_pMorphTexture0-1; 0x1800b6dc0: Lua's GetRenderTarget, and GetRenderTargetEx without an image format; REA 2026-11-04),
// so they hold linear light as the frame does (Texture::srgbEmulated). Targets asked for in another format
// (_rt_PowerOfTwoFB, _rt_WaterRefraction and _rt_spawnicon in 8 bits) keep it.
ImageFormat GetBackBufferFormat(NativeObject*) { return kImageFormatRgba16161616F; }

void GetBackBufferDimensions(NativeObject*, int& width, int& height)
{
    width = static_cast<int>(GetBackend().Width());
    height = static_cast<int>(GetBackend().Height());
}

int GetCurrentAdapter(NativeObject*) { return static_cast<DeviceManagerObject*>(DeviceManager())->adapter; }
// shaderapidx9.dll 0x180027320: whether the device exists yet; materialsystem defers its setup until it does.
bool IsUsingGraphics(NativeObject*) { return GetBackend().HasDevice(); }
int StencilBufferBits(NativeObject*) { return 8; }
// shaderapidx9.dll 0x180018c40: whether the present parameters' MultiSampleType is not D3DMULTISAMPLE_NONE.
bool IsAAEnabled(NativeObject*) { return false; }
template <class Fn>
Fn UtilSlot(unsigned slot)
{
    return reinterpret_cast<Fn>((*static_cast<void***>(GetBackend().shaderUtil))[slot]);
}

// shaderapidx9.dll applies the mode ChangeVideoMode recorded once a frame is presented (Present 0x1800273f0,
// CheckDeviceLost 0x180024580): it recomputes the state snapshots and, unless the window is only being resized, runs
// the mode change callbacks, releases materialsystem's objects (ReleaseResources 0x180027970), resets the device and
// restores them after a full render state reset (ReacquireResources 0x1800276b0).
void ApplyPendingMode()
{
    const ShaderDeviceInfo info = *std::exchange(g_pendingMode, std::nullopt);
    Backend& backend = GetBackend();
    UtilSlot<void (*)(void*, bool)>(kShaderUtilUncacheUnusedMaterials)(backend.shaderUtil, true);
    if (info.resizing)
        return;
    for (void (*callback)() : static_cast<DeviceManagerObject*>(DeviceManager())->modeChangeCallbacks)
        callback();
    UtilSlot<void (*)(void*)>(kShaderUtilReleaseShaderObjects)(backend.shaderUtil);
    if (!backend.SetMode(backend.Window(), info)) {
        log::Info("Native: the video mode change failed");
        return;
    }
    ResetDynamicState();
    UtilSlot<void (*)(void*, CreateInterfaceFn, int)>(kShaderUtilRestoreShaderObjects)(backend.shaderUtil, &CreateInterface, 0);
}

void Present(NativeObject*)
{
    realtime::OnPresent();
    GetBackend().Present();
    if (g_pendingMode)
        ApplyPendingMode();
    GetBackend().PlaceWindow();
}

// The window the game draws for (SetView, shaderapidx9 0x180024050). Vanilla presents into it when it is not the
// device window (0x1800273f0), which only tools do.
HWND g_view = nullptr;

void SetView(NativeObject*, void* window)
{
    g_view = static_cast<HWND>(window);
    static bool logged = false;
    if (g_view != nullptr && g_view != GetBackend().Window() && !logged) {
        logged = true;
        log::Info("Native: SetView to a window other than the game's is not supported; frames still go to the game's window");
    }
}

// 0x180023530: nothing while the device window is minimised, else the view's client area.
void GetWindowSize(NativeObject*, int& width, int& height)
{
    RECT rect = {};
    if (!IsIconic(GetBackend().Window()))
        GetClientRect(g_view != nullptr ? g_view : GetBackend().Window(), &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
}

// Direct3D 9 applied the gamma ramp in exclusive full screen alone; the display mapping applies mat_monitorgamma itself
// (display_output.cpp), in every display mode.
void SetHardwareGammaRamp(NativeObject*, float, float, float, float, bool) {}

}

ComPtr<IDXGIAdapter1> AdapterOfDisplay(int index)
{
    const std::vector<DisplayAdapter>& adapters = DisplayAdapters();
    if (index < 0 || index >= static_cast<int>(adapters.size()))
        return nullptr;
    return adapters[static_cast<size_t>(index)].adapter;
}

bool Backend::CreateDevice()
{
    const std::filesystem::path runtime = ModDirectory() / L"D3D12";
    ComPtr<ID3D12DeviceFactory> agility = AgilityDeviceFactory(runtime);
    if (HasArgument(L"-gmodpp_debug")) {
        ComPtr<ID3D12Debug1> debug;
        HRESULT hr = agility ? agility->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&debug)) : D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (SUCCEEDED(hr)) {
            debug->EnableDebugLayer();
            debug->SetEnableGPUBasedValidation(HasArgument(L"-gmodpp_gbv"));
        } else {
            log::Info("Native: the D3D12 debug layer is unavailable ({:#010x})", Hex(hr));
        }
    }
    // Streamline must be initialized before the DXGI calls it hooks; FSR's frame generation makes its own swap chain
    // instead (ffx_framegeneration.h), with which Streamline's cannot share.
    m_frameGeneration = GetSettings().frameGeneration;
    GetStatistics().startedFrameGeneration = m_frameGeneration;
    if (m_frameGeneration != FrameGeneration::Fsr)
        streamline::Initialize(ModDirectory());
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) {
        log::Info("Native: CreateDXGIFactory2 failed: {:#010x}", Hex(hr));
        return false;
    }
    const std::vector<DisplayAdapter>& adapters = DisplayAdapters();
    const int adapterIndex = static_cast<DeviceManagerObject*>(DeviceManager())->adapter;
    if (adapterIndex < 0 || adapterIndex >= static_cast<int>(adapters.size())) {
        log::Info("Native: no display adapter {}", adapterIndex);
        return false;
    }
    IDXGIAdapter1* adapter = adapters[adapterIndex].adapter.Get();
    hr = agility ? agility->CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device))
                 : D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
        log::Info("Native: D3D12CreateDevice failed: {:#010x}", Hex(hr));
        return false;
    }
    if (HasArgument(L"-gmodpp_debug") && SUCCEEDED(m_device.As(&m_infoQueue))) {
        D3D12_MESSAGE_SEVERITY hidden[] = { D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE, D3D12_MESSAGE_SEVERITY_WARNING };
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumSeverities = static_cast<UINT>(std::size(hidden));
        filter.DenyList.pSeverityList = hidden;
        m_infoQueue->PushStorageFilter(&filter);
    }
    DXGI_ADAPTER_DESC1 adapterDesc = {};
    adapter->GetDesc1(&adapterDesc);
    streamline::SetDevice(m_device.Get(), adapterDesc.AdapterLuid);
    m_queueDevice = streamline::QueueDevice(m_device.Get());
    m_swapChainFactory = streamline::SwapChainFactory(m_factory.Get());
    Statistics& statistics = GetStatistics();
    statistics.reflexAvailable = streamline::ReflexAvailable();
    statistics.dlssFrameGenerationAvailable = streamline::FrameGenerationAvailable();
    statistics.generatedFramesMax = streamline::MaxGeneratedFrames();
    statistics.fsrFrameGenerationAvailable = fsr::FrameGenerationSupported(m_device.Get(), ModDirectory());
    statistics.antiLagAvailable = anti_lag::Initialize(m_device.Get());
    // The queue comes from Streamline's proxy device, which hooks its creation (sl_hooks.h).
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(hr = m_queueDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue))) ||
        FAILED(hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
        log::Info("Native: creating the queue or fence failed: {:#010x}", Hex(hr));
        return false;
    }
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (Frame& frame : m_frames) {
        if (FAILED(hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)))) {
            log::Info("Native: CreateCommandAllocator failed: {:#010x}", Hex(hr));
            return false;
        }
    }
    if (FAILED(hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)))) {
        log::Info("Native: CreateCommandList failed: {:#010x}", Hex(hr));
        return false;
    }
    m_commandList->Close();
    if (!m_descriptors.Initialize(m_device.Get(), kDescriptorCapacity) || !m_rtvs.Initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRenderTargetViews)
        || !m_dsvs.Initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDepthStencilViews))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC samplerDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    if (FAILED(hr = m_device->CreateDescriptorHeap(&samplerDesc, IID_PPV_ARGS(&m_samplerHeap)))) {
        log::Info("Native: creating the sampler heap failed: {:#010x}", Hex(hr));
        return false;
    }
    m_samplerIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    BOOL tearing = FALSE;
    m_tearing = SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing;
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    log::Info("Native: D3D12 device on '{}'", name);
    return true;
}

bool Backend::CreateSwapChain()
{
    if (m_swapChain != nullptr) {
        if (m_recording) {
            m_commandList->Close();
            m_recording = false;
            ID3D12CommandList* lists[] = { m_commandList.Get() };
            m_queue->ExecuteCommandLists(1, lists);
        }
        WaitForGpu();
        for (TrackedResource& buffer : m_backBuffers)
            buffer = {};
        m_fsrFrameGeneration.ReleaseContext();
        HRESULT hr = m_swapChain->ResizeBuffers(kBackBuffers, m_width, m_height, m_outputFormat, m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        if (FAILED(hr)) {
            log::Info("Native: ResizeBuffers failed: {:#010x}", Hex(hr));
            return false;
        }
    } else {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.Format = m_outputFormat;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBackBuffers;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> swapChain;
        if (m_frameGeneration == FrameGeneration::Fsr && GetStatistics().fsrFrameGenerationAvailable)
            swapChain = m_fsrFrameGeneration.CreateSwapChain(m_device.Get(), m_factory.Get(), m_queue.Get(), m_window, desc);
        HRESULT hr = S_OK;
        // Else through Streamline's factory, so the swap chain is its proxy (DLSS Frame Generation, Reflex's present
        // markers); a plain DXGI one also when FSR could not make its own (logged), so the game still has a window.
        if (swapChain == nullptr)
            hr = m_swapChainFactory->CreateSwapChainForHwnd(m_queue.Get(), m_window, &desc, nullptr, nullptr, &swapChain);
        if (FAILED(hr) || FAILED(hr = swapChain.As(&m_swapChain))) {
            log::Info("Native: CreateSwapChainForHwnd failed: {:#010x}", Hex(hr));
            return false;
        }
        m_factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);
    }
    AcquireBuffers();
    if (!CreateFrameBuffer() || !CreateDepthBuffer())
        return false;
    log::Info("Native: swap chain {}x{} in format {}, vsync {}", m_width, m_height, static_cast<int>(m_outputFormat), m_vsync ? "on" : "off");
    BeginFrame();
    return true;
}

void Backend::AcquireBuffers()
{
    for (UINT i = 0; i < kBackBuffers; ++i) {
        m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i].resource));
        m_backBuffers[i].state = D3D12_RESOURCE_STATE_PRESENT;
        if (m_backBufferViews[i].ptr == 0)
            m_backBufferViews[i] = m_rtvs.Allocate();
        D3D12_RENDER_TARGET_VIEW_DESC desc = {};
        desc.Format = m_outputFormat;
        desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_device->CreateRenderTargetView(m_backBuffers[i].resource.Get(), &desc, m_backBufferViews[i]);
    }
    // R10G10B10A2 takes either G22_P709 (SDR) or G2084_P2020 (HDR10); B8G8R8A8 G22_P709 (DXGI_COLOR_SPACE_TYPE).
    const DXGI_COLOR_SPACE_TYPE space = m_outputHdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const HRESULT hr = m_swapChain->SetColorSpace1(space);
    if (FAILED(hr))
        log::Info("Native: SetColorSpace1({}) failed: {:#010x}", static_cast<int>(space), Hex(hr));
}

bool Backend::CreateDepthBuffer()
{
    if (m_depthBuffer.resource != nullptr)
        Retire(std::move(m_depthBuffer.resource));
    m_depthBuffer = {};
    const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kDepthResourceFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kDepthFormat;
    clear.DepthStencil.Depth = 1.0f;
    HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
        IID_PPV_ARGS(&m_depthBuffer.resource));
    if (FAILED(hr)) {
        log::Info("Native: creating the {}x{} depth buffer failed: {:#010x}", m_width, m_height, Hex(hr));
        return false;
    }
    m_depthBuffer.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (m_depthView.ptr == 0)
        m_depthView = m_dsvs.Allocate();
    D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
    view.Format = kDepthFormat;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_depthBuffer.resource.Get(), &view, m_depthView);
    return true;
}

bool Backend::CreateFrameBuffer()
{
    if (m_frameBuffer.resource != nullptr)
        Retire(std::move(m_frameBuffer.resource));
    m_frameBuffer = {};
    const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kFrameFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_frameBuffer.resource));
    if (FAILED(hr)) {
        log::Info("Native: creating the {}x{} frame buffer failed: {:#010x}", m_width, m_height, Hex(hr));
        return false;
    }
    m_frameBuffer.state = D3D12_RESOURCE_STATE_COMMON;
    if (m_frameBufferView.ptr == 0)
        m_frameBufferView = m_rtvs.Allocate();
    m_device->CreateRenderTargetView(m_frameBuffer.resource.Get(), nullptr, m_frameBufferView);
    if (m_frameBufferTexture == DescriptorHeap::kInvalid)
        m_frameBufferTexture = m_descriptors.Allocate();
    m_device->CreateShaderResourceView(m_frameBuffer.resource.Get(), nullptr, m_descriptors.Cpu(m_frameBufferTexture));
    return true;
}

bool Backend::SetMode(HWND window, const ShaderDeviceInfo& info)
{
    m_window = window;
    // Vanilla (0x1800280a0) presents windowed modes immediately whatever mat_vsync says, as Direct3D 9 could not sync
    // them well; the flip model syncs every mode, so the video option counts in a window and borderless too.
    m_vsync = info.waitForVSync && !HasArgument(L"-forcenovsync");
    RECT client = {};
    GetClientRect(window, &client);
    m_width = info.displayMode.width > 0 ? static_cast<UINT>(info.displayMode.width) : static_cast<UINT>(client.right - client.left);
    m_height = info.displayMode.height > 0 ? static_cast<UINT>(info.displayMode.height) : static_cast<UINT>(client.bottom - client.top);
    // The engine makes the window a borderless popup of the mode's size at the display's corner, which is borderless
    // full screen when the mode is the display's.
    if (!info.windowed)
        log::Info("Native: exclusive fullscreen was requested; presenting borderless at {}x{} instead", m_width, m_height);
    m_windowed = info.windowed;
    m_placedDisplay = -1;
    if (m_device == nullptr) {
        m_renderThread = GetCurrentThreadId();
        if (!CreateDevice())
            return false;
    }
    // Real-time lighting reads single-sampled targets, so MSAA is never used (SupportsMSAAMode reports it unsupported).
    if (info.aaSamples > 1)
        log::Info("Native: the game asked for {}x MSAA; drawing without it", info.aaSamples);
    return CreateSwapChain();
}

VideoMode CurrentVideoMode()
{
    const Backend& backend = GetBackend();
    VideoMode mode;
    mode.width = static_cast<int>(backend.Width());
    mode.height = static_cast<int>(backend.Height());
    mode.windowed = backend.Windowed();
    // The engine makes a borderless window a popup without a caption, as it makes its full-screen window.
    mode.borderless = mode.windowed && backend.Window() != nullptr && (GetWindowLongW(backend.Window(), GWL_STYLE) & WS_CAPTION) != WS_CAPTION;
    return mode;
}

std::vector<std::pair<int, int>> Resolutions()
{
    std::vector<std::pair<int, int>> sizes;
    for (const ShaderDisplayMode& mode : DisplayModes(static_cast<DeviceManagerObject*>(DeviceManager())->adapter))
        sizes.emplace_back(mode.width, mode.height);
    std::sort(sizes.begin(), sizes.end(), std::greater<>());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    return sizes;
}

std::vector<DisplayInfo> Displays()
{
    std::vector<DisplayInfo> displays;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFO info = {};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info))
                reinterpret_cast<std::vector<DisplayInfo>*>(param)->push_back({info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&displays));
    return displays;
}

// The engine places the window on the primary display (centred when it has a frame, at the corner when it is a
// borderless popup); a chosen display gets it placed the same way there.
void Backend::PlaceWindow()
{
    const int display = GetSettings().display;
    if (display == m_placedDisplay || m_window == nullptr)
        return;
    m_placedDisplay = display;
    if (display <= 0)
        return;
    const std::vector<DisplayInfo> displays = Displays();
    if (display > static_cast<int>(displays.size())) {
        log::Info("Native: display {} is not connected ({} displays); the window stays where it is", display, displays.size());
        return;
    }
    const RECT& area = displays[static_cast<size_t>(display) - 1].area;
    RECT window = {};
    GetWindowRect(m_window, &window);
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    const bool framed = (GetWindowLongW(m_window, GWL_STYLE) & WS_CAPTION) == WS_CAPTION;
    const int x = framed ? area.left + (area.right - area.left - width) / 2 : area.left;
    const int y = framed ? area.top + (area.bottom - area.top - height) / 2 : area.top;
    SetWindowPos(m_window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    log::Info("Native: window placed on display {} at {}, {}", display, x, y);
}

void Backend::WaitForGpu()
{
    if (m_queue == nullptr)
        return;
    m_queue->Signal(m_fence.Get(), ++m_fenceValue);
    m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);
}

// Opens the frame's command list once the GPU has finished the frame that last used its allocator, and releases
// what that frame retired.
void Backend::BeginFrame()
{
    Frame& frame = m_frames[m_frame];
    if (m_fence->GetCompletedValue() < frame.fence) {
        m_fence->SetEventOnCompletion(frame.fence, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    frame.retired.clear();
    for (UINT index : frame.retiredDescriptors)
        m_descriptors.Free(index);
    frame.retiredDescriptors.clear();
    for (UploadPage& page : frame.uploads)
        page.used = 0;
    for (UploadPage& page : frame.staging)
        page.used = 0;
    m_stagingUsed = 0;
    frame.allocator->Reset();
    m_commandList->Reset(frame.allocator.Get(), nullptr);
    ++m_listGeneration;
    m_recording = true;
    Transition(m_frameBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

namespace {

// The swap chain's buffer as presented (output.request), copied for writing out once the GPU is done.
struct OutputCapture {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    bool hdr = false;
    float paperWhite = 0;
    float peak = 0;
};

// Writes the rows of the copy tightly packed, 4 bytes a pixel: B8G8R8A8_UNORM, or R10G10B10A2_UNORM holding sRGB
// (10 bits) or PQ (HDR10).
void WriteOutputCapture(const OutputCapture& capture, unsigned presents)
{
    const D3D12_SUBRESOURCE_FOOTPRINT& footprint = capture.footprint.Footprint;
    std::uint8_t* mapped = nullptr;
    const D3D12_RANGE range = { 0, static_cast<SIZE_T>(footprint.RowPitch) * footprint.Height };
    if (FAILED(capture.readback->Map(0, &range, reinterpret_cast<void**>(&mapped))))
        return;
    std::error_code error;
    std::filesystem::create_directories(ModDirectory() / L"captures", error);
    const std::filesystem::path path = ModDirectory() / L"captures" / std::format(L"output_{}_{}x{}_format{}.raw", presents, footprint.Width, footprint.Height,
        static_cast<int>(footprint.Format));
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    for (UINT row = 0; row < footprint.Height; ++row)
        file.write(reinterpret_cast<const char*>(mapped + static_cast<size_t>(row) * footprint.RowPitch), static_cast<std::streamsize>(footprint.Width) * 4);
    capture.readback->Unmap(0, nullptr);
    log::Info("Dev: saved the presented image ({}, format {}, paper white {:.0f} nits, peak {:.0f} nits) to {}", capture.hdr ? "HDR10" : "SDR",
        static_cast<int>(footprint.Format), capture.paperWhite, capture.peak, path.string());
}

}

void Backend::Present()
{
    if (!m_recording)
        return;
    RunPending();
    UpdateOutput();
    busy::NotePresent();
    SavePipelineLibrary(false);
    // Once the game has been drawing for a few seconds (its menu is up), shaders of earlier sessions that an update
    // made stale are compiled again in the background.
    static const auto firstPresent = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - firstPresent > kReplayDelay)
        shader_precache::StartReplay(GetOptions().gameDirectory);
    static const bool developer = HasArgument(L"-gmodpp_dev");
    static unsigned presents = 0;
    bool captureOutput = false;
    if (developer) {
        static int limit = -1;
        if (++presents % kDevFilePollPresents == 0) {
            const std::filesystem::path request = ModDirectory() / L"drawlog.request";
            std::error_code ignored;
            const std::filesystem::path outputRequest = ModDirectory() / L"output.request";
            if (std::filesystem::exists(outputRequest, ignored)) {
                std::filesystem::remove(outputRequest, ignored);
                captureOutput = true;
            }
            if (std::filesystem::exists(request, ignored)) {
                int count = 200;
                std::ifstream(request) >> count;
                std::filesystem::remove(request, ignored);
                LogNextDraws(count > 0 ? count : 200);
            }
            const std::filesystem::path limitRequest = ModDirectory() / L"drawlimit.request";
            if (std::filesystem::exists(limitRequest, ignored)) {
                std::ifstream(limitRequest) >> limit;
                std::filesystem::remove(limitRequest, ignored);
            }
        }
        LimitDraws(limit);
    }
    MapToOutput();
    OutputCapture outputCapture;
    if (captureOutput) {
        const D3D12_RESOURCE_DESC desc = SwapChainBuffer().resource->GetDesc();
        UINT64 bytes = 0;
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &outputCapture.footprint, nullptr, nullptr, &bytes);
        const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK };
        const D3D12_RESOURCE_DESC buffer = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
        if (SUCCEEDED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&outputCapture.readback)))) {
            Transition(SwapChainBuffer(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_TEXTURE_COPY_LOCATION to = { outputCapture.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, { .PlacedFootprint = outputCapture.footprint } };
            const D3D12_TEXTURE_COPY_LOCATION from = { SwapChainBuffer().resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, { .SubresourceIndex = 0 } };
            m_commandList->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            const display::Mapping mapping = HudlessMapping();
            outputCapture.hdr = m_outputHdr;
            outputCapture.paperWhite = mapping.paperWhite;
            outputCapture.peak = mapping.peak * mapping.paperWhite;
        } else {
            log::Info("Dev: creating a readback buffer for the presented image failed");
        }
    }
    Transition(SwapChainBuffer(), D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->Close();
    m_recording = false;
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    anti_lag::MarkEndOfFrameRendering();
    m_fsrFrameGeneration.BeforePresent();
    streamline::MarkPresent(false);
    const HRESULT hr = m_swapChain->Present(m_vsync ? 1 : 0, !m_vsync && m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    streamline::MarkPresent(true);
    RecordFrameInterval(m_fsrFrameGeneration.Active() ? m_fsrFrameGeneration.PresentedFrames() : streamline::PresentedFrames());
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        if (!m_deviceLost) {
            m_deviceLost = true;
            log::Info("Native: the GPU device was lost ({:#010x}, reason {:#010x})", Hex(hr), Hex(m_device->GetDeviceRemovedReason()));
        }
    } else if (FAILED(hr)) {
        log::Info("Native: Present failed: {:#010x}", Hex(hr));
    }
    m_queue->Signal(m_fence.Get(), ++m_fenceValue);
    m_frames[m_frame].fence = m_fenceValue;
    if (outputCapture.readback) {
        WaitForGpu();
        WriteOutputCapture(outputCapture, presents);
    }
    DrainDebugMessages();
    m_frame = (m_frame + 1) % kFrames;
    BeginFrame();
    // The next frame starts: Reflex and Anti-Lag 2 wait here, before the game reads input for it.
    streamline::BeginFrame();
    anti_lag::Update();
}

display::Mapping Backend::HudlessMapping() const { return display::ForOutput(m_outputHdr, m_outputFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? 10 : 8); }

// How often (in presents) the display is looked for again while the window stays put: Windows' SDR white level and HDR
// switch raise no event the swap chain sees.
constexpr unsigned kDisplayPollPresents = 60;

void Backend::UpdateOutput()
{
    RECT window = {};
    GetWindowRect(m_window, &window);
    const bool moved = !EqualRect(&window, &m_displayWindow);
    const bool stale = m_displayFactory == nullptr || !m_displayFactory->IsCurrent();
    if (moved || stale || ++m_displayPresents >= kDisplayPollPresents) {
        m_displayPresents = 0;
        m_displayWindow = window;
        if (stale) {
            m_displayFactory = nullptr;
            CreateDXGIFactory1(IID_PPV_ARGS(&m_displayFactory));
        }
        std::optional<display::DisplayDesc> found = display::FindDisplay(m_displayFactory.Get(), m_device->GetAdapterLuid(), m_window);
        if (found) {
            const bool changed = !m_display || m_display->device != found->device || m_display->hdr != found->hdr || m_display->peakNits != found->peakNits ||
                m_display->sdrWhiteNits != found->sdrWhiteNits || m_display->bits != found->bits;
            if (changed) {
                log::Info("Native: display '{}' ({}), HDR {}, peak {:.0f} nits, full frame {:.0f} nits, black {:.3f} nits, SDR white {:.0f} nits, {} bits",
                    found->name, display::Utf8Name(found->device), found->hdr ? "on" : "off", found->peakNits, found->fullFrameNits, found->blackNits,
                    found->sdrWhiteNits, found->bits);
                if (!m_display || m_display->device != found->device || m_display->hdr != found->hdr)
                    m_hdrRefused = false;
            }
            m_display = std::move(found);
        }
    }
    Statistics& statistics = GetStatistics();
    if (m_display) {
        statistics.displayKnown = true;
        statistics.displayName = m_display->name;
        statistics.displayHdr = m_display->hdr;
        statistics.displayPeakNits = m_display->peakNits;
        statistics.displayFullFrameNits = m_display->fullFrameNits;
        statistics.displayBlackNits = m_display->blackNits;
        statistics.sdrWhiteNits = m_display->sdrWhiteNits;
        statistics.displayBits = m_display->bits;
    }

    const Settings& settings = GetSettings();
    if (settings.hdrOutput != m_hdrRequest) {
        m_hdrRequest = settings.hdrOutput;
        m_hdrRefused = false;
    }
    const bool displayHdr = m_display && m_display->hdr;
    const bool wantHdr = !m_hdrRefused && (settings.hdrOutput == HdrOutput::On || (settings.hdrOutput == HdrOutput::Auto && displayHdr));
    const bool tenBits = settings.outputBitDepth == OutputBits::Ten || (settings.outputBitDepth == OutputBits::Auto && m_display && m_display->bits >= 10);
    const DXGI_FORMAT format = wantHdr || tenBits ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (format != m_outputFormat || wantHdr != m_outputHdr)
        SwitchOutput(format, wantHdr);
    statistics.outputHdr = m_outputHdr;
    statistics.outputBits = m_outputFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? 10 : 8;
    statistics.hdrUnavailable = m_hdrRefused;
}

// Microsoft's D3D12HDR sample's order: the buffers are released, resized to the new format, and the colour space set
// once the swap chain says it can present it (CheckColorSpaceSupport).
void Backend::SwitchOutput(DXGI_FORMAT format, bool hdr)
{
    Flush();
    for (TrackedResource& buffer : m_backBuffers)
        buffer = {};
    m_fsrFrameGeneration.ReleaseContext();
    HRESULT hr = m_swapChain->ResizeBuffers(kBackBuffers, m_width, m_height, format, m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        log::Info("Native: resizing the swap chain's buffers to format {} failed: {:#010x}", static_cast<int>(format), Hex(hr));
        if (hdr)
            m_hdrRefused = true;
        AcquireBuffers();
        return;
    }
    m_outputFormat = format;
    if (hdr) {
        UINT support = 0;
        if (FAILED(m_swapChain->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support)) ||
            (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
            log::Info("Native: the swap chain cannot present HDR10 on this display; staying in SDR");
            m_hdrRefused = true;
            hdr = false;
        }
    }
    m_outputHdr = hdr;
    AcquireBuffers();
    streamline::SetOutputFormat(format);
    log::Info("Native: the swap chain shows {} in format {}", hdr ? "HDR10" : "SDR", static_cast<int>(format));
}

void Backend::MapToOutput()
{
    display::Mapping mapping = HudlessMapping();
    if (overlay::Visible()) {
        UINT view = DescriptorHeap::kInvalid;
        const D3D12_CPU_DESCRIPTOR_HANDLE target = display::BeginUi(m_width, m_height, view);
        if (target.ptr != 0) {
            overlay::Render(m_commandList.Get(), target);
            display::EndUi();
            mapping.uiView = view;
        }
    }
    Transition(m_frameBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    TrackedResource& presented = SwapChainBuffer();
    Transition(presented, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const bool uiAlpha = PrepareUiAlpha(mapping, m_width, m_height);
    display::Map(m_commandList.Get(), m_frameBufferTexture, m_backBufferViews[m_swapChain->GetCurrentBackBufferIndex()], m_outputFormat, m_width, m_height,
        0, 0, mapping);
    if (uiAlpha)
        TagUiAlpha();
    display::ClearColorCorrection();
}

// Logs the debug layer's errors (at most kMaxDebugMessages in all).
void Backend::DrainDebugMessages()
{
    constexpr UINT64 kMaxDebugMessages = 500;
    if (m_infoQueue == nullptr)
        return;
    const UINT64 count = m_infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < count && m_debugMessages < kMaxDebugMessages; ++i, ++m_debugMessages) {
        SIZE_T length = 0;
        if (FAILED(m_infoQueue->GetMessage(i, nullptr, &length)))
            continue;
        std::vector<std::byte> storage(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(m_infoQueue->GetMessage(i, message, &length)))
            log::Info("D3D12 [{}] {}", static_cast<int>(message->Severity), std::string_view(message->pDescription, message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0));
    }
    m_infoQueue->ClearStoredMessages();
}

void Backend::Flush()
{
    if (!m_recording)
        return;
    RunPending();
    m_commandList->Close();
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();
    Frame& frame = m_frames[m_frame];
    frame.retired.clear();
    for (UploadPage& page : frame.staging)
        page.used = 0;
    m_stagingUsed = 0;
    frame.allocator->Reset();
    m_commandList->Reset(frame.allocator.Get(), nullptr);
    ++m_listGeneration;
}

void Backend::Submit()
{
    if (!m_recording)
        return;
    RunPending();
    m_commandList->Close();
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    // A command list can be reset while it executes; its allocator keeps both recordings until the frame is done.
    m_commandList->Reset(m_frames[m_frame].allocator.Get(), nullptr);
    ++m_listGeneration;
}

Backend::Upload Backend::AllocateUpload(UINT64 size, UINT64 alignment) { return Allocate(m_frames[m_frame].uploads, size, alignment); }

Backend::Upload Backend::AllocateStaging(UINT64 size, UINT64 alignment)
{
    if (m_stagingUsed + size > kStagingFlushBytes && m_stagingUsed > 0)
        Flush();
    m_stagingUsed += size;
    return Allocate(m_frames[m_frame].staging, size, alignment);
}

UINT Backend::SamplerIndex(const D3D12_SAMPLER_DESC& desc)
{
    static_assert(sizeof(SamplerKey) == sizeof(D3D12_SAMPLER_DESC));
    SamplerKey key;
    std::memcpy(key.data(), &desc, sizeof(desc));
    auto found = m_samplers.find(key);
    if (found != m_samplers.end())
        return found->second;
    const UINT index = static_cast<UINT>(m_samplers.size());
    if (index == kSamplerHeapSize) {
        log::Info("Native: the sampler heap is full ({} samplers)", kSamplerHeapSize);
        return 0;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_samplerHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_samplerIncrement;
    m_device->CreateSampler(&desc, handle);
    m_samplers.emplace(key, index);
    return index;
}

Backend::Upload Backend::Allocate(std::vector<UploadPage>& pages, UINT64 size, UINT64 alignment)
{
    for (UploadPage& page : pages) {
        const UINT64 offset = (page.used + alignment - 1) / alignment * alignment;
        if (offset + size <= page.size) {
            page.used = offset + size;
            return { page.buffer.Get(), offset, page.data + offset };
        }
    }
    UploadPage page;
    page.size = std::max(&pages == &m_frames[m_frame].staging ? kStagingPageSize : kUploadPageSize, size);
    const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = page.size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&page.buffer));
    if (FAILED(hr) || FAILED(hr = page.buffer->Map(0, nullptr, reinterpret_cast<void**>(&page.data)))) {
        log::Info("Native: creating a {}-byte upload buffer failed: {:#010x}", page.size, Hex(hr));
        return { nullptr, 0, nullptr };
    }
    page.used = size;
    pages.push_back(page);
    return { page.buffer.Get(), 0, page.data };
}

void Backend::RunOnRenderThread(std::function<void()> work)
{
    if (OnRenderThread()) {
        work();
        return;
    }
    std::scoped_lock lock(m_pendingMutex);
    m_pending.push_back(std::move(work));
    m_hasPending = true;
}

// Not reentrant: work that flushes must not run later work before its own commands are recorded.
void Backend::RunPending()
{
    if (m_runningPending || !m_hasPending)
        return;
    std::vector<std::function<void()>> pending;
    {
        std::scoped_lock lock(m_pendingMutex);
        pending.swap(m_pending);
        m_hasPending = false;
    }
    m_runningPending = true;
    for (auto& work : pending)
        work();
    m_runningPending = false;
}

void Backend::Retire(ComPtr<ID3D12Resource> resource) { m_frames[m_frame].retired.push_back(std::move(resource)); }

void Backend::RetireDescriptor(UINT index) { m_frames[m_frame].retiredDescriptors.push_back(index); }

void Backend::Transition(TrackedResource& resource, D3D12_RESOURCE_STATES state)
{
    if (resource.resource == nullptr || resource.state == state)
        return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource.resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = resource.state;
    barrier.Transition.StateAfter = state;
    m_commandList->ResourceBarrier(1, &barrier);
    resource.state = state;
}

bool CpuDescriptorPool::Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = { type, capacity, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
    if (FAILED(hr)) {
        log::Info("Native: creating a {}-entry descriptor heap of type {} failed: {:#010x}", capacity, static_cast<int>(type), Hex(hr));
        return false;
    }
    m_increment = device->GetDescriptorHandleIncrementSize(type);
    m_capacity = capacity;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorPool::Allocate()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
    if (!m_free.empty()) {
        handle.ptr = m_free.back();
        m_free.pop_back();
    } else if (m_next < m_capacity) {
        handle.ptr = m_heap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(m_next++) * m_increment;
    } else {
        log::Info("Native: a view heap is full ({} views)", m_capacity);
    }
    return handle;
}

void CpuDescriptorPool::Free(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    if (handle.ptr != 0)
        m_free.push_back(handle.ptr);
}

namespace {

Backend g_backend;

}

Backend& GetBackend() { return g_backend; }

void RequestVideoMode(const ShaderDeviceInfo& info) { g_pendingMode = info; }

NativeObject* DeviceManager()
{
    static NativeClass nativeClass = [] {
        NativeClass c("IShaderDeviceMgr", { shim::kShaderDeviceMgrSlots, shim::kShaderDeviceMgrSlotsCount }, kDeviceManagerSlots);
        c.Bind("Connect", &Connect);
        c.Bind("Disconnect", &Disconnect);
        c.Bind("QueryInterface", &QueryInterface);
        c.Bind("Init", &Init);
        c.Bind("Shutdown", &Shutdown);
        c.Bind("GetAdapterCount", &GetAdapterCount);
        c.Bind("GetAdapterInfo", &GetAdapterInfo);
        c.Bind("GetRecommendedConfigurationInfo", &GetRecommendedConfigurationInfo);
        c.Bind("GetModeCount", &GetModeCount);
        c.Bind("GetModeInfo", &GetModeInfo);
        c.Bind("GetCurrentModeInfo", &GetCurrentModeInfo);
        c.Bind("SetAdapter", &SetAdapter);
        c.Bind("SetMode", &SetMode);
        c.Bind("AddModeChangeCallback", &AddModeChangeCallback);
        c.Bind("RemoveModeChangeCallback", &RemoveModeChangeCallback);
        return c;
    }();
    static DeviceManagerObject object{ { nativeClass.Vtable(), &nativeClass } };
    return &object;
}

NativeObject* Device()
{
    static NativeClass nativeClass = [] {
        NativeClass c("IShaderDevice", { shim::kShaderDeviceSlots, shim::kShaderDeviceSlotsCount }, kDeviceSlots);
        c.Bind("ReleaseResources", &ReleaseResources);
        c.Bind("ReacquireResources", &ReacquireResources);
        c.Bind("GetBackBufferFormat", &GetBackBufferFormat);
        c.Bind("GetBackBufferDimensions", &GetBackBufferDimensions);
        c.Bind("GetCurrentAdapter", &GetCurrentAdapter);
        c.Bind("IsUsingGraphics", &IsUsingGraphics);
        c.Bind("StencilBufferBits", &StencilBufferBits);
        c.Bind("IsAAEnabled", &IsAAEnabled);
        c.Bind("Present", &Present);
        c.Bind("GetWindowSize", &GetWindowSize);
        c.Bind("SetView", &SetView);
        c.Bind("SetHardwareGammaRamp", &SetHardwareGammaRamp);
        BindMeshDeviceMethods(c);
        return c;
    }();
    static NativeObject object{ nativeClass.Vtable(), &nativeClass };
    return &object;
}

}
