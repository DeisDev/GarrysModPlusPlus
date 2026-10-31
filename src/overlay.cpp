#include "overlay.h"

#include "command_line.h"
#include "log.h"
#include "options.h"
#include "overlay_widgets.h"
#include "settings.h"
#include "shaderapi_device.h"
#include "ui_text.h"

#include <MinHook.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <intrin.h>

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gmodpp::overlay {

namespace {

using ui::Text;
using ui::TextId;

constexpr UINT kToggleKey = VK_INSERT;

enum class Page { General, Lighting, RayTracing, Image, Performance, Count };

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

template <class... Args>
std::string Format(TextId id, Args&&... args)
{
    return std::vformat(Text(id), std::make_format_args(args...));
}

template <class... Args>
void TextLine(TextId id, Args&&... args)
{
    ImGui::TextUnformatted(Format(id, args...).c_str());
}

bool BeginRows(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp))
        return false;
    ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, 0.58f);
    return true;
}

// Starts a table row with `label` on the left; the control goes on the right.
void Row(TextId label)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(label));
    ImGui::TableNextColumn();
}

// A switch (a plain label without one) and an optional intensity slider, which is off while the switch is.
void ToggleRow(TextId label, bool* enabled, Settings& settings, float Settings::* intensity)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(static_cast<int>(label));
    if (enabled != nullptr) {
        widgets::Toggle(Text(label), enabled);
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Text(label));
    }
    ImGui::TableNextColumn();
    if (intensity != nullptr) {
        const SettingRange range = RangeOf(intensity);
        ImGui::BeginDisabled(enabled != nullptr && !*enabled);
        widgets::Slider("##intensity", &(settings.*intensity), range.minimum, range.maximum);
        ImGui::EndDisabled();
    }
    ImGui::PopID();
}

void ToggleRow(TextId label, bool& enabled, Settings& settings, float Settings::* intensity) { ToggleRow(label, &enabled, settings, intensity); }

// A switch alone in the left column; returns true when it is flipped.
bool SwitchRow(TextId label, bool& enabled)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(static_cast<int>(label));
    const bool flipped = widgets::Toggle(Text(label), &enabled);
    ImGui::PopID();
    ImGui::TableNextColumn();
    return flipped;
}

void SliderRow(TextId label, Settings& settings, float Settings::* field)
{
    Row(label);
    ImGui::PushID(static_cast<int>(label));
    const SettingRange range = RangeOf(field);
    widgets::Slider("##value", &(settings.*field), range.minimum, range.maximum);
    ImGui::PopID();
}

// A whole-number slider; changing it leaves the quality preset.
void IntSliderRow(TextId label, Settings& settings, int Settings::* field)
{
    Row(label);
    ImGui::PushID(static_cast<int>(label));
    const SettingRange range = RangeOf(field);
    if (widgets::Slider("##value", &(settings.*field), static_cast<int>(range.minimum), static_cast<int>(range.maximum)))
        settings.qualityPreset = QualityPreset::Custom;
    ImGui::PopID();
}

// A full-width drop-down list; returns true when another option is picked.
template <class E>
bool ComboRow(TextId label, E& value, std::span<const TextId> options)
{
    Row(label);
    ImGui::PushID(static_cast<int>(label));
    const int current = static_cast<int>(value);
    bool changed = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##combo", Text(options[static_cast<size_t>(current)]))) {
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            if (ImGui::Selectable(Text(options[static_cast<size_t>(i)]), i == current) && i != current) {
                value = static_cast<E>(i);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

// A row of buttons, one per option; returns true when another option is picked.
template <class E>
bool SegmentRow(TextId label, E& value, std::span<const TextId> options, std::span<const bool> available = {})
{
    Row(label);
    std::vector<const char*> texts;
    for (TextId option : options)
        texts.push_back(Text(option));
    int index = static_cast<int>(value);
    const bool changed = widgets::Segmented(Text(label), &index, texts, available);
    value = static_cast<E>(index);
    return changed;
}

void DisplayCombo(Settings& settings)
{
    const std::vector<native::DisplayInfo> displays = native::Displays();
    auto name = [&](int display) {
        if (display <= 0 || display > static_cast<int>(displays.size()))
            return std::string(Text(TextId::DisplayDefault));
        const native::DisplayInfo& info = displays[static_cast<size_t>(display) - 1];
        const long width = info.area.right - info.area.left;
        const long height = info.area.bottom - info.area.top;
        return Format(info.primary ? TextId::DisplayPrimary : TextId::DisplayItem, display, width, height);
    };
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##display", name(settings.display).c_str())) {
        for (int i = 0; i <= static_cast<int>(displays.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(name(i).c_str(), i == settings.display))
                settings.display = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void DrawGeneralPage(Settings& settings)
{
    widgets::BeginCard("##realtime");
    widgets::Toggle(Text(TextId::RealtimeLighting), &settings.realtimeLighting);
    constexpr TextId kBlocks[] = {TextId::RealtimeLighting, TextId::RealtimeUnsupported, TextId::RealtimeOtherThread};
    const RealtimeBlock block = GetStatistics().realtimeBlock;
    if (settings.realtimeLighting && block != RealtimeBlock::None)
        ImGui::TextWrapped("%s", Text(kBlocks[static_cast<size_t>(block)]));
    widgets::EndCard();

    widgets::BeginCard("##view");
    if (BeginRows("view")) {
        Row(TextId::Display);
        DisplayCombo(settings);
        constexpr TextId kViews[] = {TextId::ViewFinal, TextId::ViewAlbedo, TextId::ViewNormals, TextId::ViewCoverage, TextId::ViewDirectLight,
            TextId::ViewIndirectLight, TextId::ViewIndirectRaw, TextId::ViewSunVisibility, TextId::ViewAdditiveLight, TextId::ViewReflections,
            TextId::ViewMotionVectors, TextId::ViewAmbientOcclusion, TextId::ViewRadianceCache, TextId::ViewLightSample, TextId::ViewReservoirM,
            TextId::ViewDenoiserValidation};
        static_assert(std::size(kViews) == static_cast<size_t>(DebugView::Count));
        ComboRow(TextId::DebugView, settings.debugView, kViews);
        ImGui::EndTable();
    }
    widgets::EndCard();

    if (ImGui::Button(Text(TextId::ResetDefaults)))
        settings = Settings{};
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

    widgets::BeginCard("##credits", Text(TextId::SectionCredits));
    widgets::Note(Text(TextId::CreditsDlss));
    widgets::Note(Text(TextId::CreditsLicenses));
    widgets::EndCard();
}

void DrawLightingPage(Settings& settings)
{
    widgets::BeginCard("##lights", Text(TextId::SectionLights));
    if (BeginRows("lights")) {
        ToggleRow(TextId::Sun, settings.sun, settings, &Settings::sunIntensity);
        ToggleRow(TextId::SkyIntensity, nullptr, settings, &Settings::skyIntensity);
        ToggleRow(TextId::MapLights, settings.mapLights, settings, &Settings::lightIntensity);
        ToggleRow(TextId::DynamicLights, settings.dynamicLights, settings, nullptr);
        ToggleRow(TextId::ProjectedLights, settings.projectedLights, settings, &Settings::projectedIntensity);
        ImGui::EndTable();
    }
    widgets::EndCard();

    widgets::BeginCard("##indirect", Text(TextId::SectionIndirect));
    if (BeginRows("indirect")) {
        ToggleRow(TextId::IndirectLight, settings.indirectLight, settings, &Settings::indirectIntensity);
        ToggleRow(TextId::Denoiser, settings.denoiser, settings, nullptr);
        // DLSS Ray Reconstruction denoises by itself.
        ImGui::BeginDisabled(!settings.denoiser || settings.antiAliasing == AntiAliasing::RayReconstruction);
        constexpr TextId kDenoisers[] = {TextId::DenoiserBuiltIn, TextId::DenoiserRelax, TextId::DenoiserReblur};
        static_assert(std::size(kDenoisers) == static_cast<size_t>(DenoiserType::Count));
        ComboRow(TextId::DenoiserMethod, settings.denoiserType, kDenoisers);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(settings.indirectLighting != IndirectLighting::Baked);
        ToggleRow(TextId::AmbientOcclusion, settings.ambientOcclusion, settings, &Settings::aoIntensity);
        ImGui::EndDisabled();
        ToggleRow(TextId::Reflections, settings.reflections, settings, &Settings::reflectionIntensity);
        ImGui::BeginDisabled(!settings.reflections);
        ToggleRow(TextId::GlossyReflections, settings.glossyReflections, settings, nullptr);
        ImGui::EndDisabled();
        ToggleRow(TextId::TranslucentLighting, settings.translucentLighting, settings, nullptr);
        ToggleRow(TextId::RenderTargetLighting, settings.renderTargetLighting, settings, nullptr);
        ToggleRow(TextId::VolumetricLight, settings.volumetricLight, settings, &Settings::volumetricIntensity);
        ImGui::EndTable();
    }
    widgets::EndCard();
}

void DrawRayTracingPage(Settings& settings)
{
    widgets::BeginCard("##quality");
    if (BeginRows("quality")) {
        constexpr TextId kPresets[] = {TextId::PresetLow, TextId::PresetMedium, TextId::PresetHigh, TextId::PresetUltra, TextId::PresetCustom};
        static_assert(std::size(kPresets) == static_cast<size_t>(QualityPreset::Count));
        QualityPreset preset = settings.qualityPreset;
        if (SegmentRow(TextId::QualityPreset, preset, kPresets))
            ApplyPreset(settings, preset);
        ImGui::EndTable();
    }
    widgets::EndCard();

    widgets::BeginCard("##shadows", Text(TextId::SectionShadows));
    if (BeginRows("shadows")) {
        constexpr TextId kSampling[] = {TextId::LightSamplingLegacy, TextId::LightSamplingRis, TextId::LightSamplingRestir, TextId::LightSamplingAuto};
        static_assert(std::size(kSampling) == static_cast<size_t>(LightSampling::Count));
        if (ComboRow(TextId::LightSampling, settings.lightSampling, kSampling))
            settings.qualityPreset = QualityPreset::Custom;
        // Auto uses these with DLSS Ray Reconstruction (Lighting::Run).
        const bool directPass = settings.lightSampling == LightSampling::Legacy ||
            (settings.lightSampling == LightSampling::Auto && settings.antiAliasing == AntiAliasing::RayReconstruction);
        ImGui::BeginDisabled(!directPass);
        IntSliderRow(TextId::ExactLights, settings, &Settings::exactLights);
        IntSliderRow(TextId::SampledLightRays, settings, &Settings::sampledLightRays);
        ImGui::EndDisabled();
        IntSliderRow(TextId::ViewmodelShadowRays, settings, &Settings::viewmodelShadowRays);
        constexpr TextId kFoliage[] = {TextId::FoliageOff, TextId::FoliageSun, TextId::FoliageSunAndIndirect, TextId::FoliageAll};
        static_assert(std::size(kFoliage) == static_cast<size_t>(FoliageShadows::Count));
        if (ComboRow(TextId::FoliageShadows, settings.foliageShadows, kFoliage))
            settings.qualityPreset = QualityPreset::Custom;
        constexpr TextId kEmissive[] = {TextId::EmissiveOff, TextId::EmissiveBounce, TextId::EmissiveFull};
        static_assert(std::size(kEmissive) == static_cast<size_t>(EmissiveLighting::Count));
        if (SegmentRow(TextId::EmissiveLighting, settings.emissiveLighting, kEmissive))
            settings.qualityPreset = QualityPreset::Custom;
        ImGui::EndTable();
    }
    widgets::EndCard();

    widgets::BeginCard("##indirect", Text(TextId::SectionIndirect));
    if (BeginRows("indirect")) {
        constexpr TextId kSources[] = {TextId::IndirectRayTraced, TextId::IndirectBaked};
        static_assert(std::size(kSources) == static_cast<size_t>(IndirectLighting::Count));
        if (SegmentRow(TextId::IndirectSource, settings.indirectLighting, kSources))
            settings.qualityPreset = QualityPreset::Custom;
        IntSliderRow(TextId::IndirectRays, settings, &Settings::indirectRays);
        IntSliderRow(TextId::IndirectBounces, settings, &Settings::indirectBounces);
        const bool cacheAvailable = GetStatistics().radianceCacheAvailable;
        ImGui::BeginDisabled(!cacheAvailable || settings.indirectLighting == IndirectLighting::Baked);
        if (SwitchRow(TextId::RadianceCache, settings.radianceCache))
            settings.qualityPreset = QualityPreset::Custom;
        ImGui::EndDisabled();
        if (!cacheAvailable) {
            ImGui::AlignTextToFramePadding();
            widgets::Note(Text(TextId::RadianceCacheUnavailable));
        }
        ImGui::BeginDisabled(settings.indirectLighting != IndirectLighting::Baked);
        IntSliderRow(TextId::AoRays, settings, &Settings::aoRays);
        ImGui::EndDisabled();
        IntSliderRow(TextId::VolumeRays, settings, &Settings::volumeRays);
        ImGui::EndTable();
    }
    widgets::EndCard();

    widgets::BeginCard("##effects", Text(TextId::SectionEffects));
    if (BeginRows("effects")) {
        IntSliderRow(TextId::VolumetricSteps, settings, &Settings::volumetricSteps);
        ImGui::EndTable();
    }
    widgets::EndCard();

    const Statistics& statistics = GetStatistics();
    widgets::BeginCard("##alpha", Text(TextId::SectionAlphaTests));
    ImGui::BeginDisabled(!statistics.opacityMicromapsAvailable);
    widgets::Toggle(Text(TextId::OpacityMicromaps), &settings.opacityMicromaps);
    ImGui::BeginDisabled(!settings.opacityMicromaps);
    if (widgets::Toggle(Text(TextId::FastAlphaTests), &settings.fastAlphaTests))
        settings.qualityPreset = QualityPreset::Custom;
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!statistics.opacityMicromapsAvailable)
        widgets::Note(Text(TextId::MicromapsUnavailable));
    else
        widgets::Note(Format(TextId::StatMicromaps, statistics.micromapTriangles, statistics.micromaps).c_str());
    widgets::EndCard();
}

void DrawImagePage(Settings& settings)
{
    const Statistics& statistics = GetStatistics();
    widgets::BeginCard("##antialiasing", Text(TextId::SectionAntiAliasing));
    if (BeginRows("antialiasing")) {
        constexpr TextId kModes[] = {TextId::AntiAliasingOff, TextId::AntiAliasingTemporal, TextId::AntiAliasingDlssShort};
        static_assert(std::size(kModes) == static_cast<size_t>(AntiAliasing::Count));
        const bool available[] = {true, true, statistics.rayReconstructionAvailable};
        SegmentRow(TextId::AntiAliasingMode, settings.antiAliasing, kModes, available);
        if (settings.antiAliasing == AntiAliasing::RayReconstruction) {
            constexpr TextId kUpscaling[] = {TextId::UpscalingNative, TextId::DlssQuality, TextId::DlssBalanced, TextId::DlssPerformance,
                TextId::DlssUltraPerformance};
            static_assert(std::size(kUpscaling) == static_cast<size_t>(UpscalingMode::Count));
            ComboRow(TextId::Upscaling, settings.upscaling, kUpscaling);
        } else {
            SliderRow(TextId::HistoryWeight, settings, &Settings::taaHistoryWeight);
            SliderRow(TextId::Sharpening, settings, &Settings::sharpening);
        }
        ImGui::EndTable();
    }
    if (!statistics.rayReconstructionAvailable)
        widgets::Note(Text(TextId::DlssUnavailable));
    if (statistics.renderWidth != 0)
        widgets::Note(Format(TextId::StatRenderResolution, statistics.renderWidth, statistics.renderHeight).c_str());
    widgets::EndCard();

    widgets::BeginCard("##effects", Text(TextId::SectionEffects));
    widgets::Toggle(Text(TextId::Fog), &settings.fog);
    widgets::EndCard();
}

// A table cell's time, right-aligned.
void Milliseconds(double ms)
{
    const std::string text = Format(TextId::ValueMilliseconds, ms);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text.c_str()).x);
    ImGui::TextUnformatted(text.c_str());
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

    widgets::Heading(Text(TextId::SectionFrameRate));
    const TextId labels[] = {TextId::StatFpsCurrent, TextId::StatFpsAverage, TextId::StatFpsMedian, TextId::StatFpsLow1, TextId::StatFpsLow01};
    const double rates[] = {fps(recentTotal / static_cast<double>(recent)), fps(average), fps(sorted[count / 2]), fps(slowestAverage(0.01)),
        fps(slowestAverage(0.001))};
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = (ImGui::GetContentRegionAvail().x - spacing * static_cast<float>(std::size(labels) - 1)) / static_cast<float>(std::size(labels));
    for (size_t i = 0; i < std::size(labels); ++i) {
        if (i > 0)
            ImGui::SameLine();
        widgets::Tile(Format(TextId::StatFpsValue, rates[i]).c_str(), Text(labels[i]), width);
    }
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

    widgets::BeginCard("##frametime", Text(TextId::StatFrameTimeGraph));
    TextLine(TextId::StatFrameTime, average, sorted[std::min(count - 1, count * 99 / 100)]);
    ImGui::PlotLines("##frametimes", chronological.data(), static_cast<int>(count), 0, nullptr, 0.0f, std::max(sorted.back(), 1.0f),
        ImVec2(-FLT_MIN, ImGui::GetFontSize() * 4.0f));
    widgets::Note(Format(TextId::StatFrameTimeRange, sorted.front(), sorted.back(), count).c_str());
    widgets::EndCard();
}

void DrawPerformancePage()
{
    const Statistics& statistics = GetStatistics();
    DrawFrameRate(statistics);

    widgets::BeginCard("##passes", Text(TextId::SectionGpuPasses));
    constexpr TextId kPasses[] = {TextId::PassSceneUpdate, TextId::PassSurface, TextId::PassRadianceCache, TextId::PassVolume, TextId::PassDirectLight,
        TextId::PassLightSampling, TextId::PassAmbientOcclusion, TextId::PassIndirectTrace, TextId::PassReflections, TextId::PassAccumulation,
        TextId::PassDenoiser, TextId::PassLighting, TextId::PassResolve};
    static_assert(std::size(kPasses) == static_cast<size_t>(GpuPass::Count));
    if (ImGui::BeginTable("passes", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize(Format(TextId::ValueMilliseconds, 100.0).c_str()).x);
        double longest = statistics.otherViewsMs;
        for (double ms : statistics.passMs)
            longest = std::max(longest, ms);
        auto passRow = [&](TextId label, double ms) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Text(label));
            ImGui::TableNextColumn();
            widgets::Meter(longest > 0.0 ? static_cast<float>(ms / longest) : 0.0f);
            ImGui::TableNextColumn();
            Milliseconds(ms);
        };
        for (size_t i = 0; i < std::size(kPasses); ++i)
            passRow(kPasses[i], statistics.passMs[i]);
        passRow(TextId::PassOtherViews, statistics.otherViewsMs);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushFont(widgets::GetFonts().strong);
        ImGui::TextUnformatted(Text(TextId::StatLightingTime));
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        Milliseconds(statistics.lightingMs);
        ImGui::PopFont();
        ImGui::EndTable();
    }
    widgets::EndCard();

    widgets::BeginCard("##statistics", Text(TextId::SectionStatistics));
    TextLine(TextId::StatLights, statistics.mapLights, statistics.dynamicLights, statistics.projectedLights);
    TextLine(TextId::StatInstances, statistics.sceneInstances);
    TextLine(TextId::StatShaders, statistics.gbufferShaders, statistics.replacedShaders);
    widgets::EndCard();
}

// Returns false when the close button was clicked.
bool DrawMenu()
{
    Settings& settings = GetSettings();
    const Settings before = settings;
    const float font = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(font * 1.5f, font * 1.5f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(font * 44.0f, font * 32.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(font * 30.0f, font * 16.0f), ImVec2(FLT_MAX, FLT_MAX));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    bool keepOpen = true;
    if (ImGui::Begin("###gmodpp_menu", nullptr, kFlags)) {
        keepOpen = !widgets::Header(Text(TextId::WindowTitle), Text(TextId::MenuHint));

        constexpr TextId kPages[] = {TextId::TabGeneral, TextId::TabLighting, TextId::TabRayTracing, TextId::TabImage, TextId::TabPerformance};
        static_assert(std::size(kPages) == static_cast<size_t>(Page::Count));
        const ImGuiStyle& style = ImGui::GetStyle();
        ImFont* navFont = widgets::GetFonts().strong;
        float navWidth = 0.0f;
        for (TextId page : kPages)
            navWidth = std::max(navWidth, navFont->CalcTextSizeA(navFont->FontSize, FLT_MAX, 0.0f, Text(page)).x);
        navWidth += style.FramePadding.x * 4.0f;
        if (ImGui::BeginChild("##nav", ImVec2(navWidth, 0.0f))) {
            for (size_t i = 0; i < std::size(kPages); ++i) {
                if (widgets::NavItem(Text(kPages[i]), g_state.page == static_cast<Page>(i)))
                    g_state.page = static_cast<Page>(i);
            }
        }
        ImGui::EndChild();
        const ImVec2 navMin = ImGui::GetItemRectMin();
        const ImVec2 navMax = ImGui::GetItemRectMax();
        const float divider = navMax.x + style.ItemSpacing.x;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(divider, navMin.y), ImVec2(divider, navMax.y), ImGui::GetColorU32(ImGuiCol_Border));
        ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);

        // Each page keeps its own scroll position.
        ImGui::PushID(static_cast<int>(g_state.page));
        if (ImGui::BeginChild("##page")) {
            switch (g_state.page) {
            case Page::General: DrawGeneralPage(settings); break;
            case Page::Lighting: DrawLightingPage(settings); break;
            case Page::RayTracing: DrawRayTracingPage(settings); break;
            case Page::Image: DrawImagePage(settings); break;
            case Page::Performance: DrawPerformancePage(); break;
            default: break;
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
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
