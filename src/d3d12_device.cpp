#include "d3d12_device.h"

#include "log.h"

#include <string>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

// Side-by-side runtimes are named relative to the executable.
std::string RelativeToExecutable(const std::filesystem::path& directory)
{
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return {};
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(directory, std::filesystem::path(exePath).parent_path(), error);
    return error ? std::string() : relative.string() + "\\";
}

}

ComPtr<ID3D12DeviceFactory> AgilityDeviceFactory(const std::filesystem::path& runtimeDirectory)
{
    ComPtr<ID3D12SDKConfiguration1> configuration;
    HRESULT hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&configuration));
    if (FAILED(hr)) {
        log::Info("D3D12 SDK configuration unavailable ({:#010x}); using the system D3D12 runtime", static_cast<unsigned>(hr));
        return nullptr;
    }
    const std::string path = RelativeToExecutable(runtimeDirectory);
    ComPtr<ID3D12DeviceFactory> factory;
    hr = configuration->CreateDeviceFactory(D3D12_SDK_VERSION, path.c_str(), IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        log::Info("Agility SDK {} runtime at '{}' could not be loaded ({:#010x}); using the system D3D12 runtime", D3D12_SDK_VERSION, path,
            static_cast<unsigned>(hr));
        return nullptr;
    }
    log::Info("Agility SDK {} runtime loaded from '{}'", D3D12_SDK_VERSION, path);
    return factory;
}

RaytracingSupport QueryRaytracing(ID3D12Device* device)
{
    RaytracingSupport support;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    support.tier = options5.RaytracingTier;
    // A runtime that does not know the shader model asked about fails the query, so ask for 6.6 after 6.9.
    for (D3D_SHADER_MODEL highest : {D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_6}) {
        D3D12_FEATURE_DATA_SHADER_MODEL query = {highest};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &query, sizeof(query)))) {
            support.shaderModel = query.HighestShaderModel;
            break;
        }
    }
    support.inlineRaytracing = support.tier >= D3D12_RAYTRACING_TIER_1_1 && support.shaderModel >= D3D_SHADER_MODEL_6_6;
    support.opacityMicromaps = support.tier >= D3D12_RAYTRACING_TIER_1_2 && support.shaderModel >= D3D_SHADER_MODEL_6_9;
    return support;
}

}
