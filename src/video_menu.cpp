#include "video_menu.h"

#include "log.h"
#include "module_scan.h"
#include "settings.h"

#include <MinHook.h>

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

// GameUI.dll (GMod x64, confirmed 2026-10-30 with Ghidra on the build with SHA-256 1f633e8d…ca547305, against
// GMODResourced's src/GameUI/OptionsSubVideo.cpp and resource/OptionsSubVideoAdvancedDlg.res):
// - COptionsSubVideoAdvancedDlg::COptionsSubVideoAdvancedDlg(Panel* parent) (0x1800191b0, `this, parent, 1`): the
//   only code that references "resource/OptionsSubVideoAdvancedDlg.res"; it builds the combo boxes and loads the .res,
//   whose positions are applied there and never reloaded (BuildGroup::ApplySchemeSettings only draws rulers).
// - ApplyChanges() (0x18001a3c0): the only reference to "FOVSlider".
// - The MSAA combo box is the member ApplyChanges reads (`mov rcx, [rdi+310h]; call GetActiveItem`) right before it
//   loads "mat_antialias".
// - OnResetData() (0x18001b9e0): the only reference to "dxinstalledlabel"; it sets every combo box from the convars.
// - ComboBox virtual ActivateItemByRow(int) at +0x950; Panel::GetVPanel() is slot 0 (IClientPanel's first method).
namespace gmodpp::video_menu {

namespace {

constexpr char kDialogResource[] = "resource/OptionsSubVideoAdvancedDlg.res";
constexpr char kDialogName[] = "OptionsSubVideoAdvancedDlg";
constexpr char kApplyChangesString[] = "FOVSlider";
constexpr char kResetDataString[] = "dxinstalledlabel";
constexpr char kMsaaConvar[] = "mat_antialias";

constexpr size_t kActivateItemByRow = 0x950;

// vgui2.dll's IPanel (public/vgui/IPanel.h, "VGUI_Panel009"): slot 0 is IBaseInterface's destructor.
constexpr char kPanelInterface[] = "VGUI_Panel009";
constexpr size_t kSetPos = 2;
constexpr size_t kGetPos = 3;
constexpr size_t kSetSize = 4;
constexpr size_t kGetSize = 5;
constexpr size_t kSetVisible = 14;
constexpr size_t kGetChildCount = 17;
constexpr size_t kGetChild = 18;
constexpr size_t kGetName = 36;

using VPanel = std::uintptr_t;
using CreateInterfaceFn = void*(__cdecl*)(const char* name, int* returnCode);
using DialogFn = void*(__fastcall*)(void* dialog, void* parent, int unknown);
using MethodFn = void(__fastcall*)(void* dialog);

void* g_panels = nullptr;
// Offset of COptionsSubVideoAdvancedDlg's MSAA combo box.
size_t g_msaaMember = 0;
DialogFn g_dialog = nullptr;
MethodFn g_resetData = nullptr;
bool g_attempted = false;
std::unordered_set<void*> g_dialogs;

template <typename R, typename... Args>
R CallVirtual(void* object, size_t offset, Args... args)
{
    auto* table = *reinterpret_cast<std::byte**>(object);
    return (*reinterpret_cast<R(__fastcall**)(void*, Args...)>(table + offset))(object, args...);
}

template <typename R, typename... Args>
R CallPanels(size_t slot, Args... args)
{
    return CallVirtual<R>(g_panels, slot * sizeof(void*), args...);
}

VPanel VPanelOf(void* panel) { return CallVirtual<VPanel>(panel, 0); }

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

Rect RectOf(VPanel panel)
{
    Rect rect;
    CallPanels<void>(kGetPos, panel, &rect.x, &rect.y);
    CallPanels<void>(kGetSize, panel, &rect.w, &rect.h);
    return rect;
}

std::string_view NameOf(VPanel panel)
{
    const char* name = CallPanels<const char*>(kGetName, panel);
    return name != nullptr ? name : "";
}

std::vector<VPanel> ChildrenOf(VPanel parent)
{
    std::vector<VPanel> children;
    const int count = CallPanels<int>(kGetChildCount, parent);
    for (int i = 0; i < count; ++i)
        children.push_back(CallPanels<VPanel>(kGetChild, parent, i));
    return children;
}

std::optional<VPanel> ChildNamed(const std::vector<VPanel>& children, std::string_view name)
{
    for (VPanel child : children) {
        if (NameOf(child) == name)
            return child;
    }
    return std::nullopt;
}

// The dialog member ApplyChanges reads just before it loads "mat_antialias": `mov rcx, [rdi+disp32]`.
size_t FindMsaaMember(HMODULE module, const void* applyChanges)
{
    for (const std::byte* reference : FindStringReferences(module, kMsaaConvar)) {
        if (reference < applyChanges || reference > static_cast<const std::byte*>(applyChanges) + 0x1000)
            continue;
        for (size_t back = 7; back < 32; ++back) {
            const auto* code = reinterpret_cast<const uint8_t*>(reference - back);
            if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x8F) {
                uint32_t displacement;
                std::memcpy(&displacement, code + 3, sizeof(displacement));
                return displacement < 0x1000 ? displacement : 0;
            }
        }
    }
    return 0;
}

// MSAA is hidden and off: the native renderer is single-sampled, and anti-aliasing comes from TAA or DLSS.
void TurnOffMsaa(void* dialog)
{
    void* combo = *reinterpret_cast<void**>(static_cast<std::byte*>(dialog) + g_msaaMember);
    if (combo != nullptr)
        CallVirtual<void>(combo, kActivateItemByRow, 0);
}

// The label of a row's control: the dialog names two labels "Label8", so it is the one in the control's column.
std::optional<VPanel> LabelOf(const std::vector<VPanel>& children, std::string_view name, VPanel control)
{
    const int column = RectOf(control).x;
    std::optional<VPanel> found;
    for (VPanel child : children) {
        if (NameOf(child) == name && (!found || std::abs(RectOf(child).x - column) < std::abs(RectOf(*found).x - column)))
            found = child;
    }
    return found;
}

// Hides MSAA and the Direct3D 9Ex row, and moves the rows below Direct3D 9Ex up into its place. With real-time
// lighting on, Shadow detail is hidden too: the flashlight's shadows and the projected prop shadows it sets are
// skipped and ray traced instead (shaderapi_capture.cpp).
bool Trim(void* dialog)
{
    const VPanel root = VPanelOf(dialog);
    if (NameOf(root) != kDialogName) {
        log::Info("Video menu: the Advanced video dialog's panel is named '{}', not {}; left unchanged", NameOf(root), kDialogName);
        return false;
    }
    const std::vector<VPanel> children = ChildrenOf(root);
    auto find = [&](std::string_view name) { return ChildNamed(children, name); };
    const auto msaa = find("AntialiasingMode"), msaaLabel = find("Label5"), d3d9ex = find("D3D9Ex"), d3d9exLabel = find("D3D9ExLabel"),
               dxLevel = find("dxlabel");
    if (!msaa || !msaaLabel || !d3d9ex || !d3d9exLabel || !dxLevel) {
        log::Info("Video menu: the Advanced video dialog's layout is not the expected one; left unchanged");
        return false;
    }
    // Resizing the dialog puts its controls back where the .res file has them, so it comes first.
    const Rect dialogRect = RectOf(root);
    const int d3d9exY = RectOf(*d3d9ex).y;
    const int step = d3d9exY - RectOf(*dxLevel).y;
    CallPanels<void>(kSetSize, root, dialogRect.w, dialogRect.h - step);
    for (VPanel panel : {*msaa, *msaaLabel, *d3d9ex, *d3d9exLabel})
        CallPanels<void>(kSetVisible, panel, false);
    if (GetSettings().realtimeLighting) {
        if (const auto shadows = find("ShadowDetail")) {
            CallPanels<void>(kSetVisible, *shadows, false);
            if (const auto label = LabelOf(children, "Label8", *shadows))
                CallPanels<void>(kSetVisible, *label, false);
        }
    }
    for (VPanel child : children) {
        if (NameOf(child).starts_with("frame_"))
            continue;
        const Rect rect = RectOf(child);
        if (rect.y > d3d9exY)
            CallPanels<void>(kSetPos, child, rect.x, rect.y - step);
    }
    TurnOffMsaa(dialog);
    return true;
}

void* __fastcall HookDialog(void* dialog, void* parent, int unknown)
{
    void* result = g_dialog(dialog, parent, unknown);
    if (Trim(dialog))
        g_dialogs.insert(dialog);
    else
        g_dialogs.erase(dialog);
    return result;
}

void __fastcall HookResetData(void* dialog)
{
    g_resetData(dialog);
    if (g_dialogs.contains(dialog))
        TurnOffMsaa(dialog);
}

bool Hook(void* target, void* hook, void** original, const char* name)
{
    MH_STATUS status = MH_CreateHook(target, hook, original);
    if (status == MH_OK)
        status = MH_EnableHook(target);
    if (status != MH_OK)
        log::Info("Video menu: hooking {} failed: {}", name, MH_StatusToString(status));
    return status == MH_OK;
}

}

void TryInstall()
{
    if (g_attempted)
        return;
    HMODULE gameUi = GetModuleHandleW(L"GameUI.dll");
    HMODULE vgui = GetModuleHandleW(L"vgui2.dll");
    if (gameUi == nullptr || vgui == nullptr)
        return;
    g_attempted = true;

    auto createInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(vgui, "CreateInterface"));
    g_panels = createInterface != nullptr ? createInterface(kPanelInterface, nullptr) : nullptr;
    void* dialog = FindFunctionReferencingString(gameUi, kDialogResource);
    void* applyChanges = FindFunctionReferencingString(gameUi, kApplyChangesString);
    void* resetData = FindFunctionReferencingString(gameUi, kResetDataString);
    g_msaaMember = applyChanges != nullptr ? FindMsaaMember(gameUi, applyChanges) : 0;
    if (g_panels == nullptr || dialog == nullptr || resetData == nullptr || g_msaaMember == 0) {
        log::Info("Video menu: GameUI.dll is not the expected build; the video options are left unchanged");
        return;
    }

    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log::Info("MinHook initialization failed: {}", MH_StatusToString(status));
        return;
    }
    if (!Hook(resetData, reinterpret_cast<void*>(&HookResetData), reinterpret_cast<void**>(&g_resetData), "OnResetData") ||
        !Hook(dialog, reinterpret_cast<void*>(&HookDialog), reinterpret_cast<void**>(&g_dialog), "COptionsSubVideoAdvancedDlg"))
        return;
    log::Info("Video menu: hooked the Advanced video dialog (GameUI.dll+{:#x})", reinterpret_cast<uintptr_t>(dialog) - reinterpret_cast<uintptr_t>(gameUi));
}

}
