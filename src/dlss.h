#pragma once

#include "settings.h"

#include <d3d12.h>

#include <filesystem>

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace gmodpp {

// NVIDIA DLSS Ray Reconstruction (NGX): denoises the noisy ray-traced image and upscales it to the output size.
class RayReconstruction {
public:
    struct Inputs {
        ID3D12Resource* color;
        ID3D12Resource* depth;
        ID3D12Resource* motion;
        ID3D12Resource* normals;
        ID3D12Resource* diffuseAlbedo;
        ID3D12Resource* specularAlbedo;
        ID3D12Resource* roughness;
        ID3D12Resource* specularHitDistance;
        ID3D12Resource* output;
        // Row-major, for row vectors (v' = v M); the hit distances need them.
        const float* worldToView;
        const float* viewToClip;
        UINT width;
        UINT height;
        // How far this frame's image is shifted by the jitter, in render pixels (x right, y down).
        float jitterX;
        float jitterY;
        bool reset;
    };

    ~RayReconstruction();
    // Loads NGX on the device, finding nvngx_dlssd.dll in `featureDirectory` and writing NGX logs to
    // `logDirectory`. Returns false, with a log line, when Ray Reconstruction is unavailable.
    bool Initialize(ID3D12Device* device, const std::filesystem::path& featureDirectory, const std::filesystem::path& logDirectory);
    bool Usable() const { return m_available && !m_failed; }
    // The render size NVIDIA recommends for a mode and output size; false when the mode is unavailable.
    bool RenderSize(UpscalingMode mode, UINT outputWidth, UINT outputHeight, UINT& width, UINT& height);
    // Records the evaluation into `list`, creating the feature for this mode and these sizes first when needed.
    // Inputs must be in NON_PIXEL_SHADER_RESOURCE and the output in UNORDERED_ACCESS. The list's descriptor
    // heaps and root signature must be set again afterwards.
    bool Evaluate(ID3D12GraphicsCommandList* list, UpscalingMode mode, UINT outputWidth, UINT outputHeight, const Inputs& inputs);
    // Frees the feature; the GPU must be done with it.
    void ReleaseFeature();
    // True when Evaluate would have to create a new feature, which needs the GPU idle if one exists.
    bool NeedsNewFeature(UpscalingMode mode, UINT width, UINT height, UINT outputWidth, UINT outputHeight) const;

private:
    void Shutdown();

    ID3D12Device* m_device = nullptr;
    NVSDK_NGX_Parameter* m_parameters = nullptr;
    NVSDK_NGX_Handle* m_feature = nullptr;
    bool m_available = false;
    bool m_failed = false;
    UpscalingMode m_featureMode = UpscalingMode::Count;
    UINT m_featureSize[4] = {};
};

}
