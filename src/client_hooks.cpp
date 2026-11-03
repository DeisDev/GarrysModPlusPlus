#include "client_hooks.h"

#include "exposure.h"
#include "log.h"
#include "module_scan.h"
#include "settings.h"
#include "shaderapi.h"
#include "post_effects.h"
#include "view_events.h"
#include "world.h"

#include <MinHook.h>

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace gmodpp::client {

namespace {

// CRendering3dView::DrawWorld(float waterZAdjust) in client.dll, located through the "RenderWorld" VPROF
// counter only it references. It starts every view's opaque pass.
constexpr char kDrawWorldLabel[] = "RenderWorld";
// CRendering3dView::DrawTranslucentRenderables(bool bInSkybox, bool bShadowDepth) in client.dll,
// located through the VPROF label it passes to CVProfile::EnterScope.
constexpr char kDrawTranslucentRenderablesLabel[] = "CViewRender::DrawTranslucentRenderables";
// CRendering3dView::EnableWorldFog (client.dll+0x30E4C0, REA 2026-11-03) sets the map's fog in the open air through the
// render context (FogMode, FogColor3fv, FogStart, FogEnd, FogMaxDensity), unless GM:SetupWorldFog (Lua hook 0x98)
// returns true, having set its own. CAboveWaterView, its reflection view, CUnderWaterView's refraction view and
// CSimpleWorldView (when the eye is in no fog volume) call it; water's fog is set elsewhere. It loads no string: this is
// its call of the Lua hook.
constexpr char kEnableWorldFogPattern[] = "BA 98 00 00 00 E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B 0D ?? ?? ?? ?? 45 33 C0 33 D2 E8";
// CSkyboxView::Enable3dSkyboxFog (client.dll+0x30E040, REA 2026-11-03) sets the 3D skybox's fog, its distances times
// 1 / the skybox's scale, unless GM:SetupSkyboxFog (0x97) returns true; CSkyboxView::DrawInternal calls it before
// DrawWorld. This is its call of the Lua hook.
constexpr char kEnable3dSkyboxFogPattern[] = "BA 97 00 00 00 E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B 0D ?? ?? ?? ?? 0F 5A CF 48 8B 01 FF 90 F0 00 00 00";
// DoEnginePostProcessing(int x, int y, int w, int h, bool bFlashlightIsOn, bool bPostVGui) (client.dll+0x300540, REA
// 2026-11-03), the only function that loads "EnginePost", the name of its render target dump. CViewRender::RenderView
// calls it for views with bloom and tone mapping after their viewmodel and fade, before the screen-space effects and
// the HUD; the camera effects and colour grading follow it.
constexpr char kDoEnginePostProcessingLabel[] = "EnginePost";

using DrawWorldFn = void(__fastcall*)(void* view, float waterZAdjust);
using DrawTranslucentRenderablesFn = void(__fastcall*)(void* view, bool inSkybox, bool shadowDepth);
using EnableFogFn = void(__fastcall*)(void* view);
using DoEnginePostProcessingFn = void(__fastcall*)(int x, int y, int w, int h, bool flashlightOn, bool postVGui);
DrawWorldFn g_drawWorld = nullptr;
DrawTranslucentRenderablesFn g_drawTranslucentRenderables = nullptr;
EnableFogFn g_enableWorldFog = nullptr;
EnableFogFn g_enable3dSkyboxFog = nullptr;
DoEnginePostProcessingFn g_doEnginePostProcessing = nullptr;
bool g_attempted = false;

void __fastcall HookDrawWorld(void* view, float waterZAdjust)
{
    effects::OnViewOpaqueBegin();
    g_drawWorld(view, waterZAdjust);
}

void __fastcall HookDrawTranslucentRenderables(void* view, bool inSkybox, bool shadowDepth)
{
    if (!shadowDepth)
        effects::OnViewOpaqueEnd(inSkybox);
    g_drawTranslucentRenderables(view, inSkybox, shadowDepth);
    if (!shadowDepth)
        effects::OnViewTranslucentEnd();
}

// Replaces the fog the game just set with the menu's fog override. The 3D skybox's distances are in its own units,
// as the game converts them.
void OverrideFog(bool skybox)
{
    native::SceneFog fog = native::CurrentFog();
    if (!skybox) {
        Statistics& statistics = GetStatistics();
        statistics.gameFogKnown = fog.mode != native::kFogNone;
        if (statistics.gameFogKnown) {
            for (size_t i = 0; i < 3; ++i)
                statistics.gameFogColor[i] = fog.color[i] / 255.0f;
            statistics.gameFogStart = fog.start;
            statistics.gameFogEnd = fog.end;
            statistics.gameFogMaxDensity = fog.maxDensity;
        }
    }
    const Settings& settings = GetSettings();
    if (!settings.fogOverride)
        return;
    float scale = 1.0f;
    if (skybox) {
        const float skyboxScale = world::SkyboxScale();
        if (skyboxScale <= 0.0f)
            return;
        scale = 1.0f / skyboxScale;
    }
    fog.mode = native::kFogLinear;
    for (size_t i = 0; i < 3; ++i)
        fog.color[i] = static_cast<unsigned char>(std::lround(std::clamp(settings.fogColor[i], 0.0f, 1.0f) * 255.0f));
    fog.start = settings.fogStart * scale;
    fog.end = std::max(settings.fogEnd, settings.fogStart + 1.0f) * scale;
    fog.maxDensity = settings.fogMaxDensity;
    native::SetFog(fog);
}

void __fastcall HookEnableWorldFog(void* view)
{
    g_enableWorldFog(view);
    OverrideFog(false);
}

void __fastcall HookEnable3dSkyboxFog(void* view)
{
    g_enable3dSkyboxFog(view);
    OverrideFog(true);
}

void __fastcall HookDoEnginePostProcessing(int x, int y, int w, int h, bool flashlightOn, bool postVGui)
{
    g_doEnginePostProcessing(x, y, w, h, flashlightOn, postVGui);
    native::PostProcessView({x, y, x + w, y + h});
}

bool Hook(HMODULE clientModule, void* target, const char* name, void* hook, void** original)
{
    if (target == nullptr) {
        log::Info("client.dll: {} not found", name);
        return false;
    }
    log::Info("client.dll: {} at client.dll+{:#x}", name, reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(clientModule));
    MH_STATUS status = MH_CreateHook(target, hook, original);
    if (status == MH_OK)
        status = MH_EnableHook(target);
    if (status != MH_OK) {
        log::Info("Hooking {} failed: {}", name, MH_StatusToString(status));
        return false;
    }
    return true;
}

bool Install(HMODULE clientModule, const char* label, const char* name, void* hook, void** original)
{
    return Hook(clientModule, FindFunctionReferencingString(clientModule, label), name, hook, original);
}

}

void TryInstall()
{
    if (g_attempted)
        return;
    HMODULE clientModule = GetModuleHandleW(L"client.dll");
    if (clientModule == nullptr)
        return;
    g_attempted = true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log::Info("MinHook initialization failed: {}", MH_StatusToString(status));
        return;
    }
    exposure::TryInstall();
    // The fog override and the camera effects each need only their own hook.
    if (!Hook(clientModule, FindFunctionWithPattern(clientModule, kEnableWorldFogPattern), "EnableWorldFog", reinterpret_cast<void*>(&HookEnableWorldFog),
            reinterpret_cast<void**>(&g_enableWorldFog)))
        log::Info("client.dll: the fog override is unavailable");
    if (!Hook(clientModule, FindFunctionWithPattern(clientModule, kEnable3dSkyboxFogPattern), "Enable3dSkyboxFog",
            reinterpret_cast<void*>(&HookEnable3dSkyboxFog), reinterpret_cast<void**>(&g_enable3dSkyboxFog)))
        log::Info("client.dll: the fog override leaves the 3D skybox's fog");
    if (!Install(clientModule, kDoEnginePostProcessingLabel, "DoEnginePostProcessing", reinterpret_cast<void*>(&HookDoEnginePostProcessing),
            reinterpret_cast<void**>(&g_doEnginePostProcessing)))
        log::Info("client.dll: the camera effects and colour grading are unavailable");
    // Both hooks are needed: a capture started by DrawWorld must always be finished by the translucent pass.
    if (!Install(clientModule, kDrawTranslucentRenderablesLabel, "DrawTranslucentRenderables", reinterpret_cast<void*>(&HookDrawTranslucentRenderables),
            reinterpret_cast<void**>(&g_drawTranslucentRenderables))) {
        log::Info("client.dll: effects disabled");
        return;
    }
    if (!Install(clientModule, kDrawWorldLabel, "DrawWorld", reinterpret_cast<void*>(&HookDrawWorld), reinterpret_cast<void**>(&g_drawWorld)))
        log::Info("client.dll: effects disabled");
}

}
