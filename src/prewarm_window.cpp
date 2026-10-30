#include "shader_prewarm.h"

#include "ui_text.h"

#include <windows.h>
#include <commctrl.h>

#include <format>
#include <locale>
#include <string>
#include <string_view>
#include <thread>

namespace gmodpp {

namespace {

constexpr wchar_t kClassName[] = L"GarrysModPlusPlusShaderPrewarm";
constexpr UINT_PTR kTimer = 1;
constexpr UINT kTimerMilliseconds = 100;
constexpr DWORD kStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

struct WindowState {
    PrewarmProgress* progress = nullptr;
    HWND bar = nullptr;
    HWND count = nullptr;
};

std::wstring Wide(std::string_view text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

void Update(const WindowState& state)
{
    const size_t total = state.progress->total;
    if (total == 0)
        return;
    const size_t done = state.progress->done;
    SendMessageW(state.bar, PBM_SETRANGE32, 0, static_cast<LPARAM>(total));
    SendMessageW(state.bar, PBM_SETPOS, static_cast<WPARAM>(done), 0);
    const std::string text = std::vformat(std::locale(""), ui::Text(ui::TextId::PrewarmProgress), std::make_format_args(done, total));
    SetWindowTextW(state.count, Wide(text).c_str());
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CLOSE:
        // The window goes when the workers have stopped.
        if (state != nullptr)
            state->progress->cancel = true;
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

    const INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&controls);
    const HINSTANCE instance = ThisModule();
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
    const int clientHeight = scale(140);
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
    const HWND message = CreateWindowExW(0, L"STATIC", Wide(ui::Text(ui::TextId::PrewarmMessage)).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, margin, margin,
        textWidth, scale(52), window, nullptr, instance, nullptr);
    WindowState state = { &progress };
    state.bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, margin, margin + scale(60), textWidth, scale(18), window, nullptr, instance,
        nullptr);
    state.count = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, margin, margin + scale(86), textWidth, scale(22), window, nullptr, instance,
        nullptr);
    for (HWND control : { message, state.count })
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
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
