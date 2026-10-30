#include "native_lighting.h"

#include "command_line.h"
#include "d3d12_device.h"
#include "log.h"
#include "overlay.h"
#include "settings.h"
#include "shaderapi.h"
#include "shaderapi_capture.h"
#include "shaderapi_device.h"
#include "view_events.h"
#include "world.h"

#include <wrl/client.h>

#include <cmath>
#include <filesystem>
#include <optional>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace gmodpp::native::realtime {

namespace {

struct State {
    bool initialized = false;
    bool available = false;
    bool loggedThread = false;
    bool loggedSamples = false;
    bool viewmodelCapture = false;
    float viewmodelDepth[2] = {};
};

State g_state;

// A capture can start when the game draws into the back buffer with its depth buffer on the render thread.
bool CanCapture()
{
    Backend& backend = GetBackend();
    if (!g_state.available || !world::Ready() || capture::Active() || !GetSettings().realtimeLighting)
        return false;
    if (!backend.OnRenderThread()) {
        if (!std::exchange(g_state.loggedThread, true))
            log::Info("Real-time lighting: views are drawn on another thread than the device's (mat_queue_mode != 0); views are left unlit");
        return false;
    }
    // Lighting reads single-sampled targets.
    if (backend.Samples() > 1) {
        if (!std::exchange(g_state.loggedSamples, true))
            log::Info("Real-time lighting is off while the game uses {}x MSAA (mat_antialias 0 turns it on)", backend.Samples());
        return false;
    }
    const DynamicState& dynamic = Dynamic();
    return dynamic.colorTargets[0] == kDefaultTarget && dynamic.depthTarget == kDefaultTarget;
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
    if (const std::optional<gbuffer::ForwardConstants> forward = world::ForwardLighting(inputs))
        capture::BeginForward(*forward);
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
    if (!support.inlineRaytracing)
        return;
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
    overlay::EnsureWindowHook();
    Backend& backend = GetBackend();
    // gmodpp_console.txt is read by the shim (shaderapi_shim.cpp).
    world::OnPresent(backend.Width(), backend.Height(), false);
}

}

namespace gmodpp::effects {

void OnViewOpaqueBegin()
{
    using namespace native;
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
