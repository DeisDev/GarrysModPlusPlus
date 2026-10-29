#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <filesystem>

namespace gmodpp {

// A device factory that loads the Agility SDK's D3D12Core.dll from `runtimeDirectory` for this process's devices
// alone (independent devices; no developer mode or exported SDK version needed), or nothing where the system's D3D12
// cannot do that.
Microsoft::WRL::ComPtr<ID3D12DeviceFactory> AgilityDeviceFactory(const std::filesystem::path& runtimeDirectory);

}
