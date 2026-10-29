#include "shaderapi_device.h"

#include "command_line.h"
#include "d3d12_device.h"
#include "log.h"

#include <algorithm>
#include <cstring>
#include <set>
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

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

// Direct3D 9 numbers one adapter per display output; DXGI enumerates the adapter holding the primary desktop
// output first, and its outputs in order.
struct DisplayAdapter {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
};

std::vector<DisplayAdapter> DisplayAdapters()
{
    std::vector<DisplayAdapter> result;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return result;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
            result.push_back({ adapter, output });
    }
    return result;
}

std::vector<ShaderDisplayMode> DisplayModes(int adapterIndex)
{
    std::vector<ShaderDisplayMode> modes;
    std::vector<DisplayAdapter> adapters = DisplayAdapters();
    if (adapterIndex < 0 || adapterIndex >= static_cast<int>(adapters.size()))
        return modes;
    IDXGIOutput* output = adapters[adapterIndex].output.Get();
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

struct DeviceManagerObject : NativeObject {
    int adapter = 0;
    std::vector<void (*)()> modeChangeCallbacks;
};

bool Connect(DeviceManagerObject*, CreateInterfaceFn factory)
{
    GetBackend().shaderUtil = factory != nullptr ? factory("VShaderUtil001", nullptr) : nullptr;
    if (GetBackend().shaderUtil == nullptr)
        log::Info("Native: materialsystem's VShaderUtil001 is not available");
    return GetBackend().shaderUtil != nullptr;
}

void Disconnect(DeviceManagerObject*) { GetBackend().shaderUtil = nullptr; }
void* QueryInterface(DeviceManagerObject*, const char* name) { return CreateInterface(name, nullptr); }
InitReturnVal Init(DeviceManagerObject*) { return kInitOk; }
void Shutdown(DeviceManagerObject*) {}
int GetAdapterCount(DeviceManagerObject*) { return static_cast<int>(DisplayAdapters().size()); }

void GetAdapterInfo(DeviceManagerObject*, int adapterIndex, MaterialAdapterInfo& info)
{
    std::memset(&info, 0, sizeof(info));
    std::vector<DisplayAdapter> adapters = DisplayAdapters();
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
    std::vector<ShaderDisplayMode> modes = DisplayModes(adapterIndex);
    if (mode >= 0 && mode < static_cast<int>(modes.size()))
        *info = modes[mode];
}

void GetCurrentModeInfo(DeviceManagerObject*, ShaderDisplayMode* info, int adapterIndex)
{
    std::vector<DisplayAdapter> adapters = DisplayAdapters();
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
    return GetBackend().SetMode(static_cast<HWND>(window), info) ? &CreateInterface : nullptr;
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
bool IsAAEnabled(NativeObject*) { return false; }
void Present(NativeObject*) { GetBackend().Present(); }

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
    std::vector<DisplayAdapter> adapters = DisplayAdapters();
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
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBackBuffers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(hr = m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)))) {
        log::Info("Native: creating the back buffer descriptors failed: {:#010x}", Hex(hr));
        return false;
    }
    if (!m_descriptors.Initialize(m_device.Get(), kDescriptorCapacity))
        return false;
    m_rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
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
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
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
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBackBuffers; ++i) {
        m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i].resource));
        m_backBuffers[i].state = D3D12_RESOURCE_STATE_PRESENT;
        m_device->CreateRenderTargetView(m_backBuffers[i].resource.Get(), nullptr, { rtv.ptr + i * m_rtvSize });
    }
    log::Info("Native: swap chain {}x{}, vsync {}", m_width, m_height, m_vsync ? "on" : "off");
    BeginFrame();
    return true;
}

bool Backend::SetMode(HWND window, const ShaderDeviceInfo& info)
{
    m_window = window;
    m_vsync = info.waitForVSync;
    RECT client = {};
    GetClientRect(window, &client);
    m_width = info.displayMode.width > 0 ? static_cast<UINT>(info.displayMode.width) : static_cast<UINT>(client.right - client.left);
    m_height = info.displayMode.height > 0 ? static_cast<UINT>(info.displayMode.height) : static_cast<UINT>(client.bottom - client.top);
    if (!info.windowed)
        log::Info("Native: exclusive fullscreen was requested; presenting borderless at {}x{} instead", m_width, m_height);
    if (m_device == nullptr && !CreateDevice())
        return false;
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
    frame.allocator->Reset();
    m_commandList->Reset(frame.allocator.Get(), nullptr);
    m_recording = true;
    Transition(m_backBuffers[m_swapChain->GetCurrentBackBufferIndex()], D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void Backend::Present()
{
    if (!m_recording)
        return;
    Transition(m_backBuffers[m_swapChain->GetCurrentBackBufferIndex()], D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->Close();
    m_recording = false;
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    const HRESULT hr = m_swapChain->Present(m_vsync ? 1 : 0, !m_vsync && m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr))
        log::Info("Native: Present failed: {:#010x}", Hex(hr));
    m_queue->Signal(m_fence.Get(), ++m_fenceValue);
    m_frames[m_frame].fence = m_fenceValue;
    m_frame = (m_frame + 1) % kFrames;
    BeginFrame();
}

Backend::Upload Backend::AllocateUpload(UINT64 size, UINT64 alignment)
{
    std::vector<UploadPage>& pages = m_frames[m_frame].uploads;
    for (UploadPage& page : pages) {
        const UINT64 offset = (page.used + alignment - 1) / alignment * alignment;
        if (offset + size <= page.size) {
            page.used = offset + size;
            return { page.buffer.Get(), offset, page.data + offset };
        }
    }
    UploadPage page;
    page.size = std::max(kUploadPageSize, size);
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

Backend& GetBackend()
{
    static Backend backend;
    return backend;
}

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
        return c;
    }();
    static NativeObject object{ nativeClass.Vtable(), &nativeClass };
    return &object;
}

}
