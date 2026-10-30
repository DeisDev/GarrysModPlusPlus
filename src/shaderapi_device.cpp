#include "shaderapi_device.h"

#include "command_line.h"
#include "d3d12_device.h"
#include "log.h"
#include "native_lighting.h"
#include "overlay.h"
#include "shader_prewarm.h"
#include "shaderapi.h"
#include "shaderapi_meshes.h"
#include "shaderapi_pipelines.h"

#include <algorithm>
#include <cstring>
#include <fstream>
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

// The shaders are compiled ahead the first time, before the game opens its window.
bool Connect(DeviceManagerObject*, CreateInterfaceFn factory)
{
    const std::filesystem::path game = ExecutablePath().parent_path();
    if (!PrewarmIsCurrent(game))
        PrewarmWithProgressWindow(game, game / L"bin" / L"win64" / L"gmodpp");
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
void Shutdown(DeviceManagerObject*) { SavePipelineLibrary(true); }
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
ImageFormat GetBackBufferFormat(NativeObject*) { return kImageFormatBgra8888; }

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
bool IsAAEnabled(NativeObject*) { return GetBackend().Samples() > 1; }
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
}

void GetWindowSize(NativeObject*, int& width, int& height)
{
    RECT rect = {};
    GetClientRect(GetBackend().Window(), &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
}

// Windowed Direct3D 9 does not change the gamma ramp, and the game runs windowed or borderless here.
void SetHardwareGammaRamp(NativeObject*, float, float, float, float, bool) {}

}

bool Backend::CreateDevice()
{
    const std::filesystem::path runtime = ExecutablePath().parent_path() / L"bin" / L"win64" / L"D3D12";
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
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue))) || FAILED(hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
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
        HRESULT hr = m_swapChain->ResizeBuffers(kBackBuffers, m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM, m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        if (FAILED(hr)) {
            log::Info("Native: ResizeBuffers failed: {:#010x}", Hex(hr));
            return false;
        }
    } else {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        // Copies of the back buffer into textures (StretchRect) read it in a shader.
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        desc.BufferCount = kBackBuffers;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> swapChain;
        HRESULT hr = m_factory->CreateSwapChainForHwnd(m_queue.Get(), m_window, &desc, nullptr, nullptr, &swapChain);
        if (FAILED(hr) || FAILED(hr = swapChain.As(&m_swapChain))) {
            log::Info("Native: CreateSwapChainForHwnd failed: {:#010x}", Hex(hr));
            return false;
        }
        m_factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);
    }
    for (UINT i = 0; i < kBackBuffers; ++i) {
        m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i].resource));
        m_backBuffers[i].state = D3D12_RESOURCE_STATE_PRESENT;
        // Flip-model buffers accept an sRGB view of their UNORM format (DXGI flip model documentation).
        for (UINT srgb = 0; srgb < 2; ++srgb) {
            D3D12_CPU_DESCRIPTOR_HANDLE& view = m_backBufferViews[i * 2 + srgb];
            if (view.ptr == 0)
                view = m_rtvs.Allocate();
            D3D12_RENDER_TARGET_VIEW_DESC desc = {};
            desc.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : kBackBufferFormat;
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            m_device->CreateRenderTargetView(m_backBuffers[i].resource.Get(), &desc, view);
        }
        if (m_backBufferTextures[i] == DescriptorHeap::kInvalid)
            m_backBufferTextures[i] = m_descriptors.Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = kBackBufferFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_backBuffers[i].resource.Get(), &view, m_descriptors.Cpu(m_backBufferTextures[i]));
    }
    if (!CreateMultisampleTarget() || !CreateDepthBuffer())
        return false;
    log::Info("Native: swap chain {}x{}, {} samples, vsync {}", m_width, m_height, m_samples, m_vsync ? "on" : "off");
    BeginFrame();
    return true;
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
    desc.SampleDesc.Count = m_samples;
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
    view.ViewDimension = m_samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_depthBuffer.resource.Get(), &view, m_depthView);
    return true;
}

// Flip-model swap chains cannot be multisampled (DXGI_SWAP_CHAIN_DESC1), so with MSAA the game draws into a
// multisampled target that is resolved into the swap chain's buffer.
bool Backend::CreateMultisampleTarget()
{
    if (m_multisampleTarget.resource != nullptr)
        Retire(std::move(m_multisampleTarget.resource));
    m_multisampleTarget = {};
    if (m_samples == 1)
        return true;
    const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    desc.SampleDesc.Count = m_samples;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
        IID_PPV_ARGS(&m_multisampleTarget.resource));
    if (FAILED(hr)) {
        log::Info("Native: creating the {}x{} target with {} samples failed: {:#010x}", m_width, m_height, m_samples, Hex(hr));
        return false;
    }
    m_multisampleTarget.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    for (UINT srgb = 0; srgb < 2; ++srgb) {
        if (m_multisampleViews[srgb].ptr == 0)
            m_multisampleViews[srgb] = m_rtvs.Allocate();
        D3D12_RENDER_TARGET_VIEW_DESC view = {};
        view.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : kBackBufferFormat;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        m_device->CreateRenderTargetView(m_multisampleTarget.resource.Get(), &view, m_multisampleViews[srgb]);
    }
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Backend::BackBufferView(bool srgb) const
{
    if (m_samples > 1)
        return m_multisampleViews[srgb ? 1 : 0];
    return m_backBufferViews[m_swapChain->GetCurrentBackBufferIndex() * 2 + (srgb ? 1 : 0)];
}

TrackedResource& Backend::ResolvedBackBuffer()
{
    TrackedResource& buffer = SwapChainBuffer();
    if (m_samples == 1)
        return buffer;
    Transition(m_multisampleTarget, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    Transition(buffer, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    m_commandList->ResolveSubresource(buffer.resource.Get(), 0, m_multisampleTarget.resource.Get(), 0, kBackBufferFormat);
    return buffer;
}

bool Backend::SetMode(HWND window, const ShaderDeviceInfo& info)
{
    m_window = window;
    // 0x1800280a0: windowed modes present immediately whatever mat_vsync says; fullscreen waits for vsync unless
    // -forcenovsync.
    m_vsync = !info.windowed && info.waitForVSync && !HasArgument(L"-forcenovsync");
    RECT client = {};
    GetClientRect(window, &client);
    m_width = info.displayMode.width > 0 ? static_cast<UINT>(info.displayMode.width) : static_cast<UINT>(client.right - client.left);
    m_height = info.displayMode.height > 0 ? static_cast<UINT>(info.displayMode.height) : static_cast<UINT>(client.bottom - client.top);
    if (!info.windowed)
        log::Info("Native: exclusive fullscreen was requested; presenting borderless at {}x{} instead", m_width, m_height);
    if (m_device == nullptr) {
        m_renderThread = GetCurrentThreadId();
        if (!CreateDevice())
            return false;
    }
    // D3DMULTISAMPLE_TYPE counts samples; Direct3D 9's vendor quality levels have no Direct3D 12 equivalent, so
    // aaQuality is not used.
    const UINT samples = static_cast<UINT>(std::max(info.aaSamples, 1));
    for (DXGI_FORMAT format : { kBackBufferFormat, kDepthFormat }) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = { format, samples };
        if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) || levels.NumQualityLevels == 0) {
            log::Info("Native: {} samples per pixel are not supported", samples);
            return false;
        }
    }
    m_samples = samples;
    return CreateSwapChain();
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
    Transition(BackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void Backend::Present()
{
    if (!m_recording)
        return;
    RunPending();
    SavePipelineLibrary(false);
    static const bool developer = HasArgument(L"-gmodpp_dev");
    if (developer) {
        static int limit = -1;
        static unsigned presents = 0;
        if (++presents % kDevFilePollPresents == 0) {
            const std::filesystem::path request = ExecutablePath().parent_path() / L"gmodpp_drawlog.request";
            std::error_code ignored;
            if (std::filesystem::remove(request, ignored))
                LogNextDraws(200);
            const std::filesystem::path limitRequest = ExecutablePath().parent_path() / L"gmodpp_drawlimit.request";
            if (std::filesystem::exists(limitRequest, ignored)) {
                std::ifstream(limitRequest) >> limit;
                std::filesystem::remove(limitRequest, ignored);
            }
        }
        LimitDraws(limit);
    }
    TrackedResource& presented = ResolvedBackBuffer();
    if (overlay::Open()) {
        Transition(presented, D3D12_RESOURCE_STATE_RENDER_TARGET);
        overlay::Render(m_commandList.Get(), m_backBufferViews[m_swapChain->GetCurrentBackBufferIndex() * 2]);
    }
    Transition(presented, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->Close();
    m_recording = false;
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    const HRESULT hr = m_swapChain->Present(m_vsync ? 1 : 0, !m_vsync && m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr))
        log::Info("Native: Present failed: {:#010x}", Hex(hr));
    m_queue->Signal(m_fence.Get(), ++m_fenceValue);
    m_frames[m_frame].fence = m_fenceValue;
    DrainDebugMessages();
    m_frame = (m_frame + 1) % kFrames;
    BeginFrame();
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
        c.Bind("SetHardwareGammaRamp", &SetHardwareGammaRamp);
        BindMeshDeviceMethods(c);
        return c;
    }();
    static NativeObject object{ nativeClass.Vtable(), &nativeClass };
    return &object;
}

}
