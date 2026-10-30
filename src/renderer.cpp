#include "renderer.h"

#include "d3d12_device.h"
#include "log.h"
#include "mod.h"
#include "presenter.h"

#include <d3d9on12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

constexpr UINT kDescriptorCapacity = 1 << 16;

// Never destroyed: releasing D3D objects during DLL_PROCESS_DETACH is unsafe.
Renderer* g_renderer = nullptr;

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

std::string Utf8(const wchar_t* text)
{
    char buffer[256];
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, sizeof(buffer), nullptr, nullptr);
    return std::string(buffer, length > 0 ? length - 1 : 0);
}

void LogDisplayTopology(IDXGIFactory4* factory)
{
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        log::Info("Adapter {}: '{}' LUID {:08x}:{:08x}", a, Utf8(desc.Description), static_cast<unsigned>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart);
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC outputDesc;
            output->GetDesc(&outputDesc);
            const RECT& r = outputDesc.DesktopCoordinates;
            log::Info("  Output {}: '{}' desktop ({}, {})-({}, {}) monitor {}", o, Utf8(outputDesc.DeviceName), r.left, r.top, r.right, r.bottom,
                static_cast<void*>(outputDesc.Monitor));
        }
    }
}

// The Agility SDK runtime is deployed to the D3D12 subdirectory of this DLL's directory.
std::filesystem::path AgilityRuntimeDirectory()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&AgilityRuntimeDirectory), &module);
    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileNameW(module, modulePath, MAX_PATH) == 0)
        return {};
    return std::filesystem::path(modulePath).parent_path() / L"D3D12";
}

}

Renderer* GetRenderer() { return g_renderer; }

Renderer* Renderer::Create(LUID adapterLuid, const Options& options)
{
    if (g_renderer) {
        if (g_renderer->m_adapterLuid.LowPart == adapterLuid.LowPart && g_renderer->m_adapterLuid.HighPart == adapterLuid.HighPart)
            return g_renderer;
        log::Info("Direct3D 9 switched adapters; this is not supported yet");
        return nullptr;
    }

    ComPtr<ID3D12DeviceFactory> agility = AgilityDeviceFactory(AgilityRuntimeDirectory());
    if (options.d3d12Debug) {
        ComPtr<ID3D12Debug1> debug;
        const HRESULT debugResult = agility ? agility->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&debug)) : D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (SUCCEEDED(debugResult)) {
            debug->EnableDebugLayer();
            debug->SetEnableGPUBasedValidation(options.gpuValidation);
        } else {
            log::Info("D3D12 debug layer unavailable; install the Windows 'Graphics Tools' optional feature");
        }
    }

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        log::Info("CreateDXGIFactory2 failed: {:#010x}", Hex(hr));
        return nullptr;
    }

    if (options.devMode)
        LogDisplayTopology(factory.Get());

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) {
        log::Info("EnumAdapterByLuid failed: {:#010x}", Hex(hr));
        return nullptr;
    }
    DXGI_ADAPTER_DESC1 adapterDesc;
    adapter->GetDesc1(&adapterDesc);
    char adapterName[128];
    WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, adapterName, sizeof(adapterName), nullptr, nullptr);

    auto renderer = std::make_unique<Renderer>();
    renderer->m_adapterLuid = adapterLuid;
    hr = agility ? agility->CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&renderer->m_device))
                 : D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&renderer->m_device));
    if (FAILED(hr)) {
        log::Info("D3D12CreateDevice on '{}' failed: {:#010x}", adapterName, Hex(hr));
        return nullptr;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = renderer->m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&renderer->m_queue));
    if (FAILED(hr)) {
        log::Info("CreateCommandQueue failed: {:#010x}", Hex(hr));
        return nullptr;
    }

    if (!renderer->m_descriptors.Initialize(renderer->m_device.Get(), kDescriptorCapacity))
        return nullptr;

    const RaytracingSupport support = QueryRaytracing(renderer->m_device.Get());
    renderer->m_raytracing = support.inlineRaytracing;
    renderer->m_opacityMicromaps = support.opacityMicromaps;

    if (options.d3d12Debug && SUCCEEDED(renderer->m_device.As(&renderer->m_infoQueue))) {
        D3D12_MESSAGE_SEVERITY hidden[] = {D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE, D3D12_MESSAGE_SEVERITY_WARNING};
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumSeverities = static_cast<UINT>(std::size(hidden));
        filter.DenyList.pSeverityList = hidden;
        renderer->m_infoQueue->PushStorageFilter(&filter);
    }

    log::Info("D3D12 device on '{}': raytracing tier {:#x}, shader model {:#x}, inline raytracing {}, opacity micromaps {}", adapterName,
        static_cast<int>(support.tier), static_cast<int>(support.shaderModel),
        renderer->m_raytracing ? "available" : "unavailable", renderer->m_opacityMicromaps ? "available" : "unavailable");

    g_renderer = renderer.release();
    return g_renderer;
}

bool Renderer::AttachGameDevice(IDirect3DDevice9* device, HWND focusWindow, const D3DPRESENT_PARAMETERS& params)
{
    ComPtr<IDirect3DDevice9On12> device9On12;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device9On12));
    if (FAILED(hr)) {
        log::Info("Game device is not running on D3D9On12 ({:#010x}); effects disabled", Hex(hr));
        return false;
    }

    ComPtr<ID3D12Device> underlying;
    hr = device9On12->GetD3D12Device(IID_PPV_ARGS(&underlying));
    if (FAILED(hr)) {
        log::Info("IDirect3DDevice9On12::GetD3D12Device failed: {:#010x}", Hex(hr));
        return false;
    }

    ComPtr<IUnknown> ours, theirs;
    m_device.As(&ours);
    underlying.As(&theirs);
    if (ours != theirs) {
        log::Info("D3D9On12 is not using the GarrysMod++ D3D12 device; effects disabled");
        return false;
    }

    log::Info("Game device attached to the shared D3D12 device");
    if (!m_presenter) {
        auto presenter = std::make_unique<Presenter>(*this);
        if (presenter->Initialize(device, focusWindow, params))
            m_presenter = std::move(presenter);
    }
    return true;
}

void Renderer::OnPresent()
{
    ++m_frame;
    if (GetOptions().devMode && m_frame % 300 == 0)
        log::Info("Frame {}", m_frame);
    DrainDebugMessages();
}

void Renderer::OnReset()
{
    log::Info("Device reset at frame {}", m_frame);
}

void Renderer::DrainDebugMessages()
{
    if (!m_infoQueue)
        return;
    UINT64 count = m_infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T length = 0;
        if (FAILED(m_infoQueue->GetMessage(i, nullptr, &length)))
            continue;
        auto storage = std::make_unique<std::byte[]>(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.get());
        if (SUCCEEDED(m_infoQueue->GetMessage(i, message, &length)))
            log::Info("D3D12 [{}] {}", static_cast<int>(message->Severity), std::string_view(message->pDescription, message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0));
    }
    m_infoQueue->ClearStoredMessages();
}

}
