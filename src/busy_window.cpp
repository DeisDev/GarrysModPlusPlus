#include "busy_window.h"

#include "command_line.h"
#include "log.h"
#include "ui_text.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace gmodpp::busy {

namespace {

constexpr wchar_t kClassName[] = L"GarrysModPlusPlusBusy";
// Shown once the render thread has spent this long waiting since the last frame, and no frame came for as long.
constexpr long long kShowAfterMs = 1000;
constexpr DWORD kPollMs = 100;
constexpr DWORD kStyle = WS_POPUP | WS_CAPTION;
// Over the game, which may cover the screen, without taking its focus or a taskbar button.
constexpr DWORD kExStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() + 1;
}

// 0 before the first frame, and outside a scope.
std::atomic<long long> g_lastPresent = 0;
std::atomic<long long> g_scopeStart = 0;
// Time spent in scopes that ended since the last frame.
std::atomic<long long> g_busyClosed = 0;
std::atomic<int> g_reason = 0;
std::atomic<long long> g_presents = 0;
thread_local int t_depth = 0;

// The window's text, by Reason, read on the render thread (ui::Text is not thread-safe) for the window's thread, and
// the language and frame it was last checked in.
std::mutex g_textMutex;
std::wstring g_title;
std::array<std::wstring, 2> g_text;
std::string g_textLanguage;
long long g_textChecked = -1;

std::wstring Wide(std::string_view text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

// Follows GMod's language, checked at most once a frame: finding its cvar is a search, and a scope comes for every
// shader a frame reads or compiles.
void LoadText()
{
    std::scoped_lock lock(g_textMutex);
    const long long presents = g_presents;
    if (presents == g_textChecked)
        return;
    g_textChecked = presents;
    ui::UpdateLanguage();
    if (ui::Language() == g_textLanguage)
        return;
    g_textLanguage = ui::Language();
    g_title = Wide(ui::Text("busy.title"));
    g_text[static_cast<size_t>(Reason::Shaders)] = Wide(ui::Text("busy.shaders"));
    g_text[static_cast<size_t>(Reason::Pipelines)] = Wide(ui::Text("busy.pipelines"));
}

HINSTANCE ThisModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ThisModule),
        &module);
    return module;
}

// The window and its message label, created on the window's thread.
class Window {
public:
    void Show(Reason reason)
    {
        std::wstring title;
        std::wstring text;
        {
            std::scoped_lock lock(g_textMutex);
            title = g_title;
            text = g_text[static_cast<size_t>(reason)];
        }
        if (m_window == nullptr && !Create(title))
            return;
        if (title != m_title) {
            m_title = title;
            SetWindowTextW(m_window, m_title.c_str());
        }
        if (text != m_text) {
            m_text = text;
            SetWindowTextW(m_label, m_text.c_str());
        }
        if (!m_visible) {
            Center();
            ShowWindow(m_window, SW_SHOWNOACTIVATE);
            UpdateWindow(m_window);
            m_visible = true;
            log::Info("Busy window: shown ({})", reason == Reason::Shaders ? "compiling shaders" : "preparing ray tracing");
        }
    }

    void Hide()
    {
        if (!m_visible)
            return;
        ShowWindow(m_window, SW_HIDE);
        m_visible = false;
        log::Info("Busy window: hidden");
    }

    // Handles this thread's messages: the window's and its label's (the game's windows belong to other threads).
    void Pump()
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

private:
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        // It closes by itself when the game goes on.
        if (message == WM_CLOSE)
            return 0;
        if (message == WM_CTLCOLORSTATIC) {
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool Create(const std::wstring& title)
    {
        const HINSTANCE instance = ThisModule();
        WNDCLASSEXW windowClass = { sizeof(windowClass) };
        windowClass.lpfnWndProc = &Procedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        windowClass.lpszClassName = kClassName;
        RegisterClassExW(&windowClass);
        m_dpi = GetDpiForSystem();
        const int margin = Scale(16);
        RECT frame = { 0, 0, Scale(420), Scale(84) };
        AdjustWindowRectExForDpi(&frame, kStyle, FALSE, kExStyle, m_dpi);
        m_window = CreateWindowExW(kExStyle, kClassName, title.c_str(), kStyle, 0, 0, frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr,
            instance, nullptr);
        if (m_window == nullptr) {
            log::Info("Busy window: creating it failed ({})", GetLastError());
            return false;
        }
        NONCLIENTMETRICSW metrics = { sizeof(metrics) };
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, m_dpi);
        m_font = CreateFontIndirectW(&metrics.lfMessageFont);
        m_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, margin, margin, Scale(420) - 2 * margin, Scale(84) - 2 * margin,
            m_window, nullptr, instance, nullptr);
        SendMessageW(m_label, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        return true;
    }

    // In the middle of the monitor the game's window is on (the one under the cursor before it has one).
    void Center()
    {
        const HWND game = GetForegroundWindow();
        DWORD process = 0;
        GetWindowThreadProcessId(game, &process);
        POINT cursor = {};
        GetCursorPos(&cursor);
        const HMONITOR monitor =
            game != nullptr && process == GetCurrentProcessId() ? MonitorFromWindow(game, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info = { sizeof(info) };
        GetMonitorInfoW(monitor, &info);
        RECT window = {};
        GetWindowRect(m_window, &window);
        const int width = window.right - window.left;
        const int height = window.bottom - window.top;
        SetWindowPos(m_window, HWND_TOPMOST, info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2,
            info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

    int Scale(int value) const { return MulDiv(value, static_cast<int>(m_dpi), 96); }

    HWND m_window = nullptr;
    HWND m_label = nullptr;
    HFONT m_font = nullptr;
    UINT m_dpi = 96;
    bool m_visible = false;
    std::wstring m_title;
    std::wstring m_text;
};

// Watches the render thread for the life of the process; detached, as the process ends it at exit.
void Watch()
{
    Window window;
    long long shownAt = 0;
    for (;;) {
        window.Pump();
        Sleep(kPollMs);
        const long long now = NowMs();
        const long long lastPresent = g_lastPresent;
        if (shownAt != 0 && lastPresent > shownAt) {
            window.Hide();
            shownAt = 0;
        }
        const long long scopeStart = g_scopeStart;
        const long long busy = g_busyClosed + (scopeStart != 0 ? now - scopeStart : 0);
        const bool stalled = lastPresent == 0 || now - lastPresent >= kShowAfterMs;
        if (stalled && busy >= kShowAfterMs) {
            window.Show(static_cast<Reason>(g_reason.load()));
            if (shownAt == 0)
                shownAt = now;
        }
    }
}

void StartWatching()
{
    static std::once_flag once;
    std::call_once(once, [] {
        PinThisModule();
        std::thread(&Watch).detach();
    });
}

}

Scope::Scope(Reason reason)
{
    if (t_depth++ != 0)
        return;
    m_outer = true;
    LoadText();
    StartWatching();
    g_reason = static_cast<int>(reason);
    g_scopeStart = NowMs();
}

Scope::~Scope()
{
    --t_depth;
    if (!m_outer)
        return;
    g_busyClosed += NowMs() - g_scopeStart;
    g_scopeStart = 0;
}

void NotePresent()
{
    g_busyClosed = 0;
    g_lastPresent = NowMs();
    ++g_presents;
}

}
