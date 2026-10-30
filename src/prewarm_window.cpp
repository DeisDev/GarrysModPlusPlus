#include "shader_prewarm.h"

#include "log.h"
#include "ui_text.h"

#include <windows.h>
#include <commctrl.h>

#include <chrono>
#include <deque>
#include <format>
#include <locale>
#include <string>
#include <string_view>
#include <thread>

// The controls take the current visual style only from Common Controls 6 (Microsoft Learn, "Enabling Visual Styles"): the
// linker puts this dependency in the DLL's manifest, resource 2, which the window activates while it creates them.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace gmodpp {

namespace {

constexpr wchar_t kClassName[] = L"GarrysModPlusPlusShaderPrewarm";
constexpr UINT_PTR kTimer = 1;
constexpr UINT kTimerMilliseconds = 100;
constexpr DWORD kStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr int kSkipId = 1;
// The time left comes from the rate over the last 20 s: shaders cached by an earlier run go by much faster.
constexpr auto kRateWindow = std::chrono::seconds(20);
constexpr auto kRateMinimum = std::chrono::seconds(5);

struct Sample {
    std::chrono::steady_clock::time_point time;
    size_t done = 0;
};

struct WindowState {
    PrewarmProgress* progress = nullptr;
    HWND bar = nullptr;
    HWND count = nullptr;
    HWND skip = nullptr;
    std::deque<Sample> samples;
    std::wstring countText;
};

std::wstring Wide(std::string_view text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string CountText(WindowState& state, size_t done, size_t total)
{
    const auto now = std::chrono::steady_clock::now();
    state.samples.push_back({ now, done });
    while (state.samples.size() > 1 && now - state.samples.front().time > kRateWindow)
        state.samples.pop_front();
    const Sample& oldest = state.samples.front();
    const std::chrono::duration<double> elapsed = now - oldest.time;
    if (elapsed < kRateMinimum || done <= oldest.done)
        return std::vformat(std::locale(""), ui::Text(ui::TextId::PrewarmProgress), std::make_format_args(done, total));
    const double rate = static_cast<double>(done - oldest.done) / elapsed.count();
    const size_t secondsLeft = static_cast<size_t>(static_cast<double>(total - done) / rate) + 1;
    const size_t minutes = secondsLeft / 60;
    const size_t seconds = secondsLeft % 60;
    return std::vformat(std::locale(""), ui::Text(ui::TextId::PrewarmTimeLeft), std::make_format_args(done, total, minutes, seconds));
}

void Update(WindowState& state)
{
    const size_t total = state.progress->total;
    std::wstring text = Wide(ui::Text(ui::TextId::PrewarmStarting));
    if (total != 0) {
        const size_t done = state.progress->done;
        SendMessageW(state.bar, PBM_SETRANGE32, 0, static_cast<LPARAM>(total));
        SendMessageW(state.bar, PBM_SETPOS, static_cast<WPARAM>(done), 0);
        text = Wide(CountText(state, done, total));
    }
    if (text != state.countText) {
        state.countText = std::move(text);
        SetWindowTextW(state.count, state.countText.c_str());
    }
}

void Skip(const WindowState& state)
{
    // The window goes when the workers have stopped.
    state.progress->cancel = true;
    EnableWindow(state.skip, FALSE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CLOSE:
        if (state != nullptr)
            Skip(*state);
        return 0;
    case WM_COMMAND:
        if (state != nullptr && LOWORD(wParam) == kSkipId && HIWORD(wParam) == BN_CLICKED)
            Skip(*state);
        return 0;
    case WM_TIMER:
        if (state != nullptr)
            Update(*state);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

HINSTANCE ThisModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&WindowProc),
        &module);
    return module;
}

}

void PrewarmWithProgressWindow(const std::filesystem::path& gameDirectory, const std::filesystem::path& compilerDirectory)
{
    PrewarmProgress progress;
    std::atomic<bool> finished = false;
    std::jthread worker([&] {
        PrewarmShaders(gameDirectory, compilerDirectory, progress);
        finished = true;
    });

    const HINSTANCE instance = ThisModule();
    ACTCTXW contextInfo = { sizeof(contextInfo) };
    contextInfo.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    contextInfo.hModule = instance;
    contextInfo.lpResourceName = ISOLATIONAWARE_MANIFEST_RESOURCE_ID;
    const HANDLE context = CreateActCtxW(&contextInfo);
    ULONG_PTR contextCookie = 0;
    if (context == INVALID_HANDLE_VALUE || !ActivateActCtx(context, &contextCookie))
        log::Info("Shader prewarm: the window's visual style is unavailable (error {})", GetLastError());

    const INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);
    WNDCLASSEXW windowClass = { sizeof(windowClass) };
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    const UINT dpi = GetDpiForSystem();
    auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int margin = scale(16);
    const int clientWidth = scale(460);
    const int clientHeight = scale(150);
    RECT frame = { 0, 0, clientWidth, clientHeight };
    AdjustWindowRectExForDpi(&frame, kStyle, FALSE, 0, dpi);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const HWND window = CreateWindowExW(0, kClassName, Wide(ui::Text(ui::TextId::WindowTitle)).c_str(), kStyle, work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2, width, height, nullptr, nullptr, instance, nullptr);

    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    const HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
    const int textWidth = clientWidth - 2 * margin;
    const int buttonWidth = scale(88);
    const int buttonHeight = scale(26);
    const int rowTop = margin + scale(92);
    const HWND message = CreateWindowExW(0, L"STATIC", Wide(ui::Text(ui::TextId::PrewarmMessage)).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, margin, margin,
        textWidth, scale(52), window, nullptr, instance, nullptr);
    WindowState state = { &progress };
    state.bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, margin, margin + scale(62), textWidth, scale(18), window, nullptr, instance,
        nullptr);
    state.count = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, margin, rowTop, textWidth - buttonWidth - margin,
        buttonHeight, window, nullptr, instance, nullptr);
    state.skip = CreateWindowExW(0, L"BUTTON", Wide(ui::Text(ui::TextId::PrewarmSkip)).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        clientWidth - margin - buttonWidth, rowTop, buttonWidth, buttonHeight, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSkipId)), instance,
        nullptr);
    for (HWND control : { message, state.count, state.skip })
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (contextCookie != 0)
        DeactivateActCtx(0, contextCookie);
    if (context != INVALID_HANDLE_VALUE)
        ReleaseActCtx(context);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    Update(state);
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
    SetFocus(state.skip);
    SetTimer(window, kTimer, kTimerMilliseconds, nullptr);

    // Only this window's messages are taken: the game has no window yet, and its thread must not run other code here.
    while (!finished) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, kTimerMilliseconds, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, window, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    worker.join();
    KillTimer(window, kTimer);
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    DestroyWindow(window);
    DeleteObject(font);
    UnregisterClassW(kClassName, instance);
}

}
