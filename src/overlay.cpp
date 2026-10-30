#include "overlay.h"

#include "log.h"
#include "options.h"
#include "settings.h"
#include "ui_text.h"

#include <MinHook.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <intrin.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gmodpp::overlay {

namespace {

using ui::Text;
using ui::TextId;

constexpr UINT kToggleKey = VK_INSERT;

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
void TextLine(TextId id, Args&&... args)
{
    const std::string line = std::vformat(Text(id), std::make_format_args(args...));
    ImGui::TextUnformatted(line.c_str());
}

bool BeginOptionTable(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp))
        return false;
    ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// One table row: an optional on/off box (a plain label without one) and an optional intensity slider.
void ToggleRow(TextId label, bool* enabled, Settings& settings, float Settings::* intensity)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(static_cast<int>(label));
    if (enabled != nullptr)
        ImGui::Checkbox(Text(label), enabled);
    else
        ImGui::TextUnformatted(Text(label));
    ImGui::TableNextColumn();
    if (intensity != nullptr) {
        const SettingRange range = RangeOf(intensity);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##intensity", &(settings.*intensity), range.minimum, range.maximum, "%.2f");
    }
    ImGui::PopID();
}

void ToggleRow(TextId label, bool& enabled, Settings& settings, float Settings::* intensity) { ToggleRow(label, &enabled, settings, intensity); }

void SliderRow(TextId label, Settings& settings, float Settings::* field)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(Text(label));
    ImGui::TableNextColumn();
    ImGui::PushID(static_cast<int>(label));
    const SettingRange range = RangeOf(field);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##value", &(settings.*field), range.minimum, range.maximum, "%.2f");
    ImGui::PopID();
}

// Frame rate figures over the recorded history. Lows average the slowest 1% / 0.1% of frames.
void DrawPerformance(const Statistics& statistics)
{
    ImGui::SeparatorText(Text(TextId::SectionPerformance));
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
    TextLine(TextId::StatFpsCurrent, fps(recentTotal / static_cast<double>(recent)));
    TextLine(TextId::StatFpsAverage, fps(average));
    TextLine(TextId::StatFpsMedian, fps(sorted[count / 2]));
    TextLine(TextId::StatFpsLow1, fps(slowestAverage(0.01)));
    TextLine(TextId::StatFpsLow01, fps(slowestAverage(0.001)));
    TextLine(TextId::StatFrameTime, average, sorted[std::min(count - 1, count * 99 / 100)]);
    TextLine(TextId::StatFrameTimeRange, sorted.front(), sorted.back(), count);
    ImGui::PlotLines("##frametimes", chronological.data(), static_cast<int>(count), 0, Text(TextId::StatFrameTimeGraph), 0.0f,
        std::max(sorted.back(), 1.0f), ImVec2(-FLT_MIN, ImGui::GetFontSize() * 4.0f));

    ImGui::SeparatorText(Text(TextId::SectionGpuPasses));
    TextLine(TextId::StatLightingTime, statistics.lightingMs);
    const TextId passes[] = {TextId::PassSceneUpdate, TextId::PassSurface, TextId::PassVolume, TextId::PassDirectLight, TextId::PassAmbientOcclusion,
        TextId::PassIndirectTrace, TextId::PassAccumulation, TextId::PassDenoiser, TextId::PassReflections, TextId::PassLighting, TextId::PassResolve};
    static_assert(std::size(passes) == static_cast<size_t>(GpuPass::Count));
    for (size_t i = 0; i < std::size(passes); ++i)
        TextLine(passes[i], statistics.passMs[i]);
    TextLine(TextId::PassOtherViews, statistics.otherViewsMs);
}

void DrawGeneralTab(Settings& settings)
{
    ImGui::Checkbox(Text(TextId::RealtimeLighting), &settings.realtimeLighting);
    constexpr TextId kBlocks[] = {TextId::RealtimeLighting, TextId::RealtimeUnsupported, TextId::RealtimeMultisampling, TextId::RealtimeOtherThread};
    const RealtimeBlock block = GetStatistics().realtimeBlock;
    if (settings.realtimeLighting && block != RealtimeBlock::None)
        ImGui::TextWrapped("%s", Text(kBlocks[static_cast<size_t>(block)]));
    const TextId views[] = {TextId::ViewFinal, TextId::ViewAlbedo, TextId::ViewNormals, TextId::ViewCoverage, TextId::ViewDirectLight,
        TextId::ViewIndirectLight, TextId::ViewIndirectRaw, TextId::ViewSunVisibility, TextId::ViewAdditiveLight,
        TextId::ViewReflections, TextId::ViewMotionVectors, TextId::ViewAmbientOcclusion};
    static_assert(std::size(views) == static_cast<size_t>(DebugView::Count));
    int view = static_cast<int>(settings.debugView);
    if (ImGui::BeginCombo(Text(TextId::DebugView), Text(views[view]))) {
        for (int i = 0; i < static_cast<int>(std::size(views)); ++i) {
            if (ImGui::Selectable(Text(views[i]), i == view))
                settings.debugView = static_cast<DebugView>(i);
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button(Text(TextId::ResetDefaults)))
        settings = Settings{};
}

void DrawLightingTab(Settings& settings)
{
    ImGui::SeparatorText(Text(TextId::SectionLights));
    if (BeginOptionTable("lights")) {
        ToggleRow(TextId::Sun, settings.sun, settings, &Settings::sunIntensity);
        ToggleRow(TextId::SkyIntensity, nullptr, settings, &Settings::skyIntensity);
        ToggleRow(TextId::MapLights, settings.mapLights, settings, &Settings::lightIntensity);
        ToggleRow(TextId::DynamicLights, settings.dynamicLights, settings, nullptr);
        ToggleRow(TextId::ProjectedLights, settings.projectedLights, settings, &Settings::projectedIntensity);
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Text(TextId::SectionIndirect));
    if (BeginOptionTable("indirect")) {
        ToggleRow(TextId::IndirectLight, settings.indirectLight, settings, &Settings::indirectIntensity);
        ToggleRow(TextId::Denoiser, settings.denoiser, settings, nullptr);
        ToggleRow(TextId::AmbientOcclusion, settings.ambientOcclusion, settings, &Settings::aoIntensity);
        ToggleRow(TextId::Reflections, settings.reflections, settings, &Settings::reflectionIntensity);
        ToggleRow(TextId::TranslucentLighting, settings.translucentLighting, settings, nullptr);
        ToggleRow(TextId::RenderTargetLighting, settings.renderTargetLighting, settings, nullptr);
        ToggleRow(TextId::VolumetricLight, settings.volumetricLight, settings, &Settings::volumetricIntensity);
        ImGui::EndTable();
    }
}

// A table row with a whole-number slider; changing it leaves the quality preset.
void IntSliderRow(TextId label, Settings& settings, int Settings::* field)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(Text(label));
    ImGui::TableNextColumn();
    ImGui::PushID(static_cast<int>(label));
    const SettingRange range = RangeOf(field);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##value", &(settings.*field), static_cast<int>(range.minimum), static_cast<int>(range.maximum)))
        settings.qualityPreset = QualityPreset::Custom;
    ImGui::PopID();
}

void DrawRayTracingTab(Settings& settings)
{
    const TextId presets[] = {TextId::PresetLow, TextId::PresetMedium, TextId::PresetHigh, TextId::PresetUltra, TextId::PresetCustom};
    static_assert(std::size(presets) == static_cast<size_t>(QualityPreset::Count));
    const int preset = static_cast<int>(settings.qualityPreset);
    if (ImGui::BeginCombo(Text(TextId::QualityPreset), Text(presets[preset]))) {
        for (int i = 0; i < static_cast<int>(std::size(presets)); ++i) {
            if (ImGui::Selectable(Text(presets[i]), i == preset))
                ApplyPreset(settings, static_cast<QualityPreset>(i));
        }
        ImGui::EndCombo();
    }
    ImGui::SeparatorText(Text(TextId::SectionShadows));
    if (BeginOptionTable("shadows")) {
        IntSliderRow(TextId::ExactLights, settings, &Settings::exactLights);
        IntSliderRow(TextId::SampledLightRays, settings, &Settings::sampledLightRays);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(Text(TextId::FoliageShadows));
        ImGui::TableNextColumn();
        const TextId foliage[] = {TextId::FoliageOff, TextId::FoliageSun, TextId::FoliageSunAndIndirect};
        static_assert(std::size(foliage) == static_cast<size_t>(FoliageShadows::Count));
        const int mode = static_cast<int>(settings.foliageShadows);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##foliage", Text(foliage[mode]))) {
            for (int i = 0; i < static_cast<int>(std::size(foliage)); ++i) {
                if (ImGui::Selectable(Text(foliage[i]), i == mode) && i != mode) {
                    settings.foliageShadows = static_cast<FoliageShadows>(i);
                    settings.qualityPreset = QualityPreset::Custom;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Text(TextId::SectionIndirect));
    if (BeginOptionTable("indirect")) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(Text(TextId::IndirectSource));
        ImGui::TableNextColumn();
        const TextId sources[] = {TextId::IndirectRayTraced, TextId::IndirectBaked};
        static_assert(std::size(sources) == static_cast<size_t>(IndirectLighting::Count));
        const int source = static_cast<int>(settings.indirectLighting);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##indirectSource", Text(sources[source]))) {
            for (int i = 0; i < static_cast<int>(std::size(sources)); ++i) {
                if (ImGui::Selectable(Text(sources[i]), i == source) && i != source) {
                    settings.indirectLighting = static_cast<IndirectLighting>(i);
                    settings.qualityPreset = QualityPreset::Custom;
                }
            }
            ImGui::EndCombo();
        }
        IntSliderRow(TextId::IndirectRays, settings, &Settings::indirectRays);
        IntSliderRow(TextId::IndirectBounces, settings, &Settings::indirectBounces);
        IntSliderRow(TextId::AoRays, settings, &Settings::aoRays);
        IntSliderRow(TextId::VolumeRays, settings, &Settings::volumeRays);
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Text(TextId::SectionEffects));
    if (BeginOptionTable("effects")) {
        IntSliderRow(TextId::VolumetricSteps, settings, &Settings::volumetricSteps);
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Text(TextId::SectionAlphaTests));
    const Statistics& statistics = GetStatistics();
    ImGui::BeginDisabled(!statistics.opacityMicromapsAvailable);
    ImGui::Checkbox(Text(TextId::OpacityMicromaps), &settings.opacityMicromaps);
    ImGui::BeginDisabled(!settings.opacityMicromaps);
    if (ImGui::Checkbox(Text(TextId::FastAlphaTests), &settings.fastAlphaTests))
        settings.qualityPreset = QualityPreset::Custom;
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!statistics.opacityMicromapsAvailable)
        ImGui::TextDisabled("%s", Text(TextId::MicromapsUnavailable));
    else
        TextLine(TextId::StatMicromaps, statistics.micromapTriangles, statistics.micromaps);
}

void DrawImageTab(Settings& settings)
{
    ImGui::SeparatorText(Text(TextId::SectionAntiAliasing));
    if (BeginOptionTable("antialiasing")) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(Text(TextId::AntiAliasingMode));
        ImGui::TableNextColumn();
        const Statistics& statistics = GetStatistics();
        int aa = static_cast<int>(settings.antiAliasing);
        ImGui::RadioButton(Text(TextId::AntiAliasingOff), &aa, static_cast<int>(AntiAliasing::Off));
        ImGui::SameLine();
        ImGui::RadioButton(Text(TextId::AntiAliasingTemporal), &aa, static_cast<int>(AntiAliasing::Temporal));
        ImGui::SameLine();
        ImGui::BeginDisabled(!statistics.rayReconstructionAvailable);
        ImGui::RadioButton(Text(TextId::AntiAliasingDlss), &aa, static_cast<int>(AntiAliasing::RayReconstruction));
        ImGui::EndDisabled();
        settings.antiAliasing = static_cast<AntiAliasing>(aa);
        if (settings.antiAliasing == AntiAliasing::RayReconstruction) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Text(TextId::DlssMode));
            ImGui::TableNextColumn();
            const TextId modes[] = {TextId::DlssDlaa, TextId::DlssQuality, TextId::DlssBalanced, TextId::DlssPerformance, TextId::DlssUltraPerformance};
            static_assert(std::size(modes) == static_cast<size_t>(DlssMode::Count));
            int mode = static_cast<int>(settings.dlssMode);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##dlssmode", Text(modes[mode]))) {
                for (int i = 0; i < static_cast<int>(std::size(modes)); ++i) {
                    if (ImGui::Selectable(Text(modes[i]), i == mode))
                        settings.dlssMode = static_cast<DlssMode>(i);
                }
                ImGui::EndCombo();
            }
        } else {
            SliderRow(TextId::HistoryWeight, settings, &Settings::taaHistoryWeight);
            SliderRow(TextId::Sharpening, settings, &Settings::sharpening);
        }
        ImGui::EndTable();
    }
    const Statistics& statistics = GetStatistics();
    if (!statistics.rayReconstructionAvailable)
        ImGui::TextDisabled("%s", Text(TextId::DlssUnavailable));
    if (statistics.renderWidth != 0)
        TextLine(TextId::StatRenderResolution, statistics.renderWidth, statistics.renderHeight);
    ImGui::SeparatorText(Text(TextId::SectionEffects));
    ImGui::Checkbox(Text(TextId::Fog), &settings.fog);
}

void DrawPerformanceTab()
{
    const Statistics& statistics = GetStatistics();
    DrawPerformance(statistics);
    ImGui::SeparatorText(Text(TextId::SectionStatistics));
    TextLine(TextId::StatLights, statistics.mapLights, statistics.dynamicLights, statistics.projectedLights);
    TextLine(TextId::StatInstances, statistics.sceneInstances);
    TextLine(TextId::StatShaders, statistics.gbufferShaders, statistics.replacedShaders);
}

// A tab whose contents scroll when the window is too small for them.
template <class Draw>
void Tab(TextId label, Draw draw)
{
    if (!ImGui::BeginTabItem(Text(label)))
        return;
    if (ImGui::BeginChild("##content"))
        draw();
    ImGui::EndChild();
    ImGui::EndTabItem();
}

void DrawMenu()
{
    Settings& settings = GetSettings();
    const Settings before = settings;
    const float font = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(font * 30.0f, font * 34.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(font * 18.0f, font * 12.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin(Text(TextId::WindowTitle))) {
        ImGui::TextDisabled("%s", Text(TextId::MenuHint));
        if (ImGui::BeginTabBar("##tabs")) {
            Tab(TextId::TabGeneral, [&] { DrawGeneralTab(settings); });
            Tab(TextId::TabLighting, [&] { DrawLightingTab(settings); });
            Tab(TextId::TabRayTracing, [&] { DrawRayTracingTab(settings); });
            Tab(TextId::TabImage, [&] { DrawImageTab(settings); });
            Tab(TextId::TabPerformance, [] { DrawPerformanceTab(); });
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    if (before != settings)
        MarkSettingsChanged();
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
    static const std::string iniPath = (GetOptions().gameDirectory / L"gmodpp_menu.ini").string();
    io.IniFilename = iniPath.c_str();
    io.MouseDrawCursor = true;
    ImGui::StyleColorsDark();

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
    DrawMenu();
    ImGui::Render();
    ID3D12DescriptorHeap* heaps[] = {g_state.descriptors->Heap()};
    list->SetDescriptorHeaps(1, heaps);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
}

}
