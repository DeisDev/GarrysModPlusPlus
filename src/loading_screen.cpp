#include "loading_screen.h"

#include "log.h"
#include "module_scan.h"
#include "ui_text.h"
#include "world.h"

#include <MinHook.h>

#include <windows.h>

#include <string>
#include <string_view>

namespace gmodpp::loading {

namespace {

// CEngineVGui in engine.dll, found by its RTTI; EngineVGui() returns its only instance. Slot 26 is
// UpdateProgressBar(LevelLoadingProgress_e) (engine.dll+0x249650) and slot 27 UpdateCustomProgressBar(float,
// const wchar_t*) (+0x2495f0), at vtable +0xd0 and +0xd8 as CL_FullyConnected (+0x9b2f0) and
// FinishSignonState_New (+0xb0860) call them. CL_FullyConnected reports PROGRESS_READYTOPLAY (0x17) right before
// SCR_EndLoadingPlaque (+0x100f50) closes the loading screen. UpdateCustomProgressBar passes the text to
// CGameUI::UpdateProgressBar (GameUI.dll+0x244a0), which repaints the screen when the bar's segment count or the
// status text changes. Confirmed with Ghidra (REA).
constexpr char kEngineVGuiClass[] = ".?AVCEngineVGui@@";
constexpr size_t kUpdateProgressBarSlot = 26;
constexpr size_t kUpdateCustomProgressBarSlot = 27;
constexpr int kProgressReadyToPlay = 0x17;

using UpdateProgressBarFn = void(__fastcall*)(void* self, int progress);
using UpdateCustomProgressBarFn = void(__fastcall*)(void* self, float progress, const wchar_t* text);
UpdateProgressBarFn g_updateProgressBar = nullptr;
UpdateCustomProgressBarFn g_updateCustomProgressBar = nullptr;
int g_depth = 0;
bool g_attempted = false;

std::wstring Wide(std::string_view text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

void ShowProgress(void* self, float done, world::LoadStage stage)
{
    const std::wstring text = Wide(ui::Text(stage == world::LoadStage::Bake ? ui::TextId::LoadingBake : ui::TextId::LoadingScene));
    g_updateCustomProgressBar(self, done, text.c_str());
}

void __fastcall HookUpdateProgressBar(void* self, int progress)
{
    ++g_depth;
    g_updateProgressBar(self, progress);
    if (progress == kProgressReadyToPlay)
        world::PrepareLevel([self](float done, world::LoadStage stage) { ShowProgress(self, done, stage); });
    else
        world::BeginLevelLoad();
    --g_depth;
}

}

void TryInstall()
{
    if (g_attempted)
        return;
    HMODULE engine = GetModuleHandleW(L"engine.dll");
    if (engine == nullptr)
        return;
    g_attempted = true;
    const void* const* vtable = FindVtable(engine, kEngineVGuiClass, 0);
    if (vtable == nullptr) {
        log::Info("Loading screen: CEngineVGui not found; lighting loads after the map");
        return;
    }
    void* target = const_cast<void*>(vtable[kUpdateProgressBarSlot]);
    g_updateCustomProgressBar = reinterpret_cast<UpdateCustomProgressBarFn>(const_cast<void*>(vtable[kUpdateCustomProgressBarSlot]));
    const auto base = reinterpret_cast<uintptr_t>(engine);
    log::Info("Loading screen: UpdateProgressBar at engine.dll+{:#x}, UpdateCustomProgressBar at engine.dll+{:#x}", reinterpret_cast<uintptr_t>(target) - base,
        reinterpret_cast<uintptr_t>(g_updateCustomProgressBar) - base);
    MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)
        status = MH_CreateHook(target, reinterpret_cast<void*>(&HookUpdateProgressBar), reinterpret_cast<void**>(&g_updateProgressBar));
    if (status == MH_OK)
        status = MH_EnableHook(target);
    if (status != MH_OK)
        log::Info("Loading screen: hooking UpdateProgressBar failed: {}; lighting loads after the map", MH_StatusToString(status));
}

bool Active() { return g_depth > 0; }

}
