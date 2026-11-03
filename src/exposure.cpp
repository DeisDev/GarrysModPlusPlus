#include "exposure.h"

#include "log.h"
#include "module_scan.h"
#include "settings.h"
#include "shaderapi.h"

#include <MinHook.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace gmodpp::exposure {

namespace {

// materialsystem.dll's CMatRenderContext (vtable 0x1800b4fa0, REA 2026-11-03). CMatQueuedRenderContext and a third
// context class share these three functions; only SetToneMappingScaleLinear (slot 151) differs.
//   148 ResetToneMappingScale(float): current = scale; SetToneMappingScaleLinear (IShaderAPI slot 56); last = current;
//       goal = 1.
//   149 SetGoalToneMappingScale(float): goal = scale when finite. The client's SetToneMapScale (client.dll+0x3047f0) sets
//       it each frame from the histogram's target, clamped to mat_autoexposure_min/max or the map's
//       env_tonemap_controller (GetExposureRange, client.dll+0x303790), then again to a weighted average of 10 frames.
//   150 TurnOnToneMapping(): once per CViewRender::RenderView, moves current toward the goal by at most 1/64 a frame
//       (mat_tonemap_algorithm 1), faster when darkening (mat_accelerate_adjust_exposure_down), and applies it.
// The client's next target is its histogram's ratio times the last scale applied (GetToneMappingScaleLinear, slot 152,
// which returns the context's own last value), so a goal biased or clamped here holds without fighting its loop.
constexpr char kRenderContextClass[] = ".?AVCMatRenderContext@@";
constexpr size_t kResetSlot = 148;
constexpr size_t kSetGoalSlot = 149;
constexpr size_t kTurnOnSlot = 150;
// Source darkens up to 3 times faster than it brightens (mat_accelerate_adjust_exposure_down's default).
constexpr float kDarkeningSpeedup = 3.0f;

using ScaleFn = void(__fastcall*)(void* context, float scale);
using TurnOnFn = void(__fastcall*)(void* context);
ScaleFn g_reset = nullptr;
ScaleFn g_setGoal = nullptr;
TurnOnFn g_turnOn = nullptr;
bool g_attempted = false;

// Per render context: the goal as passed on, the scale applied, and when it was last moved.
struct ToneState {
    float goal = 1.0f;
    float current = 0.0f;
    std::chrono::steady_clock::time_point moved;
};
std::unordered_map<void*, ToneState> g_states;

void __fastcall HookReset(void* context, float scale)
{
    ToneState& state = g_states[context];
    state.current = scale;
    state.goal = 1.0f;
    g_reset(context, scale);
}

void __fastcall HookSetGoal(void* context, float scale)
{
    const Settings& settings = GetSettings();
    if (settings.exposureAdaptation && std::isfinite(scale)) {
        scale *= std::exp2(settings.exposureBias);
        scale = std::clamp(scale, settings.exposureMinimum, std::max(settings.exposureMaximum, settings.exposureMinimum));
    }
    if (std::isfinite(scale))
        g_states[context].goal = scale;
    g_setGoal(context, scale);
}

// Source's smoothing, frame-rate bound by its 1/64 step, replaced by an exponential approach over exposureTime seconds.
// ResetToneMappingScale applies the scale as TurnOnToneMapping would; the goal it resets is set back at once.
void __fastcall HookTurnOn(void* context)
{
    const Settings& settings = GetSettings();
    ToneState& state = g_states[context];
    const auto now = std::chrono::steady_clock::now();
    if (!settings.exposureAdaptation || native::HdrType() == native::kHdrNone) {
        g_turnOn(context);
        // Where Source's own smoothing left it, for when the settings take over.
        state.current = native::ToneMappingScale();
        state.moved = now;
        return;
    }
    const float seconds = state.moved.time_since_epoch().count() != 0 ? std::chrono::duration<float>(now - state.moved).count() : 0.0f;
    state.moved = now;
    if (state.current <= 0.0f || !std::isfinite(state.current))
        state.current = state.goal;
    const float time = state.goal < state.current ? settings.exposureTime / kDarkeningSpeedup : settings.exposureTime;
    const float share = time > 0.0f ? 1.0f - std::exp(-std::clamp(seconds, 0.0f, 1.0f) / time) : 1.0f;
    state.current += (state.goal - state.current) * share;
    const float goal = state.goal;
    g_reset(context, state.current);
    g_setGoal(context, goal);
}

bool Hook(HMODULE module, const void* const* vtable, size_t slot, void* hook, void** original, const char* name)
{
    void* target = const_cast<void*>(vtable[slot]);
    log::Info("Auto exposure: {} at materialsystem.dll+{:#x}", name, reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(module));
    MH_STATUS status = MH_CreateHook(target, hook, original);
    if (status == MH_OK)
        status = MH_EnableHook(target);
    if (status != MH_OK)
        log::Info("Auto exposure: hooking {} failed: {}", name, MH_StatusToString(status));
    return status == MH_OK;
}

}

void TryInstall()
{
    if (g_attempted)
        return;
    HMODULE module = GetModuleHandleW(L"materialsystem.dll");
    if (module == nullptr)
        return;
    g_attempted = true;
    const void* const* vtable = FindVtable(module, kRenderContextClass, 0);
    if (vtable == nullptr) {
        log::Info("Auto exposure: CMatRenderContext not found; its settings do nothing");
        return;
    }
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log::Info("Auto exposure: MinHook initialization failed: {}", MH_StatusToString(status));
        return;
    }
    // TurnOnToneMapping's replacement calls the other two, so it is hooked only when both are.
    if (Hook(module, vtable, kResetSlot, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_reset), "ResetToneMappingScale") &&
        Hook(module, vtable, kSetGoalSlot, reinterpret_cast<void*>(&HookSetGoal), reinterpret_cast<void**>(&g_setGoal), "SetGoalToneMappingScale"))
        Hook(module, vtable, kTurnOnSlot, reinterpret_cast<void*>(&HookTurnOn), reinterpret_cast<void**>(&g_turnOn), "TurnOnToneMapping");
}

}
