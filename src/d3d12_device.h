#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <filesystem>

namespace gmodpp {

// The frame the game draws into and the lighting composites into (native::Backend): linear light times the tonemap
// scale, 1 = paper white.
inline constexpr DXGI_FORMAT kFrameFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// A device factory that loads the Agility SDK's D3D12Core.dll from `runtimeDirectory` for this process's devices
// alone (independent devices; no developer mode or exported SDK version needed), or nothing where the system's D3D12
// cannot do that. Made on the first call, which names the directory; the launch check and the backend share it.
Microsoft::WRL::ComPtr<ID3D12DeviceFactory> AgilityDeviceFactory(const std::filesystem::path& runtimeDirectory);

struct RaytracingSupport {
    D3D12_RAYTRACING_TIER tier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;
    // Inline ray tracing in Shader Model 6.6, which the lighting needs, with what its passes rely on besides:
    // ResourceDescriptorHeap (resource binding tier 3) and typed UAV loads of RGBA16F (the additional formats).
    bool inlineRaytracing = false;
    bool bindingTier3 = false;
    bool typedUavLoads = false;
    // Raytracing tier 1.2 and Shader Model 6.9, through the Agility SDK runtime.
    bool opacityMicromaps = false;
    // The radiance cache (NVIDIA SHaRC) needs native 16-bit shader types and 64-bit integer shader operations; its
    // 64-bit atomics are on a root descriptor, which Shader Model 6.6 guarantees with them.
    bool radianceCache = false;
};
RaytracingSupport QueryRaytracing(ID3D12Device* device);

}
