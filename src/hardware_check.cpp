#include "hardware_check.h"

#include "command_line.h"
#include "d3d12_device.h"
#include "log.h"
#include "settings.h"
#include "shaderapi_device.h"
#include "ui_text.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp::hardware_check {

namespace {

enum class Support { Full, NoRayTracing, None };

struct Result {
    Support support = Support::Full;
    std::wstring adapter;
    // Localization keys of the features the adapter lacks.
    std::vector<const char*> missing;
};

enum Button { kPlayVanilla = 100, kQuit };

std::string Utf8(std::wstring_view text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(std::max(length, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring Wide(std::string_view text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

// The native renderer needs a feature level 12_0 device with Shader Model 6.6 and ResourceDescriptorHeap, which
// resource binding tier 3 guarantees (HLSL SM 6.6 Dynamic Resources spec); the lighting also needs what
// RaytracingSupport::inlineRaytracing checks.
Result Evaluate(IDXGIAdapter1* adapter)
{
    Result result;
    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    result.adapter = desc.Description;
    // Through the runtime the backend uses; the device is released before the backend makes its own.
    const ComPtr<ID3D12DeviceFactory> agility = AgilityDeviceFactory(ModDirectory() / L"D3D12");
    ComPtr<ID3D12Device> device;
    const HRESULT hr = agility ? agility->CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))
                               : D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        log::Info("Hardware check: no feature level 12_0 Direct3D 12 device ({:#010x})", static_cast<unsigned>(hr));
        result.support = Support::None;
        result.missing.push_back("hardware.feature_d3d12");
        return result;
    }
    const RaytracingSupport support = QueryRaytracing(device.Get());
    log::Info("Hardware check: shader model {:#x}, resource binding tier 3 {}, raytracing tier {:#x}, typed UAV loads {}",
        static_cast<int>(support.shaderModel), support.bindingTier3, static_cast<int>(support.tier), support.typedUavLoads);
    if (support.shaderModel < D3D_SHADER_MODEL_6_6)
        result.missing.push_back("hardware.feature_shader_model");
    if (!support.bindingTier3)
        result.missing.push_back("hardware.feature_binding_tier");
    if (!result.missing.empty()) {
        result.support = Support::None;
        return result;
    }
    if (support.tier < D3D12_RAYTRACING_TIER_1_1)
        result.missing.push_back("hardware.feature_ray_tracing");
    if (!support.typedUavLoads)
        result.missing.push_back("hardware.feature_typed_uav");
    if (!result.missing.empty())
        result.support = Support::NoRayTracing;
    return result;
}

// The engine draws on display adapter -adapter, 0 without it: engine.dll 0x180101200 passes
// CommandLine()->ParmValue("-adapter", 0) to IMaterialSystem::SetAdapter (REA 2026-11-03).
int EngineAdapter()
{
    const std::wstring value = ArgumentValue(L"-adapter");
    return value.empty() ? 0 : _wtoi(value.c_str());
}

// The Windows task dialog, from Common Controls 6, which gmod_win64.exe's manifest asks for. Returns the button
// chosen, or 0 when the dialog could not be shown (logged).
int Warn(const Result& result, bool& dontShowAgain)
{
    using TaskDialogFunction = decltype(&TaskDialogIndirect);
    HMODULE commonControls = LoadLibraryW(L"comctl32.dll");
    const auto taskDialog = commonControls != nullptr ? reinterpret_cast<TaskDialogFunction>(GetProcAddress(commonControls, "TaskDialogIndirect")) : nullptr;
    if (taskDialog == nullptr) {
        log::Info("Hardware check: the Windows task dialog is unavailable ({})", GetLastError());
        return 0;
    }
    const bool unsupported = result.support == Support::None;
    std::string missing;
    for (const char* key : result.missing) {
        if (!missing.empty())
            missing += '\n';
        missing += ui::Format("hardware.missing_item", {{"feature", ui::Text(key)}});
    }
    const std::wstring title = Wide(ui::Text("hardware.title"));
    const std::wstring heading = Wide(ui::Text(unsupported ? "hardware.unsupported" : "hardware.no_ray_tracing"));
    const std::wstring body = Wide(ui::Format(unsupported ? "hardware.unsupported_body" : "hardware.no_ray_tracing_body",
        {{"gpu", Utf8(result.adapter)}, {"missing", missing}}));
    const std::wstring dontShow = Wide(ui::Text("hardware.dont_show"));
    const std::wstring play = Wide(ui::Text("hardware.play_vanilla"));
    const std::wstring quit = Wide(ui::Text("hardware.quit"));
    const TASKDIALOG_BUTTON buttons[] = {{kPlayVanilla, play.c_str()}, {kQuit, quit.c_str()}};

    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    config.dwFlags = ui::RightToLeft() ? TDF_RTL_LAYOUT : 0;
    config.pszWindowTitle = title.c_str();
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = heading.c_str();
    config.pszContent = body.c_str();
    config.pszVerificationText = dontShow.c_str();
    if (unsupported) {
        // One of the two choices must be made: the dialog has no close button.
        config.pButtons = buttons;
        config.cButtons = static_cast<UINT>(std::size(buttons));
        config.nDefaultButton = kPlayVanilla;
    } else {
        config.dwCommonButtons = TDCBF_OK_BUTTON;
        config.dwFlags |= TDF_ALLOW_DIALOG_CANCELLATION;
    }
    int button = 0;
    BOOL checked = FALSE;
    const HRESULT hr = taskDialog(&config, &button, nullptr, &checked);
    if (FAILED(hr)) {
        log::Info("Hardware check: the warning could not be shown ({:#010x})", static_cast<unsigned>(hr));
        return 0;
    }
    dontShowAgain = checked != FALSE;
    return button;
}

void StopWarning()
{
    const Settings before = config::View(config::Scope::Global);
    Settings after = before;
    after.hardwareWarning = false;
    config::Commit(config::Scope::Global, before, after);
    config::Sync({});
    log::Info("Hardware check: the warning is off from now on (hardware_warning=0 in settings.ini)");
}

}

bool NativeRendererChosen()
{
    const int index = EngineAdapter();
    const ComPtr<IDXGIAdapter1> adapter = native::AdapterOfDisplay(index);
    if (adapter == nullptr) {
        log::Info("Hardware check: there is no display adapter {}; not checked", index);
        return true;
    }
    const Result result = Evaluate(adapter.Get());
    const bool unsupported = result.support == Support::None;
    if (result.support == Support::Full) {
        log::Info("Hardware check: '{}' runs everything", Utf8(result.adapter));
        return true;
    }
    log::Info("Hardware check: '{}' {}", Utf8(result.adapter),
        unsupported ? "cannot run the native renderer" : "cannot run the ray-traced lighting; the game's baked lighting is used");
    if (!GetSettings().hardwareWarning) {
        if (unsupported)
            log::Info("Hardware check: starting with the game's own renderer without asking (hardware_warning=0)");
        return !unsupported;
    }
    bool dontShowAgain = false;
    const int button = Warn(result, dontShowAgain);
    // Unanswered, the game starts as it can: with its own renderer when the native one cannot run.
    if (button == kQuit) {
        log::Info("Hardware check: the player chose to quit");
        ExitProcess(0);
    }
    if (dontShowAgain)
        StopWarning();
    if (unsupported)
        log::Info("Hardware check: starting with the game's own renderer");
    return !unsupported;
}

}
