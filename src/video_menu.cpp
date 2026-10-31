#include "video_menu.h"

#include "log.h"
#include "module_scan.h"
#include "settings.h"
#include "shaderapi_device.h"
#include "ui_text.h"

#include <MinHook.h>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// GameUI.dll (GMod x64, confirmed 2026-10-30 with Ghidra on the build with SHA-256 1f633e8d…ca547305, against
// GMODResourced's src/GameUI/OptionsSubVideo.cpp and resource/OptionsSubVideoAdvancedDlg.res):
// - COptionsSubVideoAdvancedDlg::COptionsSubVideoAdvancedDlg(Panel* parent) (0x1800191b0, `this, parent, 1`): the
//   only code that references "resource/OptionsSubVideoAdvancedDlg.res"; it builds the combo boxes and loads the .res,
//   whose positions are applied there and never reloaded (BuildGroup::ApplySchemeSettings only draws rulers).
// - Its "ModelDetail" combo box: `mov ecx, 548h; call operator new (0x1800263b0); ...; lea r8, "ModelDetail"; ...;
//   mov rcx, rax; call ComboBox::ComboBox (0x1800892a0)` with (memory, parent, name, lines, allowEdit, 1).
// - CGammaDialog's "Gamma label": `mov ecx, 230h; ...; call operator new; ...; lea r9, text; ...; lea r8,
//   "Gamma label"; mov rdx, rdi; mov rcx, rax; call Label::Label` with (memory, parent, name, text, 1).
// - ApplyChanges() (0x18001a3c0): the only reference to "FOVSlider"; it returns at once unless a control changed,
//   and reads combo boxes with ComboBox::GetActiveItem (0x180089e50), the call after `mov rcx, [rdi+300h]`.
// - The MSAA combo box is the member ApplyChanges reads (`mov rcx, [rdi+310h]; call GetActiveItem`) right before it
//   loads "mat_antialias".
// - OnResetData() (0x18001b9e0): the only reference to "dxinstalledlabel"; it sets every combo box from the convars.
// - ComboBox virtuals: AddItem(const char*, const KeyValues*) at +0x900, ActivateItemByRow(int) at +0x950;
//   Panel::GetVPanel() is slot 0 (IClientPanel's first method).
namespace gmodpp::video_menu {

namespace {

constexpr char kDialogResource[] = "resource/OptionsSubVideoAdvancedDlg.res";
constexpr char kDialogName[] = "OptionsSubVideoAdvancedDlg";
constexpr char kApplyChangesString[] = "FOVSlider";
constexpr char kResetDataString[] = "dxinstalledlabel";
constexpr char kComboBoxName[] = "ModelDetail";
constexpr char kLabelName[] = "Gamma label";
constexpr char kMsaaConvar[] = "mat_antialias";

constexpr size_t kAddItem = 0x900;
constexpr size_t kActivateItemByRow = 0x950;

// vgui2.dll's IPanel (public/vgui/IPanel.h, "VGUI_Panel009"): slot 0 is IBaseInterface's destructor.
constexpr char kPanelInterface[] = "VGUI_Panel009";
constexpr size_t kSetPos = 2;
constexpr size_t kGetPos = 3;
constexpr size_t kSetSize = 4;
constexpr size_t kGetSize = 5;
constexpr size_t kSetVisible = 14;
constexpr size_t kIsVisible = 15;
constexpr size_t kGetChildCount = 17;
constexpr size_t kGetChild = 18;
constexpr size_t kGetName = 36;
constexpr size_t kSetEnabled = 57;

using VPanel = std::uintptr_t;
using CreateInterfaceFn = void*(__cdecl*)(const char* name, int* returnCode);
using AllocateFn = void*(__fastcall*)(size_t size);
using ComboBoxFn = void*(__fastcall*)(void* memory, void* parent, const char* name, int lines, bool allowEdit, int unknown);
using LabelFn = void*(__fastcall*)(void* memory, void* parent, const char* name, const char* text, int unknown);
using GetActiveItemFn = int(__fastcall*)(void* comboBox);
using DialogFn = void*(__fastcall*)(void* dialog, void* parent, int unknown);
using MethodFn = void(__fastcall*)(void* dialog);

struct GameUi {
    AllocateFn allocate = nullptr;
    ComboBoxFn comboBox = nullptr;
    size_t comboBoxSize = 0;
    LabelFn label = nullptr;
    size_t labelSize = 0;
    GetActiveItemFn getActiveItem = nullptr;
    // Offset of COptionsSubVideoAdvancedDlg's MSAA combo box.
    size_t msaaMember = 0;
    void* panels = nullptr;
};

GameUi g_gameUi;
DialogFn g_dialog = nullptr;
MethodFn g_applyChanges = nullptr;
MethodFn g_resetData = nullptr;
bool g_attempted = false;

// The settings the Advanced video dialog offers, each a combo box.
enum class Row {
    AntiAliasing,
    Upscaling,
    RealtimeLighting,
    Quality,
    IndirectLighting,
    FoliageShadows,
    Reflections,
    AmbientOcclusion,
    VolumetricLight,
    TranslucentLighting,
    RenderTargetLighting,
    Display,
    Count
};

// The controls added to one Advanced dialog.
struct Added {
    std::array<void*, static_cast<size_t>(Row::Count)> combos = {};
    VPanel shadowDetail = 0;
};
std::unordered_map<void*, Added> g_dialogs;

template <typename R, typename... Args>
R CallVirtual(void* object, size_t offset, Args... args)
{
    auto* table = *reinterpret_cast<std::byte**>(object);
    return (*reinterpret_cast<R(__fastcall**)(void*, Args...)>(table + offset))(object, args...);
}

template <typename R, typename... Args>
R CallPanels(size_t slot, Args... args)
{
    return CallVirtual<R>(g_gameUi.panels, slot * sizeof(void*), args...);
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

void Place(VPanel panel, const Rect& rect)
{
    CallPanels<void>(kSetPos, panel, rect.x, rect.y);
    CallPanels<void>(kSetSize, panel, rect.w, rect.h);
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

const std::byte* CallTarget(const std::byte* call)
{
    int32_t displacement;
    std::memcpy(&displacement, call + 1, sizeof(displacement));
    return call + 5 + displacement;
}

// The constructor a `new` expression calls after loading `reference`: the first `mov rcx, rax; call` within 48 bytes.
const std::byte* ConstructorAfter(const std::byte* reference)
{
    for (size_t i = 0; i < 48; ++i) {
        const auto* code = reinterpret_cast<const uint8_t*>(reference + i);
        if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0xC8 && code[3] == 0xE8)
            return CallTarget(reference + i + 3);
    }
    return nullptr;
}

// The allocation of the same `new` expression: the nearest `mov ecx, size` before `reference` and the call after it.
bool AllocationBefore(const std::byte* reference, AllocateFn& allocate, size_t& size)
{
    for (size_t back = 5; back < 64; ++back) {
        const auto* code = reinterpret_cast<const uint8_t*>(reference - back);
        if (code[0] != 0xB9)
            continue;
        uint32_t immediate;
        std::memcpy(&immediate, code + 1, sizeof(immediate));
        for (size_t i = 5; i < 20 && i + 5 <= back; ++i) {
            if (code[i] == 0xE8) {
                allocate = reinterpret_cast<AllocateFn>(const_cast<std::byte*>(CallTarget(reinterpret_cast<const std::byte*>(code + i))));
                size = immediate;
                return immediate >= 0x100 && immediate < 0x10000;
            }
        }
        return false;
    }
    return false;
}

// The `new` expression that names a control `name`, which must be loaded exactly once.
bool FindConstruction(HMODULE module, const char* name, AllocateFn& allocate, size_t& size, const std::byte*& constructor)
{
    const std::vector<const std::byte*> references = FindStringReferences(module, name);
    if (references.size() != 1)
        return false;
    constructor = ConstructorAfter(references[0]);
    return constructor != nullptr && AllocationBefore(references[0], allocate, size);
}

GetActiveItemFn FindGetActiveItem(const void* applyChanges)
{
    const auto* code = static_cast<const uint8_t*>(applyChanges);
    for (size_t i = 0; i < 0x200; ++i) {
        if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x8F && code[i + 5] == 0x00 && code[i + 6] == 0x00 && code[i + 7] == 0xE8)
            return reinterpret_cast<GetActiveItemFn>(const_cast<std::byte*>(CallTarget(reinterpret_cast<const std::byte*>(code + i + 7))));
    }
    return nullptr;
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

void* NewComboBox(void* dialog, const char* name, const std::vector<const char*>& items)
{
    void* memory = g_gameUi.allocate(g_gameUi.comboBoxSize);
    if (memory == nullptr)
        return nullptr;
    void* combo = g_gameUi.comboBox(memory, dialog, name, static_cast<int>(items.size()), false, 1);
    for (const char* item : items)
        CallVirtual<int>(combo, kAddItem, item, static_cast<const void*>(nullptr));
    return combo;
}

void* NewLabel(void* dialog, const char* name, const char* text)
{
    void* memory = g_gameUi.allocate(g_gameUi.labelSize);
    return memory != nullptr ? g_gameUi.label(memory, dialog, name, text, 1) : nullptr;
}

// Adds a combo box with its label; returns the combo box.
void* AddCombo(void* dialog, const char* name, ui::TextId label, const Rect& comboRect, const Rect& labelRect, const std::vector<const char*>& items)
{
    const std::string labelName = std::string(name) + "Label";
    void* combo = NewComboBox(dialog, name, items);
    void* text = NewLabel(dialog, labelName.c_str(), ui::Text(label));
    if (combo == nullptr || text == nullptr)
        return nullptr;
    Place(VPanelOf(combo), comboRect);
    Place(VPanelOf(text), labelRect);
    return combo;
}

const std::vector<const char*> kOnOff = {"#gameui_disabled", "#gameui_enabled"};

// The display row's texts, kept for the combo boxes' lifetime.
std::vector<std::string> g_displayItems;

std::vector<const char*> DisplayItems()
{
    g_displayItems = {ui::Text(ui::TextId::VideoDisplayDefault)};
    const std::vector<native::DisplayInfo> displays = native::Displays();
    for (size_t i = 0; i < displays.size(); ++i) {
        const RECT& area = displays[i].area;
        const size_t number = i + 1;
        const long width = area.right - area.left;
        const long height = area.bottom - area.top;
        g_displayItems.push_back(std::vformat(ui::Text(displays[i].primary ? ui::TextId::VideoDisplayPrimary : ui::TextId::VideoDisplayItem),
            std::make_format_args(number, width, height)));
    }
    std::vector<const char*> items;
    for (const std::string& item : g_displayItems)
        items.push_back(item.c_str());
    return items;
}

struct RowSpec {
    Row row;
    const char* name;
    ui::TextId label;
    std::vector<const char*> items;
};

std::vector<RowSpec> RowSpecs()
{
    using ui::Text;
    using ui::TextId;
    return {
        {Row::AntiAliasing, "GmodppAntiAliasing", TextId::SectionAntiAliasing,
            {Text(TextId::AntiAliasingOff), Text(TextId::AntiAliasingTemporal), Text(TextId::VideoAntiAliasingDlss)}},
        {Row::Upscaling, "GmodppUpscaling", TextId::Upscaling,
            {Text(TextId::UpscalingNative), Text(TextId::DlssQuality), Text(TextId::DlssBalanced), Text(TextId::DlssPerformance), Text(TextId::DlssUltraPerformance)}},
        {Row::RealtimeLighting, "GmodppRealtimeLighting", TextId::RealtimeLighting, kOnOff},
        {Row::Quality, "GmodppQuality", TextId::VideoRayTracingQuality,
            {Text(TextId::PresetLow), Text(TextId::PresetMedium), Text(TextId::PresetHigh), Text(TextId::PresetUltra), Text(TextId::PresetCustom)}},
        {Row::IndirectLighting, "GmodppIndirectLighting", TextId::IndirectSource, {Text(TextId::IndirectRayTraced), Text(TextId::IndirectBaked)}},
        {Row::FoliageShadows, "GmodppFoliageShadows", TextId::FoliageShadows,
            {Text(TextId::FoliageOff), Text(TextId::FoliageSun), Text(TextId::FoliageSunAndIndirect)}},
        {Row::Reflections, "GmodppReflections", TextId::Reflections, kOnOff},
        {Row::AmbientOcclusion, "GmodppAmbientOcclusion", TextId::AmbientOcclusion, kOnOff},
        {Row::VolumetricLight, "GmodppVolumetricLight", TextId::VolumetricLight, kOnOff},
        {Row::TranslucentLighting, "GmodppTranslucentLighting", TextId::TranslucentLighting, kOnOff},
        {Row::RenderTargetLighting, "GmodppRenderTargetLighting", TextId::RenderTargetLighting, kOnOff},
        {Row::Display, "GmodppDisplay", TextId::VideoDisplay, DisplayItems()},
    };
}

int RowValue(const Settings& settings, Row row)
{
    switch (row) {
    case Row::AntiAliasing: return static_cast<int>(settings.antiAliasing);
    case Row::Upscaling: return static_cast<int>(settings.upscaling);
    case Row::RealtimeLighting: return settings.realtimeLighting ? 1 : 0;
    case Row::Quality: return static_cast<int>(settings.qualityPreset);
    case Row::IndirectLighting: return static_cast<int>(settings.indirectLighting);
    case Row::FoliageShadows: return static_cast<int>(settings.foliageShadows);
    case Row::Reflections: return settings.reflections ? 1 : 0;
    case Row::AmbientOcclusion: return settings.ambientOcclusion ? 1 : 0;
    case Row::VolumetricLight: return settings.volumetricLight ? 1 : 0;
    case Row::TranslucentLighting: return settings.translucentLighting ? 1 : 0;
    case Row::RenderTargetLighting: return settings.renderTargetLighting ? 1 : 0;
    case Row::Display: return settings.display;
    default: return 0;
    }
}

// Sets one setting from its combo box row. Detail settings a preset decides leave the preset for Custom.
void SetRowValue(Settings& settings, Row row, int value)
{
    switch (row) {
    case Row::AntiAliasing: settings.antiAliasing = static_cast<AntiAliasing>(value); break;
    case Row::Upscaling: settings.upscaling = static_cast<UpscalingMode>(value); break;
    case Row::RealtimeLighting: settings.realtimeLighting = value == 1; break;
    case Row::Quality: ApplyPreset(settings, static_cast<QualityPreset>(value)); break;
    case Row::IndirectLighting:
        settings.indirectLighting = static_cast<IndirectLighting>(value);
        settings.qualityPreset = QualityPreset::Custom;
        break;
    case Row::FoliageShadows:
        settings.foliageShadows = static_cast<FoliageShadows>(value);
        settings.qualityPreset = QualityPreset::Custom;
        break;
    case Row::Reflections: settings.reflections = value == 1; break;
    case Row::AmbientOcclusion: settings.ambientOcclusion = value == 1; break;
    case Row::VolumetricLight: settings.volumetricLight = value == 1; break;
    case Row::TranslucentLighting: settings.translucentLighting = value == 1; break;
    case Row::RenderTargetLighting: settings.renderTargetLighting = value == 1; break;
    case Row::Display: settings.display = value; break;
    default: break;
    }
}

// MSAA is hidden and off: the game's lighting needs it off, and anti-aliasing comes from TAA or DLSS.
void TurnOffMsaa(void* dialog)
{
    void* combo = *reinterpret_cast<void**>(static_cast<std::byte*>(dialog) + g_gameUi.msaaMember);
    if (combo != nullptr)
        CallVirtual<void>(combo, kActivateItemByRow, 0);
}

void ShowSettings(const Added& added)
{
    const Settings& settings = GetSettings();
    for (const RowSpec& spec : RowSpecs()) {
        void* combo = added.combos[static_cast<size_t>(spec.row)];
        if (combo != nullptr)
            CallVirtual<void>(combo, kActivateItemByRow, std::clamp(RowValue(settings, spec.row), 0, static_cast<int>(spec.items.size()) - 1));
    }
    // Real-time lighting ray traces the flashlight's shadows and skips the game's projected shadows, so the shadow
    // detail setting does nothing while it is on.
    if (added.shadowDetail != 0)
        CallPanels<void>(kSetEnabled, added.shadowDetail, !settings.realtimeLighting);
}

// Lays the dialog out again around its changes: the anti-aliasing mode takes MSAA's place and the upscaling mode the
// filtering mode's next to it, which moves to the field of view's when that is hidden (a row of its own otherwise);
// the display goes next to HDR, the ray tracing settings fill three rows under a divider below the game's, the two
// DirectX level rows become one naming the renderer, and the Direct3D 9Ex row goes.
// Positions are taken from the dialog's own controls, so proportional scaling carries over.
bool Extend(void* dialog)
{
    const VPanel root = VPanelOf(dialog);
    if (NameOf(root) != kDialogName) {
        log::Info("Video menu: the Advanced video dialog's panel is named '{}', not {}; left unchanged", NameOf(root), kDialogName);
        return false;
    }
    const std::vector<VPanel> children = ChildrenOf(root);
    auto find = [&](std::string_view name) { return ChildNamed(children, name); };
    const auto modelDetail = find("ModelDetail"), modelLabel = find("Label3"), waterDetail = find("WaterDetail"), textureDetail = find("TextureDetail"),
               shaderDetail = find("ShaderDetail"), shadowDetail = find("ShadowDetail"), msaa = find("AntialiasingMode"), msaaLabel = find("Label5"),
               filtering = find("FilteringMode"), multicore = find("Multicore"), multicoreLabel = find("MulticoreLabel"), hdr = find("HDR"), divider = find("Divider1"), d3d9ex = find("D3D9Ex"), d3d9exLabel = find("D3D9ExLabel"),
               dxLevel = find("dxlabel"), dxLevelLabel = find("Label1"), dxInstalled = find("dxinstalledlabel"), dxInstalledLabel = find("Label2"),
               fov = find("FovSlider");
    if (!modelDetail || !modelLabel || !waterDetail || !textureDetail || !shaderDetail || !shadowDetail || !msaa || !msaaLabel || !filtering || !multicore || !multicoreLabel || !hdr ||
        !divider || !d3d9ex || !d3d9exLabel || !dxLevel || !dxLevelLabel || !dxInstalled || !dxInstalledLabel) {
        log::Info("Video menu: the Advanced video dialog's layout is not the expected one; left unchanged");
        return false;
    }
    const Rect dialogRect = RectOf(root);
    const Rect model = RectOf(*modelDetail);
    const int step = RectOf(*waterDetail).y - model.y;
    const int labelOffset = model.y - RectOf(*modelLabel).y;
    const int labelHeight = RectOf(*modelLabel).h;
    const std::array<int, 3> columns = {model.x, RectOf(*textureDetail).x, RectOf(*shaderDetail).x};
    auto comboAt = [&](int column, int y) { return Rect{columns[static_cast<size_t>(column)], y, model.w, model.h}; };
    auto labelAt = [&](int column, int comboY) {
        const int right = column + 1 < static_cast<int>(columns.size()) ? columns[static_cast<size_t>(column) + 1] : dialogRect.w - model.x;
        return Rect{columns[static_cast<size_t>(column)], comboY - labelOffset, right - columns[static_cast<size_t>(column)] - 4, labelHeight};
    };
    auto shift = [&](int fromY, int by) {
        for (VPanel child : children) {
            if (NameOf(child).starts_with("frame_"))
                continue;
            Rect rect = RectOf(child);
            if (rect.y >= fromY) {
                rect.y += by;
                Place(child, rect);
            }
        }
    };

    // Resizing the dialog puts its controls back where the .res file has them, so it comes first.
    const int d3d9exY = RectOf(*d3d9ex).y;
    const int dxLevelY = RectOf(*dxLevel).y;
    const int d3d9exStep = d3d9exY - dxLevelY;
    const int dxLevelStep = dxLevelY - RectOf(*dxInstalled).y;
    const bool fovSlot = !fov || !CallPanels<bool>(kIsVisible, *fov);
    const int newRows = fovSlot ? 3 : 4;
    // Half a row for the divider between the game's settings and ours.
    const int grow = newRows * step + step / 2;
    const Rect hdrRect = RectOf(*hdr);
    const int firstRowY = hdrRect.y + step + step / 2;
    const int dividerY = RectOf(*divider).y;
    CallPanels<void>(kSetSize, root, dialogRect.w, dialogRect.h + grow - d3d9exStep - dxLevelStep);

    // Direct3D 9Ex does nothing natively.
    CallPanels<void>(kSetVisible, *d3d9ex, false);
    CallPanels<void>(kSetVisible, *d3d9exLabel, false);
    shift(d3d9exY + 1, -d3d9exStep);
    for (VPanel panel : {*dxLevel, *dxLevelLabel, *dxInstalled, *dxInstalledLabel})
        CallPanels<void>(kSetVisible, panel, false);
    shift(dxLevelY + 1, -dxLevelStep);
    shift(dividerY, grow);
    Rect dividerRect = RectOf(*divider);
    dividerRect.y = (hdrRect.y + hdrRect.h + firstRowY - labelOffset) / 2;
    Place(*divider, dividerRect);
    void* renderer = NewLabel(dialog, "GmodppRenderer", ui::Text(ui::TextId::VideoRendererName));
    void* rendererLabel = NewLabel(dialog, "GmodppRendererLabel", ui::Text(ui::TextId::VideoRenderer));
    if (renderer != nullptr && rendererLabel != nullptr) {
        Place(VPanelOf(renderer), RectOf(*dxInstalled));
        Place(VPanelOf(rendererLabel), RectOf(*dxInstalledLabel));
    }

    Added added;
    added.shadowDetail = *shadowDetail;
    const Rect msaaRect = RectOf(*msaa);
    CallPanels<void>(kSetVisible, *msaa, false);
    CallPanels<void>(kSetVisible, *msaaLabel, false);
    const Rect filteringRect = RectOf(*filtering);
    // The filtering mode's label: the nearest child above it in its column.
    std::optional<Rect> filteringLabel;
    for (VPanel child : children) {
        const Rect rect = RectOf(child);
        if (rect.x == filteringRect.x && rect.y < filteringRect.y && (!filteringLabel || rect.y > filteringLabel->y))
            filteringLabel = rect;
    }
    if (!filteringLabel)
        return false;
    const int filteringY = fovSlot ? RectOf(*multicore).y : firstRowY + 3 * step;
    const int filteringColumn = fovSlot ? 1 : 0;
    Place(*filtering, comboAt(filteringColumn, filteringY));
    for (VPanel child : children) {
        const Rect rect = RectOf(child);
        if (rect.x == filteringLabel->x && rect.y == filteringLabel->y) {
            Rect moved = labelAt(filteringColumn, filteringY);
            if (fovSlot)
                moved.y = RectOf(*multicoreLabel).y;
            Place(child, moved);
        }
    }
    for (const RowSpec& spec : RowSpecs()) {
        Rect comboRect, labelRect;
        if (spec.row == Row::AntiAliasing) {
            comboRect = msaaRect;
            labelRect = RectOf(*msaaLabel);
        } else if (spec.row == Row::Upscaling) {
            comboRect = filteringRect;
            labelRect = *filteringLabel;
        } else if (spec.row == Row::Display) {
            comboRect = comboAt(1, hdrRect.y);
            labelRect = labelAt(1, hdrRect.y);
        } else {
            const int index = static_cast<int>(spec.row) - static_cast<int>(Row::RealtimeLighting);
            const int y = firstRowY + (index / 3) * step;
            comboRect = comboAt(index % 3, y);
            labelRect = labelAt(index % 3, y);
        }
        added.combos[static_cast<size_t>(spec.row)] = AddCombo(dialog, spec.name, spec.label, comboRect, labelRect, spec.items);
    }
    g_dialogs[dialog] = added;
    ShowSettings(added);
    TurnOffMsaa(dialog);
    return true;
}

void* __fastcall HookDialog(void* dialog, void* parent, int unknown)
{
    void* result = g_dialog(dialog, parent, unknown);
    if (!Extend(dialog))
        g_dialogs.erase(dialog);
    return result;
}

void __fastcall HookResetData(void* dialog)
{
    g_resetData(dialog);
    if (const auto found = g_dialogs.find(dialog); found != g_dialogs.end()) {
        ShowSettings(found->second);
        TurnOffMsaa(dialog);
    }
}

// The game applies its own settings only when one of them changed; ours are compared with the current ones. A changed
// preset decides the detail rows, which are shown again afterwards.
void __fastcall HookApplyChanges(void* dialog)
{
    g_applyChanges(dialog);
    const auto found = g_dialogs.find(dialog);
    if (found == g_dialogs.end())
        return;
    Settings& settings = GetSettings();
    const Settings before = settings;
    bool presetChanged = false;
    for (const RowSpec& spec : RowSpecs()) {
        void* combo = found->second.combos[static_cast<size_t>(spec.row)];
        const int value = combo != nullptr ? g_gameUi.getActiveItem(combo) : -1;
        if (value < 0 || value >= static_cast<int>(spec.items.size()) || value == RowValue(settings, spec.row))
            continue;
        if (spec.row == Row::Quality)
            presetChanged = true;
        else if ((spec.row == Row::IndirectLighting || spec.row == Row::FoliageShadows) && presetChanged)
            continue;
        SetRowValue(settings, spec.row, value);
    }
    if (!(settings == before))
        MarkSettingsChanged();
    ShowSettings(found->second);
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
    g_gameUi.panels = createInterface != nullptr ? createInterface(kPanelInterface, nullptr) : nullptr;
    void* dialog = FindFunctionReferencingString(gameUi, kDialogResource);
    void* applyChanges = FindFunctionReferencingString(gameUi, kApplyChangesString);
    void* resetData = FindFunctionReferencingString(gameUi, kResetDataString);
    const std::byte* comboBox = nullptr;
    const std::byte* label = nullptr;
    AllocateFn labelAllocate = nullptr;
    const bool constructions = FindConstruction(gameUi, kComboBoxName, g_gameUi.allocate, g_gameUi.comboBoxSize, comboBox) &&
        FindConstruction(gameUi, kLabelName, labelAllocate, g_gameUi.labelSize, label) && labelAllocate == g_gameUi.allocate;
    g_gameUi.getActiveItem = applyChanges != nullptr ? FindGetActiveItem(applyChanges) : nullptr;
    g_gameUi.msaaMember = applyChanges != nullptr ? FindMsaaMember(gameUi, applyChanges) : 0;
    if (g_gameUi.panels == nullptr || dialog == nullptr || applyChanges == nullptr || resetData == nullptr || !constructions ||
        g_gameUi.getActiveItem == nullptr || g_gameUi.msaaMember == 0) {
        log::Info("Video menu: GameUI.dll is not the expected build; the video options are left unchanged");
        return;
    }
    g_gameUi.comboBox = reinterpret_cast<ComboBoxFn>(const_cast<std::byte*>(comboBox));
    g_gameUi.label = reinterpret_cast<LabelFn>(const_cast<std::byte*>(label));

    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log::Info("MinHook initialization failed: {}", MH_StatusToString(status));
        return;
    }
    if (!Hook(applyChanges, reinterpret_cast<void*>(&HookApplyChanges), reinterpret_cast<void**>(&g_applyChanges), "ApplyChanges") ||
        !Hook(resetData, reinterpret_cast<void*>(&HookResetData), reinterpret_cast<void**>(&g_resetData), "OnResetData") ||
        !Hook(dialog, reinterpret_cast<void*>(&HookDialog), reinterpret_cast<void**>(&g_dialog), "COptionsSubVideoAdvancedDlg"))
        return;
    const auto offset = [&](const void* address) { return reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(gameUi); };
    log::Info("Video menu: hooked the Advanced video dialog (GameUI.dll+{:#x}, ComboBox {:#x}, Label {:#x})", offset(dialog), offset(comboBox),
        offset(label));

}

}
