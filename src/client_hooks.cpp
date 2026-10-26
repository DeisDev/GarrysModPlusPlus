#include "client_hooks.h"

#include "effects.h"
#include "log.h"
#include "module_scan.h"

#include <MinHook.h>

#include <windows.h>

namespace gmodpp::client {

namespace {

// CRendering3dView::DrawTranslucentRenderables(bool bInSkybox, bool bShadowDepth) in client.dll,
// located through the VPROF label it passes to CVProfile::EnterScope.
constexpr char kDrawTranslucentRenderablesLabel[] = "CViewRender::DrawTranslucentRenderables";

using DrawTranslucentRenderablesFn = void(__fastcall*)(void* view, bool inSkybox, bool shadowDepth);
DrawTranslucentRenderablesFn g_drawTranslucentRenderables = nullptr;
bool g_attempted = false;

void __fastcall HookDrawTranslucentRenderables(void* view, bool inSkybox, bool shadowDepth)
{
    if (!inSkybox && !shadowDepth)
        effects::OnOpaqueViewFinished();
    g_drawTranslucentRenderables(view, inSkybox, shadowDepth);
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

    void* target = FindFunctionReferencingString(clientModule, kDrawTranslucentRenderablesLabel);
    if (target == nullptr) {
        log::Info("client.dll: DrawTranslucentRenderables not found; effects disabled");
        return;
    }
    log::Info("client.dll: DrawTranslucentRenderables at client.dll+{:#x}",
        reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(clientModule));

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log::Info("MinHook initialization failed: {}", MH_StatusToString(status));
        return;
    }
    status = MH_CreateHook(target, reinterpret_cast<void*>(&HookDrawTranslucentRenderables),
        reinterpret_cast<void**>(&g_drawTranslucentRenderables));
    if (status == MH_OK)
        status = MH_EnableHook(target);
    if (status != MH_OK)
        log::Info("Hooking DrawTranslucentRenderables failed: {}", MH_StatusToString(status));
}

}
