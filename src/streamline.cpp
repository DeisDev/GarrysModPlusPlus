#include "streamline.h"

#include "lighting.h"
#include "log.h"
#include "settings.h"

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>

namespace gmodpp::streamline {

namespace {

// DLSS's NGX project (src/dlss.cpp), which Frame Generation's NGX feature runs under too.
constexpr const char* kProjectId = "6b1f0c2e-9d4a-4f3e-8a57-2c1b9e7d4f10";
constexpr const char* kEngineVersion = "1.0";
// Frame Generation stays on for this many frames without the main view (menus, loading), then turns off as Streamline
// asks for screens that are not game frames (ProgrammingGuideDLSS_G.md, checklist).
constexpr uint32_t kGameFrameGrace = 2;

struct State {
    bool active = false;
    bool reflexLoaded = false;
    bool reflex = false;
    bool frameGeneration = false;
    uint32_t maxGenerated = 0;
    sl::FrameToken* frame = nullptr;
    uint32_t frameCount = 0;
    uint32_t lastTagged = 0;
    bool renderMarked = false;
    std::optional<sl::ReflexMode> appliedReflex;
    std::optional<std::pair<sl::DLSSGMode, uint32_t>> appliedGeneration;
    // Last frame's camera, for the matrices to it.
    bool havePrevious = false;
    sl::float4x4 previousViewToWorld = {};
    sl::float4x4 previousViewToClip = {};
};

State g_state;
const sl::ViewportHandle kViewport{0u};

void LogMessage(sl::LogType type, const char* message)
{
    if (type == sl::LogType::eInfo)
        return;
    std::string text = message != nullptr ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    log::Info("Streamline: {}", text);
}

sl::float4x4 Matrix(const float values[16])
{
    sl::float4x4 m = {};
    for (uint32_t row = 0; row < 4; ++row)
        m.setRow(row, sl::float4(values[row * 4], values[row * 4 + 1], values[row * 4 + 2], values[row * 4 + 3]));
    return m;
}

sl::float4x4 Identity()
{
    constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return Matrix(kIdentity);
}

void Mark(sl::PCLMarker marker)
{
    if (g_state.frame != nullptr)
        slPCLSetMarker(marker, *g_state.frame);
}

// The menu's Reflex mode (Frame Generation needs it on) and Frame Generation, applied when they change.
void ApplyOptions()
{
    const Settings& settings = GetSettings();
    const bool gameFrames = g_state.frameCount - g_state.lastTagged <= kGameFrameGrace;
    const bool generate = g_state.frameGeneration && settings.frameGeneration == FrameGeneration::Dlss && gameFrames;
    if (g_state.reflexLoaded) {
        sl::ReflexMode mode = settings.reflex == ReflexMode::Boost ? sl::ReflexMode::eLowLatencyWithBoost
            : settings.reflex == ReflexMode::On || generate               ? sl::ReflexMode::eLowLatency
                                                                          : sl::ReflexMode::eOff;
        if (!g_state.reflex)
            mode = sl::ReflexMode::eOff;
        if (g_state.appliedReflex != mode) {
            sl::ReflexOptions options = {};
            options.mode = mode;
            const sl::Result result = slReflexSetOptions(options);
            if (result != sl::Result::eOk)
                log::Info("Streamline: setting Reflex's mode failed ({})", static_cast<int>(result));
            g_state.appliedReflex = mode;
        }
    }
    if (g_state.frameGeneration) {
        const std::pair<sl::DLSSGMode, uint32_t> wanted = {generate ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff,
            std::clamp(static_cast<uint32_t>(settings.generatedFrames), 1u, std::max(g_state.maxGenerated, 1u))};
        if (g_state.appliedGeneration != wanted) {
            sl::DLSSGOptions options = {};
            options.mode = wanted.first;
            options.numFramesToGenerate = wanted.second;
            const sl::Result result = slDLSSGSetOptions(kViewport, options);
            if (result != sl::Result::eOk)
                log::Info("Streamline: setting Frame Generation's options failed ({})", static_cast<int>(result));
            else
                log::Info("Streamline: Frame Generation {}", wanted.first == sl::DLSSGMode::eOn ? "on" : "off");
            g_state.appliedGeneration = wanted;
        }
    }
}

}

bool Initialize(const std::filesystem::path& directory)
{
    const std::filesystem::path interposer = directory / L"sl.interposer.dll";
    std::error_code error;
    if (!std::filesystem::exists(interposer, error)) {
        log::Info("Streamline: {} is missing; Reflex and DLSS Frame Generation are unavailable", interposer.string());
        return false;
    }
    // NVIDIA's custom signature as well as Windows' (ProgrammingGuide.md, 2.1.1 Security).
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        log::Info("Streamline: {} is not signed by NVIDIA; not loaded", interposer.string());
        return false;
    }
    // Loaded by its full path, so the delay-loaded imports find it by name.
    if (LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr) {
        log::Info("Streamline: loading {} failed ({})", interposer.string(), GetLastError());
        return false;
    }
    const std::wstring plugins = directory.wstring();
    const wchar_t* pluginPaths[] = {plugins.c_str()};
    const std::wstring logs = (directory / L"logs").wstring();
    const sl::Feature features[] = {sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS_G};
    sl::Preferences preferences = {};
    preferences.pathsToPlugins = pluginPaths;
    preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = logs.c_str();
    preferences.logMessageCallback = &LogMessage;
    // Manual hooking, tags per frame (slSetTagForFrame), and no updates downloaded over the air.
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = kEngineVersion;
    preferences.projectId = kProjectId;
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    const sl::Result result = slInit(preferences, sl::kSDKVersion);
    if (result != sl::Result::eOk) {
        log::Info("Streamline: slInit failed ({}); Reflex and DLSS Frame Generation are unavailable", static_cast<int>(result));
        return false;
    }
    g_state.active = true;
    return true;
}

bool Active() { return g_state.active; }

void SetDevice(ID3D12Device* device, const LUID& adapter)
{
    if (!g_state.active)
        return;
    if (slSetD3DDevice(device) != sl::Result::eOk) {
        log::Info("Streamline: slSetD3DDevice failed; Reflex and DLSS Frame Generation are unavailable");
        g_state.active = false;
        return;
    }
    sl::AdapterInfo info = {};
    info.deviceLUID = reinterpret_cast<uint8_t*>(const_cast<LUID*>(&adapter));
    info.deviceLUIDSizeInBytes = sizeof(LUID);
    bool loaded = false;
    g_state.reflexLoaded = slIsFeatureLoaded(sl::kFeatureReflex, loaded) == sl::Result::eOk && loaded;
    g_state.reflex = g_state.reflexLoaded && slIsFeatureSupported(sl::kFeatureReflex, info) == sl::Result::eOk;
    const sl::Result generation = slIsFeatureSupported(sl::kFeatureDLSS_G, info);
    g_state.frameGeneration = generation == sl::Result::eOk;
    if (g_state.frameGeneration) {
        sl::DLSSGState state = {};
        if (slDLSSGGetState(kViewport, state, nullptr) == sl::Result::eOk)
            g_state.maxGenerated = std::max(state.numFramesToGenerateMax, 1u);
    }
    log::Info("Streamline: Reflex {}, DLSS Frame Generation {} (up to {} generated frames)", g_state.reflex ? "available" : "unavailable",
        g_state.frameGeneration ? "available" : "unavailable", g_state.maxGenerated);
    if (!g_state.frameGeneration)
        log::Info("Streamline: DLSS Frame Generation is not supported here ({}): it needs an RTX 40 or newer GPU and hardware-accelerated GPU scheduling",
            static_cast<int>(generation));
}

Microsoft::WRL::ComPtr<ID3D12Device> QueueDevice(ID3D12Device* device)
{
    Microsoft::WRL::ComPtr<ID3D12Device> result = device;
    if (!g_state.active)
        return result;
    // The proxy keeps the reference it is made from.
    device->AddRef();
    void* proxy = device;
    if (slUpgradeInterface(&proxy) != sl::Result::eOk) {
        device->Release();
        return result;
    }
    result.Attach(static_cast<ID3D12Device*>(proxy));
    return result;
}

Microsoft::WRL::ComPtr<IDXGIFactory2> SwapChainFactory(IDXGIFactory2* factory)
{
    Microsoft::WRL::ComPtr<IDXGIFactory2> result = factory;
    if (!g_state.active)
        return result;
    factory->AddRef();
    void* proxy = factory;
    if (slUpgradeInterface(&proxy) != sl::Result::eOk) {
        factory->Release();
        log::Info("Streamline: upgrading the DXGI factory failed; DLSS Frame Generation is unavailable");
        g_state.frameGeneration = false;
        return result;
    }
    result.Attach(static_cast<IDXGIFactory2*>(proxy));
    return result;
}

void BeginFrame()
{
    if (!g_state.active)
        return;
    if (slGetNewFrameToken(g_state.frame, nullptr) != sl::Result::eOk) {
        g_state.frame = nullptr;
        return;
    }
    ++g_state.frameCount;
    ApplyOptions();
    // Called whatever Reflex's mode, as its markers are (ProgrammingGuideReflex.md, 4.0).
    if (g_state.reflexLoaded)
        slReflexSleep(*g_state.frame);
    Mark(sl::PCLMarker::eSimulationStart);
    g_state.renderMarked = false;
}

void MarkRenderStart()
{
    if (!g_state.active || g_state.renderMarked)
        return;
    g_state.renderMarked = true;
    Mark(sl::PCLMarker::eSimulationEnd);
    Mark(sl::PCLMarker::eRenderSubmitStart);
}

void MarkPresent(bool end)
{
    if (!g_state.active)
        return;
    if (end) {
        Mark(sl::PCLMarker::ePresentEnd);
        return;
    }
    MarkRenderStart();
    Mark(sl::PCLMarker::eRenderSubmitEnd);
    Mark(sl::PCLMarker::ePresentStart);
}

// Called after each present, the count is per rendered frame (ProgrammingGuideDLSS_G.md, numFramesActuallyPresented);
// without options the query is cheapest.
uint32_t PresentedFrames()
{
    if (!g_state.active || !g_state.appliedGeneration || g_state.appliedGeneration->first != sl::DLSSGMode::eOn)
        return 1;
    sl::DLSSGState state = {};
    if (slDLSSGGetState(kViewport, state, nullptr) != sl::Result::eOk)
        return 1;
    return state.numFramesActuallyPresented;
}

bool FrameGenerationWanted() { return g_state.active && g_state.frameGeneration && GetSettings().frameGeneration == FrameGeneration::Dlss; }

void TagFrame(ID3D12GraphicsCommandList* list, const FrameInputs& inputs, const PostView& post)
{
    if (!FrameGenerationWanted() || g_state.frame == nullptr)
        return;
    const UINT width = static_cast<UINT>(inputs.view.right - inputs.view.left);
    const UINT height = static_cast<UINT>(inputs.view.bottom - inputs.view.top);
    const uint32_t state = static_cast<uint32_t>(inputs.state);
    sl::Resource depth(sl::ResourceType::eTex2d, inputs.depth, nullptr, nullptr, state);
    sl::Resource motion(sl::ResourceType::eTex2d, inputs.motion, nullptr, nullptr, state);
    sl::Resource hudless(sl::ResourceType::eTex2d, inputs.hudless, nullptr, nullptr, state);
    const sl::Extent view = {static_cast<uint32_t>(inputs.view.top), static_cast<uint32_t>(inputs.view.left), width, height};
    const sl::Extent inputExtent = {0, 0, width, height};
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent),
        sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &view),
    };
    if (slSetTagForFrame(*g_state.frame, kViewport, tags, static_cast<uint32_t>(std::size(tags)), list) != sl::Result::eOk)
        return;

    // The camera, row-major for row vectors as Streamline's matrices (sl_matrix_helpers.h) are.
    sl::Constants constants = {};
    constants.cameraViewToClip = Matrix(post.viewToClip);
    sl::matrixFullInvert(constants.clipToCameraView, constants.cameraViewToClip);
    constants.clipToLensClip = Identity();
    sl::float4x4 viewToWorld;
    sl::matrixOrthoNormalInvert(viewToWorld, Matrix(post.worldToView));
    const bool reset = !g_state.havePrevious;
    if (reset) {
        constants.clipToPrevClip = Identity();
        constants.prevClipToClip = Identity();
    } else {
        sl::float4x4 viewToPreviousView;
        sl::calcCameraToPrevCamera(viewToPreviousView, viewToWorld, g_state.previousViewToWorld);
        sl::float4x4 clipToPreviousView;
        sl::matrixMul(clipToPreviousView, constants.clipToCameraView, viewToPreviousView);
        sl::matrixMul(constants.clipToPrevClip, clipToPreviousView, g_state.previousViewToClip);
        sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);
    }
    g_state.previousViewToWorld = viewToWorld;
    g_state.previousViewToClip = constants.cameraViewToClip;
    g_state.havePrevious = true;
    // Depth and motion are not jittered: the lighting writes the edges' depth at each pixel's centre
    // (shaders/translucent_depth.hlsl), and motion leaves the jitter out.
    constants.jitterOffset = {0.0f, 0.0f};
    // Motion is in the view's pixels.
    constants.mvecScale = {1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)};
    constants.cameraPinholeOffset = {0.0f, 0.0f};
    constants.cameraPos = {post.eye[0], post.eye[1], post.eye[2]};
    constants.cameraRight = {viewToWorld[0].x, viewToWorld[0].y, viewToWorld[0].z};
    constants.cameraUp = {viewToWorld[1].x, viewToWorld[1].y, viewToWorld[1].z};
    constants.cameraFwd = {viewToWorld[2].x, viewToWorld[2].y, viewToWorld[2].z};
    // A D3D projection for row vectors: device depth d = m22 + m32 / z, 0 at the near plane and 1 at the far one
    // (reversed: 1 and 0).
    const sl::float4x4& p = constants.cameraViewToClip;
    const bool reversed = p[3].z > 0.0f;
    constants.cameraNear = std::fabs(p[3].z / ((reversed ? 1.0f : 0.0f) - p[2].z));
    constants.cameraFar = std::fabs(p[3].z / ((reversed ? 0.0f : 1.0f) - p[2].z));
    constants.cameraFOV = 2.0f * std::atan(1.0f / p[1].y);
    constants.cameraAspectRatio = static_cast<float>(width) / static_cast<float>(height);
    constants.depthInverted = reversed ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    if (slSetConstants(constants, *g_state.frame, kViewport) == sl::Result::eOk)
        g_state.lastTagged = g_state.frameCount;
}

bool ReflexAvailable() { return g_state.active && g_state.reflex; }

bool FrameGenerationAvailable() { return g_state.active && g_state.frameGeneration; }

uint32_t MaxGeneratedFrames() { return g_state.maxGenerated; }

}
