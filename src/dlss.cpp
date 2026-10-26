#include "dlss.h"

#include "log.h"

#include <nvsdk_ngx_helpers_dlssd_d3d.h>

#include <algorithm>
#include <iterator>

namespace gmodpp {

namespace {

// NGX identifies a custom engine by a GUID-like project ID.
constexpr const char* kProjectId = "6b1f0c2e-9d4a-4f3e-8a57-2c1b9e7d4f10";
constexpr const char* kEngineVersion = "1.0";

NVSDK_NGX_PerfQuality_Value QualityValue(DlssMode mode)
{
    constexpr NVSDK_NGX_PerfQuality_Value kValues[] = {NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    static_assert(std::size(kValues) == static_cast<size_t>(DlssMode::Count));
    return kValues[static_cast<size_t>(mode)];
}

unsigned Hex(NVSDK_NGX_Result result) { return static_cast<unsigned>(result); }

}

RayReconstruction::~RayReconstruction() { Shutdown(); }

bool RayReconstruction::Initialize(ID3D12Device* device, const std::filesystem::path& featureDirectory, const std::filesystem::path& logDirectory)
{
    const std::wstring directory = featureDirectory.wstring();
    const wchar_t* paths[] = {directory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    NVSDK_NGX_Result result =
        NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, logDirectory.c_str(), device, &info);
    if (NVSDK_NGX_FAILED(result)) {
        log::Info("DLSS: NGX initialization failed ({:#010x}); Ray Reconstruction unavailable", Hex(result));
        return false;
    }
    m_device = device;
    result = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_parameters);
    if (NVSDK_NGX_FAILED(result)) {
        log::Info("DLSS: reading NGX capabilities failed ({:#010x}); Ray Reconstruction unavailable", Hex(result));
        return false;
    }
    int available = 0;
    int needsDriver = 0;
    NVSDK_NGX_Parameter_GetI(m_parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
    NVSDK_NGX_Parameter_GetI(m_parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needsDriver);
    m_available = available != 0;
    if (!m_available)
        log::Info("DLSS: Ray Reconstruction unavailable{}", needsDriver != 0 ? " (the driver needs updating)" : "");
    else
        log::Info("DLSS: Ray Reconstruction available");
    return m_available;
}

void RayReconstruction::Shutdown()
{
    ReleaseFeature();
    if (m_parameters != nullptr)
        NVSDK_NGX_D3D12_DestroyParameters(m_parameters);
    m_parameters = nullptr;
    if (m_device != nullptr)
        NVSDK_NGX_D3D12_Shutdown1(m_device);
    m_device = nullptr;
    m_available = false;
}

bool RayReconstruction::RenderSize(DlssMode mode, UINT outputWidth, UINT outputHeight, UINT& width, UINT& height)
{
    if (!m_available)
        return false;
    unsigned int maxWidth = 0, maxHeight = 0, minWidth = 0, minHeight = 0;
    float sharpness = 0.0f;
    width = height = 0;
    const NVSDK_NGX_Result result = NGX_DLSSD_GET_OPTIMAL_SETTINGS(
        m_parameters, outputWidth, outputHeight, QualityValue(mode), &width, &height, &maxWidth, &maxHeight, &minWidth, &minHeight, &sharpness);
    return NVSDK_NGX_SUCCEED(result) && width != 0 && height != 0;
}

bool RayReconstruction::NeedsNewFeature(DlssMode mode, UINT width, UINT height, UINT outputWidth, UINT outputHeight) const
{
    const UINT size[4] = {width, height, outputWidth, outputHeight};
    return m_feature == nullptr || mode != m_featureMode || !std::equal(std::begin(size), std::end(size), std::begin(m_featureSize));
}

void RayReconstruction::ReleaseFeature()
{
    if (m_feature != nullptr)
        NVSDK_NGX_D3D12_ReleaseFeature(m_feature);
    m_feature = nullptr;
    m_featureMode = DlssMode::Count;
}

bool RayReconstruction::Evaluate(ID3D12GraphicsCommandList* list, DlssMode mode, UINT outputWidth, UINT outputHeight, const Inputs& inputs)
{
    if (!m_available || m_failed)
        return false;
    if (NeedsNewFeature(mode, inputs.width, inputs.height, outputWidth, outputHeight)) {
        ReleaseFeature();
        NVSDK_NGX_DLSSD_Create_Params create = {};
        create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;
        create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
        create.InWidth = inputs.width;
        create.InHeight = inputs.height;
        create.InTargetWidth = outputWidth;
        create.InTargetHeight = outputHeight;
        create.InPerfQualityValue = QualityValue(mode);
        // Colour is linear HDR; motion vectors are at render resolution and exclude the jitter.
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSSD_EXT(list, 1, 1, &m_feature, m_parameters, &create);
        if (NVSDK_NGX_FAILED(result)) {
            log::Info("DLSS: creating Ray Reconstruction ({}x{} to {}x{}) failed ({:#010x}); falling back to TAA", inputs.width, inputs.height,
                outputWidth, outputHeight, Hex(result));
            m_feature = nullptr;
            m_failed = true;
            return false;
        }
        m_featureMode = mode;
        m_featureSize[0] = inputs.width;
        m_featureSize[1] = inputs.height;
        m_featureSize[2] = outputWidth;
        m_featureSize[3] = outputHeight;
        log::Info("DLSS: Ray Reconstruction {}x{} to {}x{}", inputs.width, inputs.height, outputWidth, outputHeight);
    }

    NVSDK_NGX_D3D12_DLSSD_Eval_Params eval = {};
    eval.pInColor = inputs.color;
    eval.pInOutput = inputs.output;
    eval.pInDepth = inputs.depth;
    eval.pInMotionVectors = inputs.motion;
    eval.pInNormals = inputs.normals;
    eval.pInRoughness = inputs.roughness;
    eval.pInDiffuseAlbedo = inputs.diffuseAlbedo;
    eval.pInSpecularAlbedo = inputs.specularAlbedo;
    eval.InJitterOffsetX = inputs.jitterX;
    eval.InJitterOffsetY = inputs.jitterY;
    eval.InRenderSubrectDimensions = {inputs.width, inputs.height};
    eval.InReset = inputs.reset ? 1 : 0;
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSSD_EXT(list, m_feature, m_parameters, &eval);
    if (NVSDK_NGX_FAILED(result)) {
        log::Info("DLSS: Ray Reconstruction evaluation failed ({:#010x}); falling back to TAA", Hex(result));
        m_failed = true;
        return false;
    }
    return true;
}

}
