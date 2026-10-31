#include "native_lighting.h"

#include "command_line.h"
#include "d3d12_device.h"
#include "engine_interfaces.h"
#include "log.h"
#include "options.h"
#include "overlay.h"
#include "settings.h"
#include "shaderapi.h"
#include "shaderapi_capture.h"
#include "shaderapi_device.h"
#include "shaderapi_textures.h"
#include "video_menu.h"
#include "view_events.h"
#include "world.h"

#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace gmodpp::native::realtime {

namespace {

struct State {
    bool initialized = false;
    bool available = false;
    bool loggedThread = false;
    bool viewmodelCapture = false;
    float viewmodelDepth[2] = {};
    // Why the last main view was not captured, for the menu.
    RealtimeBlock block = RealtimeBlock::None;
    // The last main view's forward lighting, which views drawn into textures reuse, and its map: the GPU addresses in
    // it do not outlive the map's scene.
    std::optional<gbuffer::ForwardConstants> forward;
    std::string forwardLevel;
    std::set<std::string> loggedTargets;
};

State g_state;

// A capture can start when the game draws into the back buffer with its depth buffer on the render thread.
bool CanCapture()
{
    Backend& backend = GetBackend();
    if (!g_state.available || !world::Ready() || capture::Active() || !GetSettings().realtimeLighting)
        return false;
    if (!backend.OnRenderThread()) {
        g_state.block = RealtimeBlock::OtherThread;
        if (!std::exchange(g_state.loggedThread, true))
            log::Info("Real-time lighting: views are drawn on another thread than the device's (mat_queue_mode != 0); views are left unlit");
        return false;
    }
    const DynamicState& dynamic = Dynamic();
    const bool mainTarget = dynamic.colorTargets[0] == kDefaultTarget && dynamic.depthTarget == kDefaultTarget;
    if (mainTarget)
        g_state.block = RealtimeBlock::None;
    return mainTarget;
}

// Views drawn into a texture (monitors, mirrors, scopes) are not captured; their surfaces are lit while drawn, like
// translucent ones, with the last main view's light. Shadow depth views draw no colour.
bool CanLightRenderTargetView()
{
    if (!g_state.forward || engine::LevelName() != g_state.forwardLevel || !GetSettings().renderTargetLighting || !GetSettings().realtimeLighting || !world::Ready() || capture::Active() ||
        capture::ForwardActive() || !GetBackend().OnRenderThread())
        return false;
    const DynamicState& dynamic = Dynamic();
    if (dynamic.colorTargets[0] == kDefaultTarget || dynamic.colorTargets[0] == 0)
        return false;
    // The water's own views keep the game's light: its reflections are traced (shaders/water.hlsli), so the game's
    // reflection is not shown, and lighting the refraction view as it is drawn costs about 3 ms at 2560x1440.
    const Texture* target = TextureFromHandle(dynamic.colorTargets[0]);
    std::string name = target != nullptr ? target->name : std::string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (GetOptions().devMode && g_state.loggedTargets.insert(name).second && g_state.loggedTargets.size() <= 16)
        log::Info("Real-time lighting: a view is drawn into '{}'", name);
    return !name.starts_with("_rt_water");
}

void Finish(gbuffer::ViewKind kind)
{
    ViewInputs inputs = {};
    if (!capture::End(kind, inputs))
        return;
    inputs.depthMin = kind == gbuffer::ViewKind::Viewmodel ? g_state.viewmodelDepth[0] : 0.0f;
    inputs.depthMax = kind == gbuffer::ViewKind::Viewmodel ? g_state.viewmodelDepth[1] : 1.0f;
    // Lighting submits its own commands, which must follow the game's that drew the view.
    GetBackend().Submit();
    world::LightView(inputs);
    if (const std::optional<gbuffer::ForwardConstants> forward = world::ForwardLighting(inputs)) {
        g_state.forward = forward;
        g_state.forwardLevel = engine::LevelName();
        capture::BeginForward(*forward);
    }
}

// Source draws the viewmodel with DepthRange(0, 0.1); a few sprites use 0..0.01, which is not the viewmodel.
bool IsViewmodelRange(float minDepth, float maxDepth) { return minDepth == 0.0f && std::fabs(maxDepth - 0.1f) < 1e-4f; }

}

void OnDeviceReady()
{
    if (std::exchange(g_state.initialized, true))
        return;
    Backend& backend = GetBackend();
    ComPtr<ID3D12Device5> device;
    if (FAILED(backend.Device()->QueryInterface(IID_PPV_ARGS(&device)))) {
        log::Info("Real-time lighting unavailable: the device has no ID3D12Device5");
        return;
    }
    const RaytracingSupport support = QueryRaytracing(device.Get());
    log::Info("Real-time lighting: raytracing tier {:#x}, shader model {:#x}, inline raytracing {}, opacity micromaps {}", static_cast<int>(support.tier),
        static_cast<int>(support.shaderModel), support.inlineRaytracing ? "available" : "unavailable", support.opacityMicromaps ? "available" : "unavailable");
    if (!support.inlineRaytracing) {
        g_state.block = RealtimeBlock::Unsupported;
        return;
    }
    // nvngx_dlssd.dll is deployed next to the game's DLLs.
    const std::filesystem::path dlssDirectory = ExecutablePath().parent_path() / L"bin" / L"win64";
    g_state.available = world::Initialize(device.Get(), backend.Queue(), backend.Descriptors(), support.opacityMicromaps, dlssDirectory);
    overlay::Initialize(backend.Window(), backend.Device(), backend.Queue(), backend.Descriptors(), Backend::kBackBufferFormat, Backend::kFrames);
}

void OnViewport(float minDepth, float maxDepth)
{
    const bool viewmodel = IsViewmodelRange(minDepth, maxDepth);
    if (g_state.viewmodelCapture) {
        if (viewmodel)
            return;
        g_state.viewmodelCapture = false;
        Finish(gbuffer::ViewKind::Viewmodel);
        return;
    }
    if (!viewmodel || !CanCapture())
        return;
    // The viewmodel is drawn after the anti-aliasing resolve, so it is not jittered.
    g_state.viewmodelDepth[0] = minDepth;
    g_state.viewmodelDepth[1] = maxDepth;
    g_state.viewmodelCapture = capture::Begin({0.0f, 0.0f});
}

void OnBackBufferRead()
{
    // Bloom copies the back buffer before the next viewport change; without the lit viewmodel in it, the glow of what
    // lies behind the viewmodel is added over it.
    if (!g_state.viewmodelCapture)
        return;
    g_state.viewmodelCapture = false;
    Finish(gbuffer::ViewKind::Viewmodel);
}

void OnPresent()
{
    if (g_state.viewmodelCapture) {
        g_state.viewmodelCapture = false;
        Finish(gbuffer::ViewKind::Viewmodel);
    }
    if (capture::Active()) {
        log::Info("Real-time lighting: a G-buffer capture was still open at present; closing it");
        ViewInputs ignored = {};
        capture::End(gbuffer::ViewKind::Main, ignored);
    }
    capture::EndForward();
    RecordFrameInterval();
    GetStatistics().realtimeBlock = g_state.block;
    overlay::EnsureWindowHook();
    video_menu::TryInstall();
    Backend& backend = GetBackend();
    // gmodpp_console.txt is read by the shim (shaderapi_shim.cpp).
    world::OnPresent(backend.Width(), backend.Height(), false);
}

}

namespace gmodpp::effects {

void OnViewOpaqueBegin()
{
    using namespace native;
    if (realtime::CanLightRenderTargetView()) {
        capture::BeginForward(*realtime::g_state.forward);
        return;
    }
    if (!realtime::CanCapture())
        return;
    Backend& backend = GetBackend();
    capture::Begin(world::ViewJitter(backend.Width(), backend.Height()));
}

void OnViewOpaqueEnd(bool inSkybox)
{
    if (!native::realtime::g_state.viewmodelCapture)
        native::realtime::Finish(inSkybox ? gbuffer::ViewKind::Skybox : gbuffer::ViewKind::Main);
}

void OnViewTranslucentEnd() { native::capture::EndForward(); }

}
