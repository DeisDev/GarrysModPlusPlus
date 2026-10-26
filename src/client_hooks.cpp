#include "client_hooks.h"

#include "effects.h"
#include "log.h"
#include "module_scan.h"

#include <MinHook.h>

#include <windows.h>

namespace gmodpp::client {

namespace {

// CRendering3dView::DrawWorld(float waterZAdjust) in client.dll, located through the "RenderWorld" VPROF
// counter only it references. It starts every view's opaque pass.
constexpr char kDrawWorldLabel[] = "RenderWorld";
// CRendering3dView::DrawTranslucentRenderables(bool bInSkybox, bool bShadowDepth) in client.dll,
// located through the VPROF label it passes to CVProfile::EnterScope.
constexpr char kDrawTranslucentRenderablesLabel[] = "CViewRender::DrawTranslucentRenderables";

using DrawWorldFn = void(__fastcall*)(void* view, float waterZAdjust);
using DrawTranslucentRenderablesFn = void(__fastcall*)(void* view, bool inSkybox, bool shadowDepth);
DrawWorldFn g_drawWorld = nullptr;
DrawTranslucentRenderablesFn g_drawTranslucentRenderables = nullptr;
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
}

bool Install(HMODULE clientModule, const char* label, const char* name, void* hook, void** original)
{
    void* target = FindFunctionReferencingString(clientModule, label);
    if (target == nullptr) {
        log::Info("client.dll: {} not found; effects disabled", name);
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
    // Both hooks are needed: a capture started by DrawWorld must always be finished by the translucent pass.
    if (!Install(clientModule, kDrawTranslucentRenderablesLabel, "DrawTranslucentRenderables", reinterpret_cast<void*>(&HookDrawTranslucentRenderables),
            reinterpret_cast<void**>(&g_drawTranslucentRenderables)))
        return;
    Install(clientModule, kDrawWorldLabel, "DrawWorld", reinterpret_cast<void*>(&HookDrawWorld), reinterpret_cast<void**>(&g_drawWorld));
}

}
