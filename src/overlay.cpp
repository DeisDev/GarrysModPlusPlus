#include "overlay.h"

#include "command_line.h"
#include "log.h"
#include "options.h"
#include "overlay_widgets.h"
#include "settings.h"
#include "shaderapi_device.h"
#include "text_layout.h"
#include "ui_text.h"

#include <MinHook.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <intrin.h>

#include <algorithm>
#include <span>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gmodpp::overlay {

namespace {

using ui::Text;

constexpr UINT kToggleKey = VK_INSERT;

enum class Page { General, Lighting, Quality, Image, Performance, Count };

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using GetCursorPosFn = BOOL(WINAPI*)(POINT*);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, void*, UINT*, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(RAWINPUT*, UINT*, UINT);

struct State {
    HWND window = nullptr;
    WNDPROC previousProc = nullptr;
    DescriptorHeap* descriptors = nullptr;
    bool initialized = false;
    bool open = false;
    Page page = Page::General;
    SetCursorPosFn setCursorPos = nullptr;
    ClipCursorFn clipCursor = nullptr;
    GetCursorPosFn getCursorPos = nullptr;
    GetRawInputDataFn getRawInputData = nullptr;
    GetRawInputBufferFn getRawInputBuffer = nullptr;
    // Where the game last put the cursor for mouse look; while the menu is open it keeps reading this point.
    POINT gameCursor = {};
};

State g_state;

bool CalledByGame(void* returnAddress)
{
    HMODULE caller = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(returnAddress), &caller);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&CalledByGame), &self);
    return caller != self;
}

// The game turns the camera from cursor movement (recentring it every frame) and from raw mouse input. While
// the menu is open it sees a cursor that stays where it put it and raw mouse input without motion or clicks.
BOOL WINAPI HookSetCursorPos(int x, int y)
{
    if (CalledByGame(_ReturnAddress())) {
        g_state.gameCursor = {x, y};
        if (g_state.open)
            return TRUE;
    }
    return g_state.setCursorPos(x, y);
}

BOOL WINAPI HookGetCursorPos(POINT* point)
{
    if (g_state.open && point != nullptr && CalledByGame(_ReturnAddress())) {
        *point = g_state.gameCursor;
        return TRUE;
    }
    return g_state.getCursorPos(point);
}

BOOL WINAPI HookClipCursor(const RECT* rect) { return g_state.clipCursor(g_state.open ? nullptr : rect); }

void SilenceMouse(RAWINPUT& input)
{
    if (input.header.dwType != RIM_TYPEMOUSE)
        return;
    input.data.mouse.lLastX = 0;
    input.data.mouse.lLastY = 0;
    input.data.mouse.usButtonFlags = 0;
    input.data.mouse.usButtonData = 0;
}

UINT WINAPI HookGetRawInputData(HRAWINPUT handle, UINT command, void* data, UINT* size, UINT headerSize)
{
    const UINT result = g_state.getRawInputData(handle, command, data, size, headerSize);
    if (g_state.open && command == RID_INPUT && data != nullptr && result != static_cast<UINT>(-1) && result >= sizeof(RAWINPUTHEADER) &&
        CalledByGame(_ReturnAddress()))
        SilenceMouse(*static_cast<RAWINPUT*>(data));
    return result;
}

UINT WINAPI HookGetRawInputBuffer(RAWINPUT* data, UINT* size, UINT headerSize)
{
    const UINT count = g_state.getRawInputBuffer(data, size, headerSize);
    if (g_state.open && data != nullptr && count != static_cast<UINT>(-1) && CalledByGame(_ReturnAddress())) {
        RAWINPUT* input = data;
        for (UINT i = 0; i < count; ++i) {
            SilenceMouse(*input);
            // NEXTRAWINPUTBLOCK: blocks are 8-byte aligned.
            input = reinterpret_cast<RAWINPUT*>((reinterpret_cast<uintptr_t>(input) + input->header.dwSize + 7) & ~uintptr_t(7));
        }
    }
    return count;
}

bool IsInputMessage(UINT msg)
{
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
}

LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wParam == kToggleKey) {
        if (msg == WM_KEYDOWN && (lParam & (1 << 30)) == 0) {
            g_state.open = !g_state.open;
            log::Info("Overlay: menu {}", g_state.open ? "opened" : "closed");
            if (g_state.open) {
                g_state.getCursorPos(&g_state.gameCursor);
                g_state.clipCursor(nullptr);
            }
        }
        return 0;
    }
    if (g_state.open && g_state.initialized) {
        ImGui_ImplWin32_WndProcHandler(window, msg, wParam, lParam);
        if (IsInputMessage(msg))
            return msg == WM_INPUT ? DefWindowProcW(window, msg, wParam, lParam) : 0;
    }
    return CallWindowProcW(g_state.previousProc, window, msg, wParam, lParam);
}

void AllocateDescriptor(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    const UINT index = g_state.descriptors->Allocate();
    *cpu = g_state.descriptors->Cpu(index);
    *gpu = g_state.descriptors->Gpu(index);
}

void FreeDescriptor(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    const SIZE_T start = g_state.descriptors->Cpu(0).ptr;
    const SIZE_T increment = g_state.descriptors->Cpu(1).ptr - start;
    g_state.descriptors->Free(static_cast<UINT>((cpu.ptr - start) / increment));
}

// The width slider values keep for their text, so every track in the menu ends at the same place.
float ValueWidth() { return ImGui::CalcTextSize(ui::Number(99.99, 2).c_str()).x; }

void BeginRow(const char* key, bool enabled = true, const char* reason = nullptr)
{
    ImGui::PushID(key);
    widgets::BeginRow(Text(key), enabled, reason);
}

bool EndRow(bool clickableLabel = false)
{
    const bool clicked = widgets::EndRow(clickableLabel);
    ImGui::PopID();
    return clicked;
}

// A switch; its label flips it too.
void SwitchRow(const char* key, bool& value, bool enabled = true, const char* reason = nullptr)
{
    BeginRow(key, enabled, reason);
    widgets::Switch("##switch", &value);
    if (EndRow(true))
        value = !value;
}

// A strength slider after the switch that enables it; without a switch (`on` null) it lines up with the others.
void StrengthRow(const char* key, bool* on, float& value, SettingRange range, bool enabled = true, const char* reason = nullptr)
{
    BeginRow(key, enabled, reason);
    const float start = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x - widgets::SwitchSpan();
    // The switch comes first in the line's direction: on the left, or on the right for right-to-left languages.
    const bool rightToLeft = widgets::RightToLeft();
    const float switchX = rightToLeft ? start + width + widgets::SwitchSpan() - widgets::SwitchWidth() : start;
    if (on != nullptr) {
        ImGui::SetCursorPosX(switchX);
        widgets::Switch("##switch", on);
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(rightToLeft ? start : start + widgets::SwitchSpan());
    ImGui::BeginDisabled(on != nullptr && !*on);
    widgets::Slider("##value", &value, range.minimum, range.maximum, ui::Number(value, 2).c_str(), ValueWidth(), width);
    ImGui::EndDisabled();
    if (EndRow(on != nullptr))
        *on = !*on;
}

void SliderRow(const char* key, float& value, SettingRange range, bool enabled = true, const char* reason = nullptr)
{
    BeginRow(key, enabled, reason);
    widgets::Slider("##value", &value, range.minimum, range.maximum, ui::Number(value, 2).c_str(), ValueWidth());
    EndRow();
}

void SliderRow(const char* key, int& value, SettingRange range, bool enabled = true, const char* reason = nullptr)
{
    BeginRow(key, enabled, reason);
    widgets::Slider("##value", &value, static_cast<int>(range.minimum), static_cast<int>(range.maximum), ui::Number(value, 0).c_str(), ValueWidth());
    EndRow();
}

// A full-width drop-down list of `options` (text keys) for an enum setting.
template <class E>
void ComboRow(const char* key, E& value, std::span<const char* const> options, bool enabled = true, const char* reason = nullptr)
{
    BeginRow(key, enabled, reason);
    std::vector<const char*> texts;
    for (const char* option : options)
        texts.push_back(Text(option));
    int index = static_cast<int>(value);
    widgets::Combo("##combo", &index, texts);
    value = static_cast<E>(index);
    EndRow();
}

// A row of buttons, one per option (text keys); returns true when another option is picked.
template <class E>
bool SegmentRow(const char* key, E& value, std::span<const char* const> options, std::span<const bool> available = {})
{
    BeginRow(key);
    std::vector<const char*> texts;
    for (const char* option : options)
        texts.push_back(Text(option));
    int index = static_cast<int>(value);
    const bool changed = widgets::Segmented("##segments", &index, texts, available);
    value = static_cast<E>(index);
    EndRow();
    return changed;
}

// A read-only figure.
void ValueRow(const char* key, const std::string& value)
{
    BeginRow(key);
    ImGui::AlignTextToFramePadding();
    widgets::Text(value.c_str());
    EndRow();
}

void DisplayRow(Settings& settings)
{
    const std::vector<native::DisplayInfo> displays = native::Displays();
    auto name = [&](int display) {
        if (display <= 0 || display > static_cast<int>(displays.size()))
            return std::string(Text("general.display_default"));
        const native::DisplayInfo& info = displays[static_cast<size_t>(display) - 1];
        return ui::Format(info.primary ? "general.display_primary" : "general.display_item",
            {{"number", std::to_string(display)}, {"width", std::to_string(info.area.right - info.area.left)},
                {"height", std::to_string(info.area.bottom - info.area.top)}});
    };
    std::vector<std::string> names;
    for (int i = 0; i <= static_cast<int>(displays.size()); ++i)
        names.push_back(name(i));
    std::vector<const char*> texts;
    for (const std::string& text : names)
        texts.push_back(text.c_str());
    BeginRow("general.display");
    // A display that is gone shows as Default, which is where the game window then stays.
    int index = settings.display <= static_cast<int>(displays.size()) ? settings.display : 0;
    if (widgets::Combo("##combo", &index, texts))
        settings.display = index;
    EndRow();
}

// Each settings page resets what it shows, and nothing else.
void ResetGeneralPage(Settings& settings)
{
    const Settings defaults;
    settings.realtimeLighting = defaults.realtimeLighting;
    settings.display = defaults.display;
    settings.debugView = defaults.debugView;
}

void ResetLightingPage(Settings& settings)
{
    const Settings defaults;
    settings.sun = defaults.sun;
    settings.sunIntensity = defaults.sunIntensity;
    settings.skyIntensity = defaults.skyIntensity;
    settings.mapLights = defaults.mapLights;
    settings.lightIntensity = defaults.lightIntensity;
    settings.dynamicLights = defaults.dynamicLights;
    settings.projectedLights = defaults.projectedLights;
    settings.projectedIntensity = defaults.projectedIntensity;
    settings.indirectLight = defaults.indirectLight;
    settings.indirectIntensity = defaults.indirectIntensity;
    settings.ambientOcclusion = defaults.ambientOcclusion;
    settings.aoIntensity = defaults.aoIntensity;
    settings.reflections = defaults.reflections;
    settings.reflectionIntensity = defaults.reflectionIntensity;
    settings.glossyReflections = defaults.glossyReflections;
    settings.volumetricLight = defaults.volumetricLight;
    settings.volumetricIntensity = defaults.volumetricIntensity;
    settings.fog = defaults.fog;
    settings.physicalSky = defaults.physicalSky;
    settings.sunDisc = defaults.sunDisc;
    settings.sunSize = defaults.sunSize;
    settings.translucentLighting = defaults.translucentLighting;
    settings.renderTargetLighting = defaults.renderTargetLighting;
}

void ResetQualityPage(Settings& settings) { ApplyPreset(settings, Settings{}.qualityPreset); }

void ResetImagePage(Settings& settings)
{
    const Settings defaults;
    settings.antiAliasing = defaults.antiAliasing;
    settings.upscaling = defaults.upscaling;
    settings.taaHistoryWeight = defaults.taaHistoryWeight;
    settings.sharpening = defaults.sharpening;
    settings.denoiser = defaults.denoiser;
    settings.denoiserType = defaults.denoiserType;
}

void DrawGeneralPage(Settings& settings)
{
    widgets::BeginCard("##general");
    if (widgets::BeginRows("general")) {
        SwitchRow("general.realtime_lighting", settings.realtimeLighting);
        DisplayRow(settings);
        widgets::EndRows();
    }
    constexpr const char* kBlocks[] = {nullptr, "general.realtime_unsupported", "general.realtime_other_thread"};
    const RealtimeBlock block = GetStatistics().realtimeBlock;
    if (settings.realtimeLighting && block != RealtimeBlock::None)
        widgets::Text(Text(kBlocks[static_cast<size_t>(block)]));
    widgets::EndCard();

    widgets::BeginCard("##developer", Text("general.section_developer"));
    if (widgets::BeginRows("developer")) {
        constexpr const char* kViews[] = {"debug.final", "debug.albedo", "debug.normals", "debug.coverage", "debug.direct_light", "debug.indirect_light",
            "debug.indirect_raw", "debug.sun_visibility", "debug.additive_light", "debug.reflections", "debug.motion_vectors", "debug.ambient_occlusion",
            "debug.radiance_cache", "debug.light_sample", "debug.reservoir_samples", "debug.denoiser_validation"};
        static_assert(std::size(kViews) == static_cast<size_t>(DebugView::Count));
        ComboRow("general.debug_view", settings.debugView, kViews);
        widgets::EndRows();
    }
    widgets::EndCard();

    widgets::BeginCard("##credits", Text("general.section_credits"));
    widgets::Note(Text("general.credits_nvidia"));
    widgets::Note(Text("general.credits_license"));
    widgets::EndCard();
}

void DrawLightingPage(Settings& settings)
{
    widgets::BeginCard("##sources", Text("lighting.section_sources"));
    if (widgets::BeginRows("sources")) {
        StrengthRow("lighting.sun", &settings.sun, settings.sunIntensity, RangeOf(&Settings::sunIntensity));
        StrengthRow("lighting.sky", nullptr, settings.skyIntensity, RangeOf(&Settings::skyIntensity));
        StrengthRow("lighting.map_lights", &settings.mapLights, settings.lightIntensity, RangeOf(&Settings::lightIntensity));
        SwitchRow("lighting.dynamic_lights", settings.dynamicLights);
        StrengthRow("lighting.projected_lights", &settings.projectedLights, settings.projectedIntensity, RangeOf(&Settings::projectedIntensity));
        widgets::EndRows();
    }
    widgets::EndCard();

    widgets::BeginCard("##indirect", Text("lighting.section_indirect"));
    if (widgets::BeginRows("indirect")) {
        StrengthRow("lighting.indirect_light", &settings.indirectLight, settings.indirectIntensity, RangeOf(&Settings::indirectIntensity));
        // Traced indirect light holds its own occlusion.
        StrengthRow("lighting.ambient_occlusion", &settings.ambientOcclusion, settings.aoIntensity, RangeOf(&Settings::aoIntensity),
            settings.indirectLighting == IndirectLighting::Baked, Text("lighting.ambient_occlusion_reason"));
        widgets::EndRows();
    }
    widgets::EndCard();

    widgets::BeginCard("##reflections", Text("lighting.section_reflections"));
    if (widgets::BeginRows("reflections")) {
        StrengthRow("lighting.reflections", &settings.reflections, settings.reflectionIntensity, RangeOf(&Settings::reflectionIntensity));
        SwitchRow("lighting.glossy_reflections", settings.glossyReflections, settings.reflections, Text("lighting.glossy_reflections_reason"));
        widgets::EndRows();
    }
    widgets::EndCard();

    widgets::BeginCard("##atmosphere", Text("lighting.section_atmosphere"));
    if (widgets::BeginRows("atmosphere")) {
        SwitchRow("lighting.physical_sky", settings.physicalSky);
        SwitchRow("lighting.sun_disc", settings.sunDisc);
        SliderRow("lighting.sun_size", settings.sunSize, RangeOf(&Settings::sunSize), settings.sunDisc, Text("lighting.sun_size_reason"));
        StrengthRow("lighting.volumetric_light", &settings.volumetricLight, settings.volumetricIntensity, RangeOf(&Settings::volumetricIntensity));
        SwitchRow("lighting.fog", settings.fog);
        widgets::EndRows();
    }
    widgets::EndCard();

    widgets::BeginCard("##surfaces", Text("lighting.section_surfaces"));
    if (widgets::BeginRows("surfaces")) {
        SwitchRow("lighting.translucent_surfaces", settings.translucentLighting);
        SwitchRow("lighting.render_targets", settings.renderTargetLighting);
        widgets::EndRows();
    }
    widgets::EndCard();
}

// Every setting on this page is set by the quality preset, so changing one makes the preset Custom.
void DrawQualityPage(Settings& settings)
{
    const Statistics& statistics = GetStatistics();
    widgets::BeginCard("##preset");
    if (widgets::BeginRows("preset")) {
        constexpr const char* kPresets[] = {"quality.preset_low", "quality.preset_medium", "quality.preset_high", "quality.preset_ultra", "quality.preset_custom"};
        static_assert(std::size(kPresets) == static_cast<size_t>(QualityPreset::Count));
        QualityPreset preset = settings.qualityPreset;
        if (SegmentRow("quality.preset", preset, kPresets))
            ApplyPreset(settings, preset);
        widgets::EndRows();
    }
    widgets::Note(Text("quality.preset_note"));
    widgets::EndCard();

    widgets::BeginCard("##shadows", Text("quality.section_shadows"));
    if (widgets::BeginRows("shadows")) {
        constexpr const char* kSampling[] = {"quality.sampling_legacy", "quality.sampling_ris", "quality.sampling_restir", "quality.sampling_auto"};
        static_assert(std::size(kSampling) == static_cast<size_t>(LightSampling::Count));
        ComboRow("quality.light_sampling", settings.lightSampling, kSampling);
        // The direct light pass that uses these runs for Legacy, which Auto picks with DLSS Ray Reconstruction
        // (Lighting::Run).
        const bool directPass = settings.lightSampling == LightSampling::Legacy ||
            (settings.lightSampling == LightSampling::Auto && settings.antiAliasing == AntiAliasing::RayReconstruction);
        const char* directReason = Text("quality.direct_pass_reason");
        SliderRow("quality.exact_lights", settings.exactLights, RangeOf(&Settings::exactLights), directPass, directReason);
        SliderRow("quality.sampled_light_rays", settings.sampledLightRays, RangeOf(&Settings::sampledLightRays), directPass, directReason);
        SliderRow("quality.viewmodel_shadow_rays", settings.viewmodelShadowRays, RangeOf(&Settings::viewmodelShadowRays));
        constexpr const char* kFoliage[] = {"quality.foliage_off", "quality.foliage_sun", "quality.foliage_sun_indirect", "quality.foliage_all"};
        static_assert(std::size(kFoliage) == static_cast<size_t>(FoliageShadows::Count));
        ComboRow("quality.foliage_shadows", settings.foliageShadows, kFoliage);
        widgets::EndRows();
    }
    widgets::EndCard();

    widgets::BeginCard("##indirect", Text("quality.section_indirect"));
    const bool cacheAvailable = statistics.radianceCacheAvailable;
    if (widgets::BeginRows("indirect")) {
        constexpr const char* kSources[] = {"quality.indirect_ray_traced", "quality.indirect_baked"};
        static_assert(std::size(kSources) == static_cast<size_t>(IndirectLighting::Count));
        SegmentRow("quality.indirect_source", settings.indirectLighting, kSources);
        const bool baked = settings.indirectLighting == IndirectLighting::Baked;
        SliderRow("quality.indirect_rays", settings.indirectRays, RangeOf(&Settings::indirectRays));
        SliderRow("quality.indirect_bounces", settings.indirectBounces, RangeOf(&Settings::indirectBounces));
        SwitchRow("quality.radiance_cache", settings.radianceCache, cacheAvailable && !baked,
            Text(cacheAvailable ? "quality.radiance_cache_reason" : "quality.radiance_cache_unavailable"));
        SliderRow("quality.volume_rays", settings.volumeRays, RangeOf(&Settings::volumeRays));
        SliderRow("quality.ao_rays", settings.aoRays, RangeOf(&Settings::aoRays), baked, Text("quality.ao_rays_reason"));
        widgets::EndRows();
    }
    if (!cacheAvailable)
        widgets::Note(Text("quality.radiance_cache_unavailable"));
    widgets::EndCard();

    widgets::BeginCard("##effects", Text("quality.section_effects"));
    if (widgets::BeginRows("effects")) {
        constexpr const char* kEmissive[] = {"quality.emissive_off", "quality.emissive_bounce", "quality.emissive_full"};
        static_assert(std::size(kEmissive) == static_cast<size_t>(EmissiveLighting::Count));
        SegmentRow("quality.emissive", settings.emissiveLighting, kEmissive);
        SliderRow("quality.volumetric_steps", settings.volumetricSteps, RangeOf(&Settings::volumetricSteps), settings.volumetricLight,
            Text("quality.volumetric_steps_reason"));
        widgets::EndRows();
    }
    widgets::EndCard();

    widgets::BeginCard("##alpha", Text("quality.section_alpha"));
    const bool micromapsAvailable = statistics.opacityMicromapsAvailable;
    if (widgets::BeginRows("alpha")) {
        SwitchRow("quality.opacity_micromaps", settings.opacityMicromaps, micromapsAvailable, Text("quality.micromaps_unavailable"));
        SwitchRow("quality.fast_alpha_tests", settings.fastAlphaTests, micromapsAvailable && settings.opacityMicromaps,
            Text(micromapsAvailable ? "quality.fast_alpha_tests_reason" : "quality.micromaps_unavailable"));
        widgets::EndRows();
    }
    if (!micromapsAvailable)
        widgets::Note(Text("quality.micromaps_unavailable"));
    widgets::EndCard();

    if (settings.qualityPreset != QualityPreset::Custom) {
        Settings preset = settings;
        ApplyPreset(preset, settings.qualityPreset);
        if (preset != settings)
            settings.qualityPreset = QualityPreset::Custom;
    }
}

void DrawImagePage(Settings& settings)
{
    const Statistics& statistics = GetStatistics();
    const bool rayReconstruction = settings.antiAliasing == AntiAliasing::RayReconstruction;
    widgets::BeginCard("##antialiasing", Text("image.section_anti_aliasing"));
    if (widgets::BeginRows("antialiasing")) {
        constexpr const char* kModes[] = {"image.anti_aliasing_off", "image.anti_aliasing_taa", "image.anti_aliasing_dlss"};
        static_assert(std::size(kModes) == static_cast<size_t>(AntiAliasing::Count));
        const bool available[] = {true, true, statistics.rayReconstructionAvailable};
        SegmentRow("image.anti_aliasing", settings.antiAliasing, kModes, available);
        constexpr const char* kUpscaling[] = {"image.upscaling_native", "image.upscaling_quality", "image.upscaling_balanced", "image.upscaling_performance",
            "image.upscaling_ultra_performance"};
        static_assert(std::size(kUpscaling) == static_cast<size_t>(UpscalingMode::Count));
        ComboRow("image.upscaling", settings.upscaling, kUpscaling, rayReconstruction, Text("image.upscaling_reason"));
        const bool temporal = settings.antiAliasing == AntiAliasing::Temporal;
        SliderRow("image.history_weight", settings.taaHistoryWeight, RangeOf(&Settings::taaHistoryWeight), temporal, Text("image.taa_reason"));
        SliderRow("image.sharpening", settings.sharpening, RangeOf(&Settings::sharpening), temporal, Text("image.taa_reason"));
        ValueRow("image.render_resolution",
            statistics.renderWidth != 0 ? ui::Format("image.render_resolution_value",
                                              {{"width", std::to_string(statistics.renderWidth)}, {"height", std::to_string(statistics.renderHeight)}})
                                        : std::string(Text("image.render_resolution_unknown")));
        widgets::EndRows();
    }
    if (!statistics.rayReconstructionAvailable)
        widgets::Note(Text("image.dlss_unavailable"));
    widgets::EndCard();

    widgets::BeginCard("##denoiser", Text("image.section_denoiser"));
    if (widgets::BeginRows("denoiser")) {
        SwitchRow("image.denoiser", settings.denoiser);
        constexpr const char* kDenoisers[] = {"image.denoiser_built_in", "image.denoiser_relax", "image.denoiser_reblur"};
        static_assert(std::size(kDenoisers) == static_cast<size_t>(DenoiserType::Count));
        // DLSS Ray Reconstruction denoises by itself.
        ComboRow("image.denoiser_type", settings.denoiserType, kDenoisers, settings.denoiser && !rayReconstruction,
            Text(rayReconstruction ? "image.denoiser_type_dlss_reason" : "image.denoiser_type_reason"));
        widgets::EndRows();
    }
    widgets::EndCard();
}

std::string Milliseconds(double ms) { return ui::Format("performance.milliseconds", {{"value", ui::Number(ms, 2)}}); }

// A table cell's text at the end of the line: right-aligned, or left-aligned for right-to-left languages.
void EndAligned(const std::string& text)
{
    const std::string visual = ui::Visual(text);
    if (!widgets::RightToLeft())
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(visual.c_str()).x);
    ImGui::TextUnformatted(visual.c_str());
}

// Frame rate figures over the recorded history. Lows average the slowest 1% / 0.1% of frames.
void DrawFrameRate(const Statistics& statistics)
{
    const size_t count = statistics.frameCount;
    if (count == 0)
        return;
    std::vector<float> chronological(count);
    const size_t first = count == Statistics::kFrameHistory ? statistics.nextFrame : 0;
    for (size_t i = 0; i < count; ++i)
        chronological[i] = statistics.frameMs[(first + i) % Statistics::kFrameHistory];
    std::vector<float> sorted = chronological;
    std::sort(sorted.begin(), sorted.end());
    double total = 0.0;
    for (float ms : sorted)
        total += ms;
    auto slowestAverage = [&](double fraction) {
        const size_t n = std::max<size_t>(1, static_cast<size_t>(static_cast<double>(count) * fraction));
        double sum = 0.0;
        for (size_t i = count - n; i < count; ++i)
            sum += sorted[i];
        return sum / static_cast<double>(n);
    };
    auto fps = [](double ms) { return ms > 0.0 ? 1000.0 / ms : 0.0; };
    const size_t recent = std::min<size_t>(count, 30);
    double recentTotal = 0.0;
    for (size_t i = count - recent; i < count; ++i)
        recentTotal += chronological[i];
    const double average = total / static_cast<double>(count);

    widgets::Heading(Text("performance.section_frame_rate"));
    constexpr const char* kLabels[] = {"performance.fps_now", "performance.fps_average", "performance.fps_median", "performance.fps_low_1",
        "performance.fps_low_01"};
    const double rates[] = {fps(recentTotal / static_cast<double>(recent)), fps(average), fps(sorted[count / 2]), fps(slowestAverage(0.01)),
        fps(slowestAverage(0.001))};
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = (ImGui::GetContentRegionAvail().x - spacing * static_cast<float>(std::size(kLabels) - 1)) / static_cast<float>(std::size(kLabels));
    for (size_t n = 0; n < std::size(kLabels); ++n) {
        const size_t i = widgets::RightToLeft() ? std::size(kLabels) - 1 - n : n;
        if (n > 0)
            ImGui::SameLine();
        widgets::Tile(ui::Number(rates[i], 0).c_str(), Text(kLabels[i]), width);
    }
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

    widgets::BeginCard("##frametime", Text("performance.section_frame_time"));
    widgets::Text(ui::Format("performance.frame_time_summary", {{"average", ui::Number(average, 2)},
                                                                 {"p99", ui::Number(sorted[std::min(count - 1, count * 99 / 100)], 2)}})
                      .c_str());
    ImGui::PlotLines("##frametimes", chronological.data(), static_cast<int>(count), 0, nullptr, 0.0f, std::max(sorted.back(), 1.0f),
        ImVec2(-FLT_MIN, ImGui::GetFontSize() * 4.0f));
    widgets::Note(ui::Format("performance.frame_time_range", {{"min", ui::Number(sorted.front(), 2)}, {"max", ui::Number(sorted.back(), 2)}}).c_str());
    widgets::EndCard();
}

void DrawPerformancePage()
{
    const Statistics& statistics = GetStatistics();
    DrawFrameRate(statistics);

    widgets::BeginCard("##passes", Text("performance.section_gpu_passes"));
    constexpr const char* kPasses[] = {"pass.scene_update", "pass.surface", "pass.radiance_cache", "pass.volume", "pass.direct_light", "pass.light_sampling",
        "pass.ambient_occlusion", "pass.indirect_trace", "pass.reflections", "pass.accumulation", "pass.denoiser", "pass.lighting", "pass.resolve"};
    static_assert(std::size(kPasses) == static_cast<size_t>(GpuPass::Count));
    if (ImGui::BeginTable("passes", 3, ImGuiTableFlags_SizingStretchProp)) {
        // Name, bar and time, from right to left for right-to-left languages.
        const int nameColumn = widgets::RightToLeft() ? 2 : 0;
        const int timeColumn = 2 - nameColumn;
        for (int column = 0; column < 3; ++column) {
            if (column == timeColumn)
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize(Milliseconds(100.0).c_str()).x);
            else
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, column == nameColumn ? 0.45f : 0.55f);
        }
        double longest = statistics.otherViewsMs;
        for (double ms : statistics.passMs)
            longest = std::max(longest, ms);
        auto passRow = [&](const char* label, double ms) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(nameColumn);
            widgets::Text(Text(label));
            ImGui::TableSetColumnIndex(1);
            widgets::Meter(longest > 0.0 ? static_cast<float>(ms / longest) : 0.0f);
            ImGui::TableSetColumnIndex(timeColumn);
            EndAligned(Milliseconds(ms));
        };
        for (size_t i = 0; i < std::size(kPasses); ++i)
            passRow(kPasses[i], statistics.passMs[i]);
        passRow("pass.other_views", statistics.otherViewsMs);
        ImGui::TableNextRow();
        ImGui::PushFont(widgets::GetFonts().strong);
        ImGui::TableSetColumnIndex(nameColumn);
        widgets::Text(Text("performance.total"));
        ImGui::TableSetColumnIndex(timeColumn);
        EndAligned(Milliseconds(statistics.lightingMs));
        ImGui::PopFont();
        ImGui::EndTable();
    }
    widgets::EndCard();

    widgets::BeginCard("##statistics", Text("performance.section_statistics"));
    if (widgets::BeginRows("statistics")) {
        ValueRow("performance.map_lights", ui::Number(statistics.mapLights, 0));
        ValueRow("performance.dynamic_lights", ui::Number(statistics.dynamicLights, 0));
        ValueRow("performance.projected_lights", ui::Number(statistics.projectedLights, 0));
        ValueRow("performance.instances", ui::Number(statistics.sceneInstances, 0));
        ValueRow("performance.patched_shaders", ui::Number(statistics.gbufferShaders, 0));
        ValueRow("performance.replaced_shaders", ui::Number(statistics.replacedShaders, 0));
        ValueRow("performance.micromap_triangles", ui::Number(statistics.micromapTriangles, 0));
        ValueRow("performance.micromaps", ui::Number(statistics.micromaps, 0));
        widgets::EndRows();
    }
    widgets::EndCard();
}

struct PageInfo {
    const char* key;
    void (*draw)(Settings&);
    void (*reset)(Settings&);
};

constexpr PageInfo kPages[] = {
    {"page.general", &DrawGeneralPage, &ResetGeneralPage},
    {"page.lighting", &DrawLightingPage, &ResetLightingPage},
    {"page.quality", &DrawQualityPage, &ResetQualityPage},
    {"page.image", &DrawImagePage, &ResetImagePage},
    {"page.performance", [](Settings&) { DrawPerformancePage(); }, nullptr},
};
static_assert(std::size(kPages) == static_cast<size_t>(Page::Count));

// Returns false when the close button was clicked.
bool DrawMenu()
{
    Settings& settings = GetSettings();
    const Settings before = settings;
    const float font = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(font * 1.5f, font * 1.5f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(font * 46.0f, font * 34.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(font * 32.0f, font * 18.0f), ImVec2(FLT_MAX, FLT_MAX));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    bool keepOpen = true;
    if (ImGui::Begin("###gmodpp_menu", nullptr, kFlags)) {
        keepOpen = !widgets::Header(Text("menu.title"), Text("menu.close_hint"));

        const ImGuiStyle& style = ImGui::GetStyle();
        ImFont* navFont = widgets::GetFonts().strong;
        float navWidth = 0.0f;
        for (const PageInfo& page : kPages)
            navWidth = std::max(navWidth, navFont->CalcTextSizeA(navFont->FontSize, FLT_MAX, 0.0f, ui::Visual(Text(page.key)).c_str()).x);
        navWidth += style.FramePadding.x * 4.0f;
        const float gap = style.ItemSpacing.x * 2.0f;
        auto drawNav = [&] {
            if (ImGui::BeginChild("##nav", ImVec2(navWidth, 0.0f))) {
                for (size_t i = 0; i < std::size(kPages); ++i) {
                    if (widgets::NavItem(Text(kPages[i].key), g_state.page == static_cast<Page>(i)))
                        g_state.page = static_cast<Page>(i);
                }
            }
            ImGui::EndChild();
        };
        const PageInfo& page = kPages[static_cast<size_t>(g_state.page)];
        auto drawPage = [&](float width) {
            const float footer = page.reset != nullptr ? ImGui::GetFrameHeight() + style.ItemSpacing.y : 0.0f;
            ImGui::BeginGroup();
            // Each page keeps its own scroll position.
            ImGui::PushID(static_cast<int>(g_state.page));
            if (ImGui::BeginChild("##page", ImVec2(width, -footer)))
                page.draw(settings);
            ImGui::EndChild();
            ImGui::PopID();
            if (page.reset != nullptr && widgets::EndButton(Text("menu.reset_page")))
                page.reset(settings);
            ImGui::EndGroup();
        };

        // The page list is at the start of the line: left, or right for right-to-left languages.
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
        if (widgets::RightToLeft()) {
            drawPage(ImGui::GetContentRegionAvail().x - navWidth - gap);
            ImGui::SameLine(0.0f, gap);
            drawNav();
            const float divider = ImGui::GetItemRectMin().x - style.ItemSpacing.x;
            draw->AddLine(ImVec2(divider, ImGui::GetItemRectMin().y), ImVec2(divider, ImGui::GetItemRectMax().y), border);
        } else {
            drawNav();
            const float divider = ImGui::GetItemRectMax().x + style.ItemSpacing.x;
            draw->AddLine(ImVec2(divider, ImGui::GetItemRectMin().y), ImVec2(divider, ImGui::GetItemRectMax().y), border);
            ImGui::SameLine(0.0f, gap);
            drawPage(0.0f);
        }
    }
    ImGui::End();

    if (before != settings)
        MarkSettingsChanged();
    return keepOpen;
}

bool HookInputFunctions()
{
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log::Info("Overlay: MinHook initialization failed: {}", MH_StatusToString(status));
        return false;
    }
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const std::pair<const char*, std::pair<void*, void**>> hooks[] = {
        {"SetCursorPos", {reinterpret_cast<void*>(&HookSetCursorPos), reinterpret_cast<void**>(&g_state.setCursorPos)}},
        {"GetCursorPos", {reinterpret_cast<void*>(&HookGetCursorPos), reinterpret_cast<void**>(&g_state.getCursorPos)}},
        {"ClipCursor", {reinterpret_cast<void*>(&HookClipCursor), reinterpret_cast<void**>(&g_state.clipCursor)}},
        {"GetRawInputData", {reinterpret_cast<void*>(&HookGetRawInputData), reinterpret_cast<void**>(&g_state.getRawInputData)}},
        {"GetRawInputBuffer", {reinterpret_cast<void*>(&HookGetRawInputBuffer), reinterpret_cast<void**>(&g_state.getRawInputBuffer)}},
    };
    for (const auto& [name, hook] : hooks) {
        void* target = reinterpret_cast<void*>(GetProcAddress(user32, name));
        status = target != nullptr ? MH_CreateHook(target, hook.first, hook.second) : MH_ERROR_FUNCTION_NOT_FOUND;
        if (status == MH_OK)
            status = MH_EnableHook(target);
        if (status != MH_OK) {
            log::Info("Overlay: hooking {} failed: {}", name, MH_StatusToString(status));
            return false;
        }
    }
    return true;
}

}

bool Initialize(HWND window, ID3D12Device* device, ID3D12CommandQueue* queue, DescriptorHeap& descriptors, DXGI_FORMAT format, UINT framesInFlight)
{
    if (g_state.initialized)
        return true;
    g_state.window = window;
    g_state.descriptors = &descriptors;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Keeps the menu's size and position between sessions.
    static const std::string iniPath = (ModDirectory() / L"menu.ini").string();
    io.IniFilename = iniPath.c_str();
    io.MouseDrawCursor = true;
    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor);
    widgets::LoadTheme(GetOptions().gameDirectory / L"garrysmod" / L"resource" / L"fonts", monitor.rcMonitor.bottom - monitor.rcMonitor.top);

    ImGui_ImplDX12_InitInfo info;
    info.Device = device;
    info.CommandQueue = queue;
    info.NumFramesInFlight = static_cast<int>(framesInFlight);
    info.RTVFormat = format;
    info.SrvDescriptorHeap = g_state.descriptors->Heap();
    info.SrvDescriptorAllocFn = &AllocateDescriptor;
    info.SrvDescriptorFreeFn = &FreeDescriptor;
    if (!ImGui_ImplWin32_Init(window) || !ImGui_ImplDX12_Init(&info)) {
        log::Info("Overlay: Dear ImGui initialization failed");
        return false;
    }
    if (!HookInputFunctions())
        return false;
    g_state.previousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
    if (g_state.previousProc == nullptr) {
        log::Info("Overlay: subclassing the game window failed ({})", GetLastError());
        return false;
    }
    g_state.initialized = true;
    log::Info("Overlay: ready (Insert opens the menu)");
    return true;
}

bool Open() { return g_state.initialized && g_state.open; }

void EnsureWindowHook()
{
    if (!g_state.initialized)
        return;
    // The game installs its own window procedure after the device exists; chain in front of it again.
    const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_state.window, GWLP_WNDPROC));
    if (current == &WindowProc)
        return;
    g_state.previousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_state.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
    log::Info("Overlay: the game replaced the window procedure; hooked again");
}

void Render(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    ui::UpdateLanguage();
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (!DrawMenu()) {
        g_state.open = false;
        log::Info("Overlay: menu closed");
    }
    ImGui::Render();
    ID3D12DescriptorHeap* heaps[] = {g_state.descriptors->Heap()};
    list->SetDescriptorHeaps(1, heaps);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
}

}
