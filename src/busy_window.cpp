#include "busy_window.h"

#include "command_line.h"
#include "log.h"
#include "options.h"
#include "overlay_fonts.h"
#include "text_layout.h"
#include "ui_text.h"

#include <windows.h>

#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace gmodpp::busy {

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kClassName[] = L"GarrysModPlusPlusBusy";
// Shown once the render thread has spent this long waiting since the last frame, and no frame came for as long.
constexpr long long kShowAfterMs = 1000;
constexpr DWORD kPollMs = 100;
constexpr DWORD kStyle = WS_POPUP;
// Over the game, which may cover the screen, without taking its focus, a taskbar button or clicks; drawn with
// per-pixel alpha (UpdateLayeredWindow).
constexpr DWORD kExStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;

// The in-game notice's look (DrawNotice in overlay.cpp, STYLE.md): the menu's window colour at 85% opacity, its 1-pixel
// border, its text colour, Roboto Medium over Roboto at the menu's font size, its window padding and line spacing.
constexpr D2D1_COLOR_F kBackground = {0.063f, 0.075f, 0.094f, 0.85f};
constexpr D2D1_COLOR_F kBorder = {1.0f, 1.0f, 1.0f, 0.07f};
constexpr D2D1_COLOR_F kText = {0.90f, 0.92f, 0.95f, 1.0f};
// Spacing in pixels at a 16-pixel font.
constexpr float kPaddingX = 12.0f;
constexpr float kPaddingY = 10.0f;
constexpr float kLineGap = 6.0f;
// The message wraps at this many font sizes.
constexpr float kWrapWidth = 28.0f;

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

// The window's text by Reason and its direction, read on the render thread (ui::Text is not thread-safe) for the
// window's thread, and the language and frame it was last checked in.
struct Text {
    std::string title;
    std::array<std::string, 2> messages;
    bool rightToLeft = false;
};
std::mutex g_textMutex;
Text g_text;
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
    g_text.title = ui::Text("busy.title");
    g_text.messages[static_cast<size_t>(Reason::Shaders)] = ui::Text("busy.shaders");
    g_text.messages[static_cast<size_t>(Reason::Pipelines)] = ui::Text("busy.pipelines");
    g_text.rightToLeft = ui::RightToLeft();
}

HINSTANCE ThisModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ThisModule),
        &module);
    return module;
}

// A font face's WWS family name, weight, style and stretch, by which a text format finds it.
struct FaceName {
    std::wstring family;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
};

// The fonts the menu draws text with (overlay_fonts.h): Roboto Medium for the title and Roboto for the message from
// the game's resource/fonts, then the fonts the game falls back to, in its order, for characters they lack.
struct Fonts {
    ComPtr<IDWriteFontCollection1> collection;
    ComPtr<IDWriteFontFallback> fallback;
    FaceName title;
    FaceName message;
};

std::optional<FaceName> AddFace(IDWriteFactory3* factory, IDWriteFontSetBuilder* builder, const overlay::fonts::Face& face)
{
    ComPtr<IDWriteFontFaceReference> reference;
    ComPtr<IDWriteFontFace3> fontFace;
    ComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(factory->CreateFontFaceReference(face.path.c_str(), nullptr, face.index, DWRITE_FONT_SIMULATIONS_NONE, &reference)) ||
        FAILED(reference->CreateFontFace(&fontFace)) || FAILED(fontFace->GetFamilyNames(&names)) || FAILED(builder->AddFontFaceReference(reference.Get())))
        return std::nullopt;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists)
        index = 0;
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(index, &length)))
        return std::nullopt;
    FaceName name;
    name.family.resize(length + 1);
    if (FAILED(names->GetString(index, name.family.data(), length + 1)))
        return std::nullopt;
    name.family.resize(length);
    name.weight = fontFace->GetWeight();
    name.style = fontFace->GetStyle();
    name.stretch = fontFace->GetStretch();
    return name;
}

std::optional<Fonts> LoadFonts(IDWriteFactory3* factory, const Text& text, Reason reason)
{
    const std::filesystem::path directory = GetOptions().gameDirectory / L"garrysmod" / L"resource" / L"fonts";
    const std::vector<overlay::fonts::Face> titleChain =
        overlay::fonts::GameFontChain(directory / L"Roboto-Medium.ttf", DWRITE_FONT_WEIGHT_MEDIUM, ui::DrawnCharacters(text.title));
    const std::vector<overlay::fonts::Face> messageChain = overlay::fonts::GameFontChain(directory / L"Roboto-Regular.ttf", DWRITE_FONT_WEIGHT_NORMAL,
        ui::DrawnCharacters(text.messages[static_cast<size_t>(reason)]));
    if (titleChain.empty() || messageChain.empty())
        return std::nullopt;
    ComPtr<IDWriteFontSetBuilder> builder;
    if (FAILED(factory->CreateFontSetBuilder(&builder)))
        return std::nullopt;
    Fonts fonts;
    std::vector<std::wstring> fallbackFamilies;
    for (const auto& chain : {&titleChain, &messageChain}) {
        for (size_t i = 0; i < chain->size(); ++i) {
            const std::optional<FaceName> name = AddFace(factory, builder.Get(), (*chain)[i]);
            if (!name)
                return std::nullopt;
            if (i == 0)
                (chain == &titleChain ? fonts.title : fonts.message) = *name;
            else if (std::ranges::find(fallbackFamilies, name->family) == fallbackFamilies.end())
                fallbackFamilies.push_back(name->family);
        }
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFactory2> factory2;
    ComPtr<IDWriteFontFallbackBuilder> fallbackBuilder;
    ComPtr<IDWriteFontFallback> system;
    if (FAILED(builder->CreateFontSet(&set)) || FAILED(factory->CreateFontCollectionFromFontSet(set.Get(), &fonts.collection)) ||
        FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory2))) || FAILED(factory2->CreateFontFallbackBuilder(&fallbackBuilder)) ||
        FAILED(factory2->GetSystemFontFallback(&system)))
        return std::nullopt;
    // The game's fallback fonts in its order for every character, then Windows' own for any they all lack.
    const DWRITE_UNICODE_RANGE everything = {0, 0x10FFFF};
    for (const std::wstring& family : fallbackFamilies) {
        const wchar_t* names[] = {family.c_str()};
        if (FAILED(fallbackBuilder->AddMapping(&everything, 1, names, 1, fonts.collection.Get(), nullptr, nullptr, 1.0f)))
            return std::nullopt;
    }
    if (FAILED(fallbackBuilder->AddMappings(system.Get())) || FAILED(fallbackBuilder->CreateFontFallback(&fonts.fallback)))
        return std::nullopt;
    return fonts;
}

// A device context with a 32-bit top-down DIB section, the surface a layered window is updated from.
class Surface {
public:
    ~Surface()
    {
        if (m_dc != nullptr) {
            SelectObject(m_dc, m_previous);
            DeleteDC(m_dc);
        }
        if (m_bitmap != nullptr)
            DeleteObject(m_bitmap);
    }

    bool Create(int width, int height)
    {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_dc = CreateCompatibleDC(nullptr);
        m_bitmap = m_dc != nullptr ? CreateDIBSection(m_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (m_bitmap == nullptr)
            return false;
        m_previous = SelectObject(m_dc, m_bitmap);
        return true;
    }

    HDC Dc() const { return m_dc; }

private:
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_previous = nullptr;
};

// The window, created and drawn on the window's thread.
class Window {
public:
    void Show(Reason reason)
    {
        Text text;
        {
            std::scoped_lock lock(g_textMutex);
            text = g_text;
        }
        if (m_window == nullptr && !Create())
            return;
        const std::string& message = text.messages[static_cast<size_t>(reason)];
        if (!m_visible || text.title != m_title || message != m_message || text.rightToLeft != m_rightToLeft) {
            if (!Draw(text, reason))
                return;
            m_title = text.title;
            m_message = message;
            m_rightToLeft = text.rightToLeft;
        }
        if (!m_visible) {
            ShowWindow(m_window, SW_SHOWNOACTIVATE);
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

    // Handles this thread's messages (the game's windows belong to other threads).
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
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool Create()
    {
        // Measured and drawn in physical pixels, like the menu in the game's swap chain, so DWM does not stretch it.
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HINSTANCE instance = ThisModule();
        WNDCLASSEXW windowClass = {sizeof(windowClass)};
        windowClass.lpfnWndProc = &Procedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;
        RegisterClassExW(&windowClass);
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.GetAddressOf());
        if (SUCCEEDED(hr))
            hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf()));
        if (SUCCEEDED(hr)) {
            // 96 DPI: one Direct2D unit is one pixel.
            const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            hr = m_d2d->CreateDCRenderTarget(&properties, &m_target);
        }
        if (FAILED(hr)) {
            log::Info("Busy window: Direct2D or DirectWrite is unavailable ({:#x})", static_cast<uint32_t>(hr));
            return false;
        }
        m_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        m_window = CreateWindowExW(kExStyle, kClassName, L"", kStyle, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
        if (m_window == nullptr) {
            log::Info("Busy window: creating it failed ({})", GetLastError());
            return false;
        }
        return true;
    }

    // The monitor the game's window is on (the one under the cursor before it has one).
    static HMONITOR GameMonitor()
    {
        const HWND game = GetForegroundWindow();
        DWORD process = 0;
        GetWindowThreadProcessId(game, &process);
        POINT cursor = {};
        GetCursorPos(&cursor);
        return game != nullptr && process == GetCurrentProcessId() ? MonitorFromWindow(game, MONITOR_DEFAULTTOPRIMARY)
                                                                  : MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    }

    ComPtr<IDWriteTextLayout> Layout(const Fonts* fonts, const FaceName& face, std::string_view text, float size, float width, bool rightToLeft)
    {
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextFormat1> format1;
        ComPtr<IDWriteTextLayout> layout;
        const std::wstring wide = Wide(text);
        const wchar_t* family = fonts != nullptr ? face.family.c_str() : L"Tahoma";
        if (FAILED(m_dwrite->CreateTextFormat(family, fonts != nullptr ? fonts->collection.Get() : nullptr, face.weight, face.style, face.stretch, size,
                L"", &format)))
            return nullptr;
        if (fonts != nullptr && SUCCEEDED(format.As(&format1)))
            format1->SetFontFallback(fonts->fallback.Get());
        format->SetReadingDirection(rightToLeft ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (FAILED(m_dwrite->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()), format.Get(), width, 10000.0f, &layout)))
            return nullptr;
        return layout;
    }

    bool Draw(const Text& text, Reason reason)
    {
        const HMONITOR monitor = GameMonitor();
        MONITORINFO info = {sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        // The menu's font size (overlay_widgets.cpp LoadTheme): the display's height x 15 / 1080, 14 to 30 pixels.
        const float size = std::clamp(std::round(static_cast<float>(info.rcMonitor.bottom - info.rcMonitor.top) * 15.0f / 1080.0f), 14.0f, 30.0f);
        const float scale = size / 16.0f;
        const std::optional<Fonts> fonts = LoadFonts(m_dwrite.Get(), text, reason);
        if (!fonts && !m_loggedFonts) {
            m_loggedFonts = true;
            log::Info("Busy window: the game's fonts could not be loaded; using Tahoma");
        }
        const Fonts* loaded = fonts ? &*fonts : nullptr;
        const FaceName fallbackTitle = {L"Tahoma", DWRITE_FONT_WEIGHT_BOLD};
        const FaceName fallbackMessage = {L"Tahoma"};
        const float wrap = std::round(size * kWrapWidth);
        ComPtr<IDWriteTextLayout> title = Layout(loaded, loaded ? loaded->title : fallbackTitle, text.title, size, wrap, text.rightToLeft);
        ComPtr<IDWriteTextLayout> message =
            Layout(loaded, loaded ? loaded->message : fallbackMessage, text.messages[static_cast<size_t>(reason)], size, wrap, text.rightToLeft);
        DWRITE_TEXT_METRICS titleMetrics = {};
        DWRITE_TEXT_METRICS messageMetrics = {};
        if (title == nullptr || message == nullptr || FAILED(title->GetMetrics(&titleMetrics)) || FAILED(message->GetMetrics(&messageMetrics))) {
            log::Info("Busy window: laying out its text failed");
            return false;
        }
        // Both lines start at the same edge: the content is as wide as the wider of them.
        const float contentWidth = std::ceil(std::max(titleMetrics.width, messageMetrics.width));
        title->SetMaxWidth(contentWidth);
        message->SetMaxWidth(contentWidth);
        const float paddingX = std::round(kPaddingX * scale);
        const float paddingY = std::round(kPaddingY * scale);
        const float gap = std::round(kLineGap * scale);
        const int width = static_cast<int>(contentWidth + 2.0f * paddingX);
        const int height = static_cast<int>(std::ceil(titleMetrics.height) + gap + std::ceil(messageMetrics.height) + 2.0f * paddingY);

        Surface surface;
        const RECT bounds = {0, 0, width, height};
        if (!surface.Create(width, height) || FAILED(m_target->BindDC(surface.Dc(), &bounds))) {
            log::Info("Busy window: preparing its surface failed");
            return false;
        }
        ComPtr<ID2D1SolidColorBrush> background;
        ComPtr<ID2D1SolidColorBrush> border;
        ComPtr<ID2D1SolidColorBrush> ink;
        m_target->BeginDraw();
        m_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        if (SUCCEEDED(m_target->CreateSolidColorBrush(kBackground, &background)) && SUCCEEDED(m_target->CreateSolidColorBrush(kBorder, &border)) &&
            SUCCEEDED(m_target->CreateSolidColorBrush(kText, &ink))) {
            m_target->FillRectangle(D2D1::RectF(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)), background.Get());
            m_target->DrawRectangle(D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f), border.Get(), 1.0f);
            m_target->DrawTextLayout(D2D1::Point2F(paddingX, paddingY), title.Get(), ink.Get());
            m_target->DrawTextLayout(D2D1::Point2F(paddingX, paddingY + std::ceil(titleMetrics.height) + gap), message.Get(), ink.Get());
        }
        const HRESULT drawn = m_target->EndDraw();
        if (FAILED(drawn)) {
            log::Info("Busy window: drawing failed ({:#x})", static_cast<uint32_t>(drawn));
            // A lost target is made again next time.
            if (drawn == D2DERR_RECREATE_TARGET) {
                m_target.Reset();
                DestroyWindow(m_window);
                m_window = nullptr;
            }
            return false;
        }

        // In the middle of the monitor's work area.
        POINT position = {info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2, info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2};
        SIZE extent = {width, height};
        POINT origin = {0, 0};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(m_window, nullptr, &position, &extent, surface.Dc(), &origin, 0, &blend, ULW_ALPHA)) {
            log::Info("Busy window: updating it failed ({})", GetLastError());
            return false;
        }
        SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return true;
    }

    HWND m_window = nullptr;
    ComPtr<ID2D1Factory> m_d2d;
    ComPtr<IDWriteFactory3> m_dwrite;
    ComPtr<ID2D1DCRenderTarget> m_target;
    bool m_visible = false;
    bool m_loggedFonts = false;
    std::string m_title;
    std::string m_message;
    bool m_rightToLeft = false;
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
