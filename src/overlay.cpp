#include "overlay.h"

#include "client_hooks.h"
#include "command_line.h"
#include "engine_interfaces.h"
#include "gmodpp_version.h"
#include "log.h"
#include "options.h"
#include "overlay_widgets.h"
#include "loading_screen.h"
#include "settings.h"
#include "shader_precache.h"
#include "shaderapi_device.h"
#include "text_layout.h"
#include "ui_text.h"
#include "world.h"

#include <MinHook.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <dxgi1_4.h>
#include <intrin.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gmodpp::overlay {

namespace {

using Microsoft::WRL::ComPtr;
using ui::Text;
using Scope = config::Scope;
using Clock = std::chrono::steady_clock;

// Work shorter than this shows no notice, so models and effects that compile a few shaders in play don't flash it.
constexpr auto kNoticeDelay = std::chrono::milliseconds(1500);
// The performance overlay's figures change this often, slow enough to read.
constexpr auto kOverlayRefresh = std::chrono::milliseconds(250);
constexpr auto kMemoryRefresh = std::chrono::milliseconds(500);
constexpr auto kStatusTime = std::chrono::seconds(8);
// A value just sent to the game's console shows until the console has applied it, or for this long.
constexpr auto kPendingVarTime = std::chrono::seconds(1);
// How often the Logs page looks for new files in the logs folder.
constexpr auto kLogFolderRefresh = std::chrono::seconds(2);

enum class Page { Display, Graphics, Image, Lighting, Sky, Camera, Performance, Profiles, Logs, Menu, About, Count };

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using SetCursorFn = HCURSOR(WINAPI*)(HCURSOR);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using GetCursorPosFn = BOOL(WINAPI*)(POINT*);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, void*, UINT*, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(RAWINPUT*, UINT*, UINT);

// What a file window the player was shown came back with.
struct FileChoice {
    enum class Result { Chosen, Cancelled, Failed } result = Result::Failed;
    std::wstring path;
};

// What a file window is for: settings (import or export) or a log export.
enum class FileKind { Settings, Log };

// An export or import waiting for its file window, which runs on its own thread so the game keeps drawing.
struct FileJob {
    FileKind kind = FileKind::Settings;
    bool save = false;
    Scope scope = Scope::Global;
    // What an export writes.
    std::string text;
    std::future<FileChoice> choice;
};

// A console variable's value as just sent to the game, shown until the game has taken it.
struct PendingVar {
    std::string name;
    std::string value;
    Clock::time_point since;
};

// A video mode picked on the Display page, applied with its Apply button.
struct PendingMode {
    int width = 0;
    int height = 0;
    // 0 full screen, 1 borderless window, 2 window (the Display mode row's order).
    int mode = 0;
};

enum class LogLevel : uint8_t { Info, Warning, Error };

// A log line as the Logs page shows it: the raw line for copying, where its message starts and how far its
// "Module:" prefix reaches, its level and its width in the body font.
struct LogEntry {
    std::string raw;
    uint32_t message = 0;
    uint32_t prefixEnd = 0;
    LogLevel level = LogLevel::Info;
    float width = 0.0f;
};

// The Logs page: the lines of the chosen source (the live log, or a file in the logs folder), the filter, the
// selection and the view.
struct LogView {
    // 0 the live log, else 1 + the index in `files`.
    int source = 0;
    std::vector<std::filesystem::path> files;
    Clock::time_point filesScanned = {};
    // The live log's last sequence taken, or the file loaded.
    uint64_t sequence = 0;
    std::filesystem::path loaded;
    std::vector<LogEntry> entries;
    float widest = 0.0f;
    // The entries that pass the filter, in order.
    std::vector<size_t> shown;
    char filter[128] = {};
    std::string appliedFilter;
    uint32_t levels = 7;
    uint32_t appliedLevels = 7;
    // How many entries `shown` already covers, and whether it must start over.
    size_t filtered = 0;
    bool refilter = true;
    bool follow = true;
    bool showTime = true;
    std::vector<bool> selected;
    size_t selectedCount = 0;
    std::optional<size_t> anchor;
    // Whether new lines arrived this frame, for following.
    bool grew = false;
};

// The confirmation or name the Profiles page is asking for.
enum class Dialog { None, DeletePreset, OverwritePreset, RenamePreset, ClearMap };

// The performance overlay's figures as last shown.
struct OverlayFigures {
    double fps = 0.0;
    std::optional<double> renderedFps;
    double frameMs = 0.0;
    double low1 = 0.0;
    double low01 = 0.0;
    std::vector<float> frameTimes;
    float longestFrame = 1.0f;
};

struct State {
    HWND window = nullptr;
    WNDPROC previousProc = nullptr;
    DescriptorHeap* descriptors = nullptr;
    bool initialized = false;
    bool open = false;
    Page page = Page::Display;
    Scope scope = Scope::Global;
    // The Menu page's "Toggle key" row is waiting for a key, and the key it got (a virtual-key code).
    bool listeningForKey = false;
    std::optional<UINT> capturedKey;
    // The menu window goes back to its default place and size next frame.
    bool resetLayout = false;
    // The theme as applied: the scale the fonts were built at (applied once the slider is released), and the display
    // height and font folder they are built for.
    float appliedScale = 1.0f;
    int displayHeight = 1080;
    std::filesystem::path fontDirectory;
    bool firstOpen = true;
    // Console variables just set, and the Display page's picked video mode.
    std::vector<PendingVar> pendingVars;
    std::optional<PendingMode> pendingMode;
    int aspectFilter = 0;
    LogView logs;
    SetCursorPosFn setCursorPos = nullptr;
    SetCursorFn setCursor = nullptr;
    ClipCursorFn clipCursor = nullptr;
    GetCursorPosFn getCursorPos = nullptr;
    GetRawInputDataFn getRawInputData = nullptr;
    GetRawInputBufferFn getRawInputBuffer = nullptr;
    // Where the game last put the cursor for mouse look; while the menu is open it keeps reading this point.
    POINT gameCursor = {};
    // The cursor shape the game last asked for, put back when the menu closes.
    HCURSOR gameCursorShape = nullptr;
    bool gameCursorShapeKnown = false;
    // The notice's text this frame, and since when there has been one.
    std::string notice;
    std::optional<Clock::time_point> noticeSince;

    // The Profiles page: the preset name being typed, the sections a saved preset and an export hold, the open dialog
    // and the preset it is about, the new name while renaming, and the last action's outcome.
    char presetName[96] = {};
    uint32_t presetSections = 0x1F;
    uint32_t shareSections = 0x1F;
    Dialog dialog = Dialog::None;
    // Set when a dialog is asked for, until DrawDialog opens it where it is drawn.
    bool dialogRequested = false;
    std::string dialogPreset;
    char renameBuffer[96] = {};
    bool renameRejected = false;
    std::string status;
    Clock::time_point statusTime = {};
    std::optional<FileJob> fileJob;

    // The performance overlay: its figures, when they were taken, and the adapter's memory for the Extra level.
    // Whether each page had anything to scroll last frame, which shows its scrollbar's grab.
    std::array<bool, static_cast<size_t>(Page::Count)> pageScrolls = {};

    OverlayFigures figures;
    Clock::time_point figuresTime = {};
    ComPtr<IDXGIAdapter3> adapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory = {};
    bool memoryKnown = false;
    Clock::time_point memoryTime = {};
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

// While the menu is open it draws the only cursor, and the shapes the game sets wait until it closes.
HCURSOR WINAPI HookSetCursor(HCURSOR cursor)
{
    if (CalledByGame(_ReturnAddress())) {
        const HCURSOR previous = std::exchange(g_state.gameCursorShape, cursor);
        g_state.gameCursorShapeKnown = true;
        if (g_state.open)
            return previous;
    }
    return g_state.setCursor(cursor);
}

void OpenMenu()
{
    g_state.open = true;
    log::Info("Overlay: menu opened");
    if (std::exchange(g_state.firstOpen, false) && GetSettings().menuRememberPage) {
        const int page = GetSettings().menuLastPage;
        if (page >= 0 && page < static_cast<int>(Page::Count))
            g_state.page = static_cast<Page>(page);
    }
    g_state.getCursorPos(&g_state.gameCursor);
    g_state.clipCursor(nullptr);
    g_state.setCursor(nullptr);
}

void CloseMenu()
{
    g_state.open = false;
    g_state.listeningForKey = false;
    log::Info("Overlay: menu closed");
    if (g_state.gameCursorShapeKnown)
        g_state.setCursor(g_state.gameCursorShape);
}

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
    const bool keyDown = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const bool keyUp = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    // The Menu page takes the next key pressed as the toggle key; Escape keeps the old one.
    if (g_state.open && g_state.listeningForKey && (keyDown || keyUp)) {
        if (keyDown) {
            if (wParam != VK_ESCAPE)
                g_state.capturedKey = static_cast<UINT>(wParam);
            g_state.listeningForKey = false;
        }
        return 0;
    }
    if ((keyDown || keyUp) && wParam == static_cast<WPARAM>(GetSettings().menuKey)) {
        if (keyDown && (lParam & (1 << 30)) == 0) {
            if (g_state.open)
                CloseMenu();
            else
                OpenMenu();
        }
        return 0;
    }
    if (g_state.open && g_state.initialized) {
        ImGui_ImplWin32_WndProcHandler(window, msg, wParam, lParam);
        // Passed on, DefWindowProc would put the game's class cursor back over the client area on every mouse move
        // (WM_SETCURSOR); TRUE stops that.
        if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT)
            return TRUE;
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

std::wstring Wide(std::string_view text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string Narrow(std::wstring_view text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

void SetStatus(std::string text)
{
    g_state.status = std::move(text);
    g_state.statusTime = Clock::now();
}

// The clipboard is opened for the game window: opened for no window, EmptyClipboard leaves it without an owner and
// SetClipboardData fails (OpenClipboard's documentation).
bool CopyToClipboard(std::string_view text)
{
    const std::wstring wide = Wide(text);
    if (!OpenClipboard(g_state.window))
        return false;
    bool copied = false;
    if (EmptyClipboard()) {
        const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* data = GlobalLock(memory)) {
                std::memcpy(data, wide.c_str(), bytes);
                GlobalUnlock(memory);
                copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            }
            if (!copied)
                GlobalFree(memory);
        }
    }
    CloseClipboard();
    return copied;
}

std::optional<std::string> ClipboardText()
{
    if (!OpenClipboard(g_state.window))
        return std::nullopt;
    std::optional<std::string> text;
    if (HANDLE memory = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* data = static_cast<const wchar_t*>(GlobalLock(memory))) {
            text = Narrow(data);
            GlobalUnlock(memory);
        }
    }
    CloseClipboard();
    return text;
}

// What a file window offers: its title, the file type's name and pattern ("*.ini"), the extension given to a name typed
// without one, the "All files" name and the name a save window starts with.
struct FileWindow {
    std::wstring title;
    std::wstring typeName;
    std::wstring pattern;
    std::wstring extension;
    std::wstring allName;
    std::wstring fileName;
};

// Shows the system's save or open window over the game window; runs on its own thread (an STA, as the Common Item
// Dialog needs).
FileChoice ChooseFile(HWND owner, bool save, FileWindow window)
{
    FileChoice choice;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        log::Info("Overlay: COM initialization for the file window failed");
        return choice;
    }
    {
        ComPtr<IFileDialog> dialog;
        HRESULT result = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        DWORD options = 0;
        const COMDLG_FILTERSPEC types[] = {{window.typeName.c_str(), window.pattern.c_str()}, {window.allName.c_str(), L"*.*"}};
        if (SUCCEEDED(result))
            result = dialog->GetOptions(&options);
        if (SUCCEEDED(result))
            result = dialog->SetOptions(options | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(result))
            result = dialog->SetFileTypes(static_cast<UINT>(std::size(types)), types);
        if (SUCCEEDED(result))
            result = dialog->SetDefaultExtension(window.extension.c_str());
        if (SUCCEEDED(result))
            result = dialog->SetTitle(window.title.c_str());
        if (SUCCEEDED(result) && save)
            result = dialog->SetFileName(window.fileName.c_str());
        if (SUCCEEDED(result))
            result = dialog->Show(owner);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            choice.result = FileChoice::Result::Cancelled;
        } else {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(result))
                result = dialog->GetResult(&item);
            PWSTR path = nullptr;
            if (SUCCEEDED(result))
                result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (SUCCEEDED(result)) {
                choice.result = FileChoice::Result::Chosen;
                choice.path = path;
                CoTaskMemFree(path);
            } else {
                log::Info("Overlay: the file window failed ({:#x})", static_cast<uint32_t>(result));
            }
        }
    }
    CoUninitialize();
    return choice;
}

void StartFileJob(FileKind kind, bool save, Scope scope, std::string text)
{
    PinThisModule();
    FileJob job;
    job.kind = kind;
    job.save = save;
    job.scope = scope;
    job.text = std::move(text);
    FileWindow window;
    window.allName = Wide(Text("profiles.all_files"));
    if (kind == FileKind::Settings) {
        window.title = Wide(Text(save ? "profiles.export_title" : "profiles.import_title"));
        window.typeName = Wide(Text("profiles.file_type"));
        window.pattern = L"*.ini";
        window.extension = L"ini";
        window.fileName = Wide(Text("profiles.export_file_name"));
    } else {
        window.title = Wide(Text("logs.export_title"));
        window.typeName = Wide(Text("logs.file_type"));
        window.pattern = L"*.log;*.txt";
        window.extension = L"log";
        window.fileName = Wide(Text("logs.export_file_name"));
    }
    job.choice = std::async(std::launch::async, &ChooseFile, g_state.window, save, std::move(window));
    g_state.fileJob = std::move(job);
}

void FinishFileJob()
{
    if (!g_state.fileJob || g_state.fileJob->choice.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    FileJob job = std::move(*g_state.fileJob);
    g_state.fileJob.reset();
    const FileChoice choice = job.choice.get();
    if (choice.result == FileChoice::Result::Cancelled)
        return;
    if (choice.result == FileChoice::Result::Failed) {
        SetStatus(Text("status.dialog_failed"));
        return;
    }
    const std::string file = Narrow(std::filesystem::path(choice.path).filename().native());
    if (job.save) {
        const bool written = config::WriteTextFile(choice.path, job.text);
        if (job.kind == FileKind::Log)
            SetStatus(ui::Format(written ? "status.log_exported" : "status.export_failed", {{"file", file}}));
        else
            SetStatus(ui::Format(written ? "status.exported" : "status.export_failed", {{"file", file}}));
        return;
    }
    const std::optional<std::string> text = config::ReadTextFile(choice.path);
    if (!text) {
        SetStatus(ui::Format("status.import_failed", {{"file", file}}));
        return;
    }
    SetStatus(ui::Format(config::Import(job.scope, *text) ? "status.imported" : "status.import_empty", {{"file", file}}));
}

// The width slider values keep for their text, so every track in the menu ends at the same place.
float ValueWidth() { return ImGui::CalcTextSize(ui::Number(99.99, 2).c_str()).x; }

// The settings the pages edit: a copy of what the menu's scope shows, committed after the frame, and the fields whose
// reset buttons were clicked.
struct Edit {
    Settings* settings = nullptr;
    Scope scope = Scope::Global;
    std::vector<FieldId> resets;
};
Edit g_edit;

// The fields of the row being drawn, for its reset button.
std::vector<FieldId> g_rowFields;
// Text a row's mark refers to, kept until the row ends.
std::string g_rowNote;

bool OnMap() { return !config::MapName().empty(); }

// Marks rows the current map sets: as its own while editing the map, as differing while editing every map. Rows get a
// reset button where there is something to go back to: on the map its own value, otherwise a value off its default.
void BeginRow(const char* key, std::span<const FieldId> fields, bool enabled = true, const char* reason = nullptr)
{
    ImGui::PushID(key);
    g_rowFields.assign(fields.begin(), fields.end());
    widgets::RowMark mark = widgets::RowMark::None;
    const char* note = nullptr;
    const char* resetHint = nullptr;
    const Settings defaults;
    bool mapSets = false;
    bool perMap = false;
    bool changed = false;
    for (const FieldId field : fields) {
        mapSets = mapSets || config::MapSets(field);
        perMap = perMap || config::PerMap(field);
        changed = changed || !SameValue(*g_edit.settings, defaults, field);
    }
    if (g_edit.scope == Scope::Map && perMap && OnMap()) {
        if (mapSets) {
            mark = widgets::RowMark::Map;
            resetHint = Text("menu.reset_to_global");
        }
    } else {
        if (mapSets && OnMap()) {
            mark = widgets::RowMark::MapElsewhere;
            g_rowNote = ui::Format("menu.map_value_note", {{"map", config::MapName()}});
            note = g_rowNote.c_str();
        }
        if (changed)
            resetHint = Text("menu.reset_to_default");
    }
    widgets::BeginRow(Text(key), enabled, reason, mark, note, resetHint);
}

void BeginRow(const char* key, std::initializer_list<FieldId> fields = {}, bool enabled = true, const char* reason = nullptr)
{
    BeginRow(key, std::span<const FieldId>(fields.begin(), fields.size()), enabled, reason);
}

bool EndRow(bool clickableLabel = false)
{
    const widgets::RowClicks clicks = widgets::EndRow(clickableLabel);
    if (clicks.reset)
        g_edit.resets.insert(g_edit.resets.end(), g_rowFields.begin(), g_rowFields.end());
    ImGui::PopID();
    return clicks.label;
}

Settings& Edited() { return *g_edit.settings; }

// A switch; its label flips it too.
void SwitchRow(const char* key, bool Settings::* member, bool enabled = true, const char* reason = nullptr)
{
    bool& value = Edited().*member;
    BeginRow(key, {FieldOf(member)}, enabled, reason);
    widgets::Switch("##switch", &value);
    if (EndRow(true))
        value = !value;
}

// A switch that starts an override from what the game shows now when it is turned on, so nothing jumps.
template <class Start>
void OverrideRow(const char* key, bool Settings::* member, bool enabled, const char* reason, Start start)
{
    const bool was = Edited().*member;
    SwitchRow(key, member, enabled, reason);
    if (!was && Edited().*member)
        start(Edited());
}

// A strength slider after the switch that enables it; without a switch (`on` null) it lines up with the others.
void StrengthRow(const char* key, bool Settings::* on, float Settings::* member, bool enabled = true, const char* reason = nullptr)
{
    float& value = Edited().*member;
    const SettingRange range = RangeOf(member);
    if (on != nullptr)
        BeginRow(key, {FieldOf(on), FieldOf(member)}, enabled, reason);
    else
        BeginRow(key, {FieldOf(member)}, enabled, reason);
    const float start = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x - widgets::SwitchSpan();
    // The switch comes first in the line's direction: on the left, or on the right for right-to-left languages.
    const bool rightToLeft = widgets::RightToLeft();
    const float switchX = rightToLeft ? start + width + widgets::SwitchSpan() - widgets::SwitchWidth() : start;
    if (on != nullptr) {
        ImGui::SetCursorPosX(switchX);
        widgets::Switch("##switch", &(Edited().*on));
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(rightToLeft ? start : start + widgets::SwitchSpan());
    ImGui::BeginDisabled(on != nullptr && !(Edited().*on));
    widgets::Slider("##value", &value, range.minimum, range.maximum, ui::Number(value, 2).c_str(), ValueWidth(), width);
    ImGui::EndDisabled();
    if (EndRow(on != nullptr))
        Edited().*on = !(Edited().*on);
}

// A slider whose value shows `decimals` digits after the separator and is rounded to them.
void SliderRow(const char* key, float Settings::* member, bool enabled = true, const char* reason = nullptr, int decimals = 2)
{
    float& value = Edited().*member;
    const SettingRange range = RangeOf(member);
    BeginRow(key, {FieldOf(member)}, enabled, reason);
    if (widgets::Slider("##value", &value, range.minimum, range.maximum, ui::Number(value, decimals).c_str(), ValueWidth())) {
        const float step = std::pow(10.0f, static_cast<float>(decimals));
        value = std::clamp(std::round(value * step) / step, range.minimum, range.maximum);
    }
    EndRow();
}

void SliderRow(const char* key, int Settings::* member, bool enabled = true, const char* reason = nullptr)
{
    int& value = Edited().*member;
    const SettingRange range = RangeOf(member);
    BeginRow(key, {FieldOf(member)}, enabled, reason);
    widgets::Slider("##value", &value, static_cast<int>(range.minimum), static_cast<int>(range.maximum), ui::Number(value, 0).c_str(), ValueWidth());
    EndRow();
}

void ColorRow(const char* key, Color Settings::* member, bool temperature, bool enabled = true, const char* reason = nullptr)
{
    BeginRow(key, {FieldOf(member)}, enabled, reason);
    widgets::ColorButton("##colour", &(Edited().*member), temperature ? Text("menu.temperature") : nullptr);
    EndRow();
}

// Text keys as text, for widgets that take a list of options.
std::vector<const char*> Texts(std::span<const char* const> keys)
{
    std::vector<const char*> texts;
    for (const char* key : keys)
        texts.push_back(Text(key));
    return texts;
}

// A full-width drop-down list of `options` (text keys) for an enum setting.
template <class E>
void ComboRow(const char* key, E Settings::* member, std::span<const char* const> options, bool enabled = true, const char* reason = nullptr)
{
    BeginRow(key, {FieldOf(member)}, enabled, reason);
    int index = static_cast<int>(Edited().*member);
    widgets::Combo("##combo", &index, Texts(options));
    Edited().*member = static_cast<E>(index);
    EndRow();
}

// A row of buttons, one per option (text keys); returns true when another option is picked. `fields` (the setting's
// own by default) are what its mark and reset button cover.
template <class E>
bool SegmentRow(const char* key, E Settings::* member, std::span<const char* const> options, std::span<const bool> available = {}, bool enabled = true,
    const char* reason = nullptr, std::span<const FieldId> fields = {})
{
    if (fields.empty())
        BeginRow(key, {FieldOf(member)}, enabled, reason);
    else
        BeginRow(key, fields, enabled, reason);
    int index = static_cast<int>(Edited().*member);
    const bool changed = widgets::Segmented("##segments", &index, Texts(options), available);
    Edited().*member = static_cast<E>(index);
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

void DisplayRow()
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
    BeginRow("general.display", {FieldOf(&Settings::display)});
    // A display that is gone shows as Default, which is where the game window then stays.
    int& display = Edited().display;
    int index = display <= static_cast<int>(displays.size()) ? display : 0;
    if (widgets::Combo("##combo", &index, texts))
        display = index;
    EndRow();
}

// sRGB components (gamma 1 / 2.2, as Settings stores colours) of a linear colour, brightest component 1; nothing for black.
std::optional<Color> HueOf(const std::array<float, 3>& linear)
{
    const float brightest = std::max({linear[0], linear[1], linear[2]});
    if (brightest <= 0.0f)
        return std::nullopt;
    Color color;
    for (size_t i = 0; i < 3; ++i)
        color[i] = std::pow(linear[i] / brightest, 1.0f / 2.2f);
    return color;
}

// The game's own settings are its console variables, read through the engine and set as console commands. A value
// just sent shows until the game has taken it (the console runs its commands next frame).
bool SameNumber(const std::string& a, const std::string& b)
{
    char* endA = nullptr;
    char* endB = nullptr;
    const double x = std::strtod(a.c_str(), &endA);
    const double y = std::strtod(b.c_str(), &endB);
    if (endA == a.c_str() || endB == b.c_str() || *endA != '\0' || *endB != '\0')
        return a == b;
    return std::abs(x - y) < 1e-4;
}

std::optional<std::string> ReadVar(const char* name)
{
    const auto now = Clock::now();
    std::erase_if(g_state.pendingVars, [&](const PendingVar& pending) { return now - pending.since > kPendingVarTime; });
    const std::optional<std::string> value = engine::ConVarString(name);
    for (auto pending = g_state.pendingVars.begin(); pending != g_state.pendingVars.end(); ++pending) {
        if (pending->name != name)
            continue;
        if (value && SameNumber(*value, pending->value)) {
            g_state.pendingVars.erase(pending);
            return value;
        }
        return pending->value;
    }
    return value;
}

int VarInt(const char* name, int fallback)
{
    const std::optional<std::string> value = ReadVar(name);
    return value ? static_cast<int>(std::strtol(value->c_str(), nullptr, 10)) : fallback;
}

float VarFloat(const char* name, float fallback)
{
    const std::optional<std::string> value = ReadVar(name);
    return value ? std::strtof(value->c_str(), nullptr) : fallback;
}

// `save` also runs mat_savechanges, which applies the material system's video settings and saves them as the game's own
// Apply button does (COptionsSubVideo::OnApplyChanges).
void WriteVar(const char* name, std::string value, bool save)
{
    engine::ExecuteCommand(std::format("{} {}", name, value));
    if (save)
        engine::ExecuteCommand("mat_savechanges");
    for (PendingVar& pending : g_state.pendingVars) {
        if (pending.name == name) {
            pending.value = std::move(value);
            pending.since = Clock::now();
            return;
        }
    }
    g_state.pendingVars.push_back({name, std::move(value), Clock::now()});
}

bool ConsoleReady() { return engine::Ready(); }
const char* ConsoleReason() { return Text("video.console_reason"); }

// A switch over a console variable (0 off, else on).
void GameSwitchRow(const char* key, const char* name, bool save, bool enabled = true, const char* reason = nullptr)
{
    const std::optional<std::string> value = ReadVar(name);
    const bool available = ConsoleReady() && value.has_value();
    bool on = value && std::strtod(value->c_str(), nullptr) != 0.0;
    BeginRow(key, {}, available && enabled, available ? reason : ConsoleReason());
    widgets::Switch("##switch", &on);
    if (EndRow(true))
        on = !on;
    if (value && on != (std::strtod(value->c_str(), nullptr) != 0.0))
        WriteVar(name, on ? "1" : "0", save);
}

// A choice over console variables: `index` is the current option, `pick` sets the variables for a picked one.
template <class Pick>
void GameChoiceRow(const char* key, std::span<const char* const> options, int index, Pick pick, bool enabled = true, const char* reason = nullptr)
{
    const bool available = ConsoleReady();
    BeginRow(key, {}, available && enabled, available ? reason : ConsoleReason());
    int picked = std::clamp(index, 0, static_cast<int>(options.size()) - 1);
    const bool changed = options.size() <= 4 ? widgets::Segmented("##choice", &picked, Texts(options)) : widgets::Combo("##choice", &picked, Texts(options));
    EndRow();
    if (changed)
        pick(picked);
}

// A slider over a console variable, rounded to `decimals`.
void GameSliderRow(const char* key, const char* name, float minimum, float maximum, int decimals, bool save, bool enabled = true,
    const char* reason = nullptr)
{
    const std::optional<std::string> text = ReadVar(name);
    const bool available = ConsoleReady() && text.has_value();
    float value = std::clamp(text ? std::strtof(text->c_str(), nullptr) : minimum, minimum, maximum);
    BeginRow(key, {}, available && enabled, available ? reason : ConsoleReason());
    if (widgets::Slider("##value", &value, minimum, maximum, ui::Number(value, decimals).c_str(), ValueWidth())) {
        const float step = std::pow(10.0f, static_cast<float>(decimals));
        value = std::clamp(std::round(value * step) / step, minimum, maximum);
        WriteVar(name, std::format("{:.{}f}", value, decimals), save);
    }
    EndRow();
}

// The name of a virtual key as the keyboard layout calls it ("Insert", "F8").
std::string KeyName(UINT key)
{
    constexpr UINT kExtended[] = {VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN, VK_NUMLOCK, VK_DIVIDE,
        VK_RCONTROL, VK_RMENU, VK_SNAPSHOT, VK_LWIN, VK_RWIN, VK_APPS};
    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    LONG lParam = static_cast<LONG>(scan << 16);
    if (std::ranges::find(kExtended, key) != std::end(kExtended))
        lParam |= 1 << 24;
    wchar_t name[64] = {};
    if (scan != 0 && GetKeyNameTextW(lParam, name, static_cast<int>(std::size(name))) > 0)
        return Narrow(name);
    return ui::Format("menupage.key_code", {{"code", std::to_string(key)}});
}

void OpenInShell(const std::wstring& target)
{
    PinThisModule();
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Whether a size has the aspect ratio the Display page's filter picks (0: any).
bool MatchesAspect(int filter, int width, int height)
{
    constexpr float kRatios[] = {0.0f, 4.0f / 3.0f, 16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f};
    constexpr float kTolerance[] = {0.0f, 0.03f, 0.03f, 0.03f, 0.08f};
    if (filter <= 0 || filter >= static_cast<int>(std::size(kRatios)))
        return true;
    const float ratio = static_cast<float>(width) / static_cast<float>(height);
    return std::abs(ratio - kRatios[filter]) < kTolerance[filter];
}

void DrawDisplayPage(Settings&)
{
    const bool console = ConsoleReady();
    const native::VideoMode current = native::CurrentVideoMode();
    const int currentMode = !current.windowed ? 0 : current.borderless ? 1 : 2;
    // A new current mode (applied here or by the game's own options) is what the rows start from.
    static std::optional<native::VideoMode> seen;
    if (!seen || seen->width != current.width || seen->height != current.height || seen->windowed != current.windowed || seen->borderless != current.borderless) {
        seen = current;
        g_state.pendingMode = PendingMode{current.width, current.height, currentMode};
    }
    PendingMode& pending = *g_state.pendingMode;

    if (widgets::BeginCard("##display", Text("display.section_display"), Text("display.mode_note"))) {
        if (widgets::BeginRows("display")) {
            DisplayRow();
            constexpr const char* kModes[] = {"display.mode_fullscreen", "display.mode_borderless", "display.mode_windowed"};
            BeginRow("display.mode", {}, console, ConsoleReason());
            widgets::Segmented("##mode", &pending.mode, Texts(kModes));
            EndRow();
            constexpr const char* kAspects[] = {"display.aspect_all", "display.aspect_4_3", "display.aspect_16_9", "display.aspect_16_10", "display.aspect_21_9"};
            BeginRow("display.aspect", {}, console, ConsoleReason());
            widgets::Combo("##aspect", &g_state.aspectFilter, Texts(kAspects));
            EndRow();

            std::vector<std::pair<int, int>> sizes;
            for (const std::pair<int, int>& size : native::Resolutions()) {
                if (MatchesAspect(g_state.aspectFilter, size.first, size.second))
                    sizes.push_back(size);
            }
            const std::pair<int, int> picked(pending.width, pending.height);
            if (std::ranges::find(sizes, picked) == sizes.end())
                sizes.insert(sizes.begin(), picked);
            std::vector<std::string> names;
            for (const std::pair<int, int>& size : sizes)
                names.push_back(ui::Format("display.resolution_value", {{"width", std::to_string(size.first)}, {"height", std::to_string(size.second)}}));
            std::vector<const char*> texts;
            for (const std::string& name : names)
                texts.push_back(name.c_str());
            BeginRow("display.resolution", {}, console, ConsoleReason());
            int index = static_cast<int>(std::ranges::find(sizes, picked) - sizes.begin());
            if (widgets::Combo("##resolution", &index, texts)) {
                pending.width = sizes[static_cast<size_t>(index)].first;
                pending.height = sizes[static_cast<size_t>(index)].second;
            }
            EndRow();
            widgets::EndRows();
        }
        const bool changed = pending.width != current.width || pending.height != current.height || pending.mode != currentMode;
        ImGui::BeginDisabled(!console || !changed);
        // mat_setvideomode <width> <height> <windowed> [noborder]: GMod's engine takes the fourth argument (engine.dll
        // 0x180114b80, REA 2026-11-03), as its own video options send it; mat_savechanges saves the mode.
        if (widgets::EndButton(Text("display.apply"))) {
            engine::ExecuteCommand(std::format("mat_setvideomode {} {} {} {}", pending.width, pending.height, pending.mode != 0 ? 1 : 0, pending.mode == 1 ? 1 : 0));
            engine::ExecuteCommand("mat_savechanges");
            log::Info("Overlay: video mode {}x{} {}", pending.width, pending.height, pending.mode == 0 ? "full screen" : pending.mode == 1 ? "borderless" : "windowed");
        }
        ImGui::EndDisabled();
    }
    widgets::EndCard();

    if (widgets::BeginCard("##framerate", Text("display.section_frame_rate"))) {
        if (widgets::BeginRows("framerate")) {
            GameSwitchRow("display.vsync", "mat_vsync", true);
            SwitchRow("general.background_full_speed", &Settings::backgroundFullSpeed);
            // fps_max: 0 is unlimited; the game's own slider runs from 30 to 300.
            const std::optional<std::string> limitText = ReadVar("fps_max");
            const bool limitAvailable = console && limitText.has_value();
            const float limitValue = limitText ? std::strtof(limitText->c_str(), nullptr) : 0.0f;
            bool limited = limitValue > 0.0f;
            float limit = std::clamp(limited ? limitValue : 144.0f, 30.0f, 300.0f);
            BeginRow("display.frame_limit", {}, limitAvailable, ConsoleReason());
            const float start = ImGui::GetCursorPosX();
            const float width = ImGui::GetContentRegionAvail().x - widgets::SwitchSpan();
            const bool rightToLeft = widgets::RightToLeft();
            ImGui::SetCursorPosX(rightToLeft ? start + width + widgets::SwitchSpan() - widgets::SwitchWidth() : start);
            const bool toggled = widgets::Switch("##switch", &limited);
            ImGui::SameLine();
            ImGui::SetCursorPosX(rightToLeft ? start : start + widgets::SwitchSpan());
            ImGui::BeginDisabled(!limited);
            const bool moved = widgets::Slider("##value", &limit, 30.0f, 300.0f, ui::Number(limit, 0).c_str(), ValueWidth(), width);
            ImGui::EndDisabled();
            const bool labelClicked = EndRow(true);
            if (labelClicked)
                limited = !limited;
            if (limitAvailable && (toggled || moved || labelClicked))
                WriteVar("fps_max", limited ? std::to_string(static_cast<int>(std::round(limit))) : "0", false);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##picture", Text("display.section_picture"))) {
        if (widgets::BeginRows("picture")) {
            // mat_monitorgamma only sets the display's gamma ramp, which Direct3D 9 applied in exclusive full screen alone;
            // the native renderer presents windowed or borderless and never sets one (SetHardwareGammaRamp).
            GameSliderRow("display.brightness", "mat_monitorgamma", 1.6f, 2.6f, 1, true, false, Text("display.brightness_reason"));
            GameSliderRow("display.fov", "fov_desired", 75.0f, 100.0f, 0, false);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##engine", Text("display.section_engine"), Text("display.multicore_note"))) {
        if (widgets::BeginRows("engine")) {
            // mat_queue_mode: -1 lets the material system pick multithreaded rendering, 0 keeps everything on the main
            // thread (the game's "Multicore rendering" checkbox).
            const std::optional<std::string> queue = ReadVar("mat_queue_mode");
            bool multicore = queue && std::strtol(queue->c_str(), nullptr, 10) != 0;
            BeginRow("display.multicore", {}, console && queue.has_value(), ConsoleReason());
            widgets::Switch("##switch", &multicore);
            if (EndRow(true))
                multicore = !multicore;
            if (queue && multicore != (std::strtol(queue->c_str(), nullptr, 10) != 0))
                WriteVar("mat_queue_mode", multicore ? "-1" : "0", false);
            SwitchRow("display.hardware_warning", &Settings::hardwareWarning);
            widgets::EndRows();
        }
    }
    widgets::EndCard();
}

void DrawLightingPage(Settings& settings)
{
    const Statistics& statistics = GetStatistics();
    if (widgets::BeginCard("##realtime")) {
        if (widgets::BeginRows("realtime")) {
            SwitchRow("general.realtime_lighting", &Settings::realtimeLighting);
            widgets::EndRows();
        }
        constexpr const char* kBlocks[] = {nullptr, "general.realtime_unsupported", "general.realtime_other_thread"};
        if (settings.realtimeLighting && statistics.realtimeBlock != RealtimeBlock::None)
            widgets::Text(Text(kBlocks[static_cast<size_t>(statistics.realtimeBlock)]));
    }
    widgets::EndCard();

    if (widgets::BeginCard("##sun", Text("lighting.section_sun"))) {
        if (widgets::BeginRows("sun")) {
            StrengthRow("lighting.sun", &Settings::sun, &Settings::sunIntensity);
            const bool sun = settings.sun;
            const char* sunReason = Text("lighting.sun_reason");
            SwitchRow("lighting.sun_follows_env_sun", &Settings::sunFollowsEnvSun, sun && !settings.sunDirectionOverride,
                Text(sun ? "lighting.sun_follows_env_sun_override_reason" : "lighting.sun_reason"));
            OverrideRow("lighting.sun_direction_override", &Settings::sunDirectionOverride, sun, sunReason, [&](Settings& edited) {
                constexpr float kDegrees = 180.0f / 3.14159265f;
                const std::array<float, 3>& direction = statistics.sunDirection;
                edited.sunElevation = std::round(std::asin(std::clamp(direction[2], -1.0f, 1.0f)) * kDegrees * 10.0f) / 10.0f;
                const float azimuth = std::atan2(direction[1], direction[0]) * kDegrees;
                edited.sunAzimuth = std::round((azimuth < 0.0f ? azimuth + 360.0f : azimuth) * 10.0f) / 10.0f;
            });
            const bool direction = sun && settings.sunDirectionOverride;
            const char* directionReason = Text(sun ? "lighting.sun_direction_reason" : "lighting.sun_reason");
            SliderRow("lighting.sun_elevation", &Settings::sunElevation, direction, directionReason, 1);
            SliderRow("lighting.sun_azimuth", &Settings::sunAzimuth, direction, directionReason, 1);
            OverrideRow("lighting.sun_color_override", &Settings::sunColorOverride, sun, sunReason, [&](Settings& edited) {
                if (const std::optional<Color> hue = HueOf(statistics.mapSunColor))
                    edited.sunColor = *hue;
            });
            ColorRow("lighting.sun_color", &Settings::sunColor, true, sun && settings.sunColorOverride,
                Text(sun ? "lighting.sun_color_reason" : "lighting.sun_reason"));
            SliderRow("lighting.sun_shadow_softness", &Settings::sunShadowSoftness, sun, sunReason);
            widgets::EndRows();
        }
        if (OnMap() && !statistics.mapHasSun)
            widgets::Note(Text("lighting.no_sun_note"));
    }
    widgets::EndCard();

    if (widgets::BeginCard("##skylight", Text("lighting.section_sky_light"))) {
        if (widgets::BeginRows("skylight")) {
            StrengthRow("lighting.sky", nullptr, &Settings::skyIntensity);
            OverrideRow("lighting.sky_light_override", &Settings::skyLightOverride, true, nullptr, [&](Settings& edited) {
                if (const std::optional<Color> hue = HueOf(statistics.mapSkyColor)) {
                    const std::array<float, 3>& sky = statistics.mapSkyColor;
                    const SettingRange range = RangeOf(&Settings::skyLightBrightness);
                    edited.skyLightColor = *hue;
                    edited.skyLightBrightness = std::clamp(std::round(255.0f * std::max({sky[0], sky[1], sky[2]})), range.minimum, range.maximum);
                }
            });
            const bool sky = settings.skyLightOverride;
            const char* skyReason = Text("lighting.sky_light_reason");
            ColorRow("lighting.sky_light_color", &Settings::skyLightColor, true, sky, skyReason);
            SliderRow("lighting.sky_light_brightness", &Settings::skyLightBrightness, sky, skyReason, 0);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##lamps", Text("lighting.section_lamps"))) {
        if (widgets::BeginRows("lamps")) {
            StrengthRow("lighting.map_lights", &Settings::mapLights, &Settings::lightIntensity);
            ColorRow("lighting.light_tint", &Settings::lightTint, true, settings.mapLights, Text("lighting.map_lights_reason"));
            SwitchRow("lighting.dynamic_lights", &Settings::dynamicLights);
            StrengthRow("lighting.projected_lights", &Settings::projectedLights, &Settings::projectedIntensity);
            StrengthRow("lighting.emissive_intensity", nullptr, &Settings::emissiveIntensity, settings.emissiveLighting != EmissiveLighting::Off,
                Text("lighting.emissive_reason"));
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##indirect", Text("lighting.section_indirect"))) {
        if (widgets::BeginRows("indirect")) {
            StrengthRow("lighting.indirect_light", &Settings::indirectLight, &Settings::indirectIntensity);
            // Traced indirect light holds its own occlusion.
            StrengthRow("lighting.ambient_occlusion", &Settings::ambientOcclusion, &Settings::aoIntensity, settings.indirectLighting == IndirectLighting::Baked,
                Text("lighting.ambient_occlusion_reason"));
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##reflections", Text("lighting.section_reflections"))) {
        if (widgets::BeginRows("reflections")) {
            StrengthRow("lighting.reflections", &Settings::reflections, &Settings::reflectionIntensity);
            SwitchRow("lighting.glossy_reflections", &Settings::glossyReflections, settings.reflections, Text("lighting.glossy_reflections_reason"));
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##fog", Text("lighting.section_fog"))) {
        if (widgets::BeginRows("fog")) {
            StrengthRow("lighting.volumetric_light", &Settings::volumetricLight, &Settings::volumetricIntensity);
            SwitchRow("lighting.fog", &Settings::fog);
            OverrideRow("lighting.fog_override", &Settings::fogOverride, true, nullptr, [&](Settings& edited) {
                if (!statistics.gameFogKnown)
                    return;
                edited.fogColor = statistics.gameFogColor;
                const SettingRange start = RangeOf(&Settings::fogStart);
                const SettingRange end = RangeOf(&Settings::fogEnd);
                edited.fogStart = std::clamp(std::round(statistics.gameFogStart), start.minimum, start.maximum);
                edited.fogEnd = std::clamp(std::round(statistics.gameFogEnd), end.minimum, end.maximum);
                edited.fogMaxDensity = std::clamp(statistics.gameFogMaxDensity, 0.0f, 1.0f);
            });
            const bool fog = settings.fogOverride;
            const char* fogReason = Text("lighting.fog_override_reason");
            ColorRow("lighting.fog_color", &Settings::fogColor, false, fog, fogReason);
            SliderRow("lighting.fog_start", &Settings::fogStart, fog, fogReason, 0);
            SliderRow("lighting.fog_end", &Settings::fogEnd, fog, fogReason, 0);
            SliderRow("lighting.fog_max_density", &Settings::fogMaxDensity, fog, fogReason);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##surfaces", Text("lighting.section_surfaces"))) {
        if (widgets::BeginRows("surfaces")) {
            SwitchRow("lighting.translucent_surfaces", &Settings::translucentLighting);
            SwitchRow("lighting.render_targets", &Settings::renderTargetLighting);
            widgets::EndRows();
        }
    }
    widgets::EndCard();
}

void DrawSkyPage(Settings& settings)
{
    if (widgets::BeginCard("##atmosphere", Text("lighting.section_atmosphere"))) {
        if (widgets::BeginRows("atmosphere")) {
            SwitchRow("lighting.physical_sky", &Settings::physicalSky);
            const char* const physicalSkyReason = Text("lighting.physical_sky_reason");
            constexpr const char* kMapSuns[] = {"lighting.map_sun_auto", "lighting.map_sun_sun", "lighting.map_sun_moon"};
            static_assert(std::size(kMapSuns) == static_cast<size_t>(MapSun::Count));
            SegmentRow("lighting.map_sun", &Settings::mapSun, kMapSuns, {}, settings.physicalSky || settings.sunDisc, Text("lighting.map_sun_reason"));
            SliderRow("lighting.moonlit_night", &Settings::moonlitNight, settings.physicalSky, physicalSkyReason);
            SliderRow("lighting.moonless_night", &Settings::moonlessNight, settings.physicalSky, physicalSkyReason, 3);
            SwitchRow("lighting.sun_disc", &Settings::sunDisc);
            SliderRow("lighting.sun_size", &Settings::sunSize, settings.sunDisc, Text("lighting.sun_size_reason"));
            SliderRow("lighting.sun_glow", &Settings::sunGlow, settings.physicalSky, physicalSkyReason);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##night", Text("lighting.section_night_sky"))) {
        if (widgets::BeginRows("night")) {
            const bool sky = settings.physicalSky;
            const char* const skyReason = Text("lighting.physical_sky_reason");
            const bool stars = sky && settings.stars;
            const char* const starsReason = sky ? Text("lighting.stars_reason") : skyReason;
            SwitchRow("lighting.stars", &Settings::stars, sky, skyReason);
            SliderRow("lighting.star_amount", &Settings::starAmount, stars, starsReason);
            SliderRow("lighting.faintest_stars", &Settings::faintestStars, stars, starsReason);
            SliderRow("lighting.star_brightness", &Settings::starBrightness, stars, starsReason);
            SliderRow("lighting.earlier_stars", &Settings::earlierStars, sky, skyReason);
            StrengthRow("lighting.twinkling", &Settings::twinkling, &Settings::twinkleStrength, stars, starsReason);
            SliderRow("lighting.twinkle_speed", &Settings::twinkleSpeed, stars && settings.twinkling, stars ? Text("lighting.twinkling_reason") : starsReason);
            StrengthRow("lighting.milky_way", &Settings::milkyWay, &Settings::milkyWayBrightness, sky, skyReason);
            SwitchRow("lighting.shooting_stars", &Settings::shootingStars, sky, skyReason);
            SliderRow("lighting.shooting_star_frequency", &Settings::shootingStarFrequency, sky && settings.shootingStars,
                sky ? Text("lighting.shooting_stars_reason") : skyReason);
            SwitchRow("lighting.satellites", &Settings::satellites, sky, skyReason);
            SwitchRow("lighting.moon", &Settings::moon, sky, skyReason);
            SliderRow("lighting.moon_phase", &Settings::moonPhase, sky && settings.moon, sky ? Text("lighting.moon_reason") : skyReason);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    const bool sky = settings.physicalSky;
    const char* const skyReason = Text("lighting.physical_sky_reason");
    const bool clouds = sky && settings.clouds;
    const char* const cloudsReason = sky ? Text("lighting.clouds_reason") : skyReason;
    // While the weather changes on its own it sets the settings a weather decides.
    const bool changing = clouds && settings.changingWeather;
    const bool chosen = clouds && !settings.changingWeather;
    const char* const chosenReason = clouds ? Text("lighting.weather_reason") : cloudsReason;
    constexpr const char* kWeathers[] = {"lighting.weather_clear", "lighting.weather_fair", "lighting.weather_scattered", "lighting.weather_broken",
        "lighting.weather_overcast", "lighting.weather_mackerel", "lighting.weather_cirrus", "lighting.weather_towering", "lighting.weather_custom"};
    static_assert(std::size(kWeathers) == static_cast<size_t>(Weather::Count));
    if (widgets::BeginCard("##clouds", Text("lighting.section_clouds"))) {
        if (widgets::BeginRows("clouds")) {
            SwitchRow("lighting.clouds", &Settings::clouds, sky, skyReason);
            SwitchRow("lighting.changing_weather", &Settings::changingWeather, clouds, cloudsReason);
            SliderRow("lighting.weather_duration", &Settings::weatherDuration, changing, clouds ? Text("lighting.weather_duration_reason") : cloudsReason, 0);
            if (changing) {
                const Statistics& statistics = GetStatistics();
                const char* const now = Text(kWeathers[static_cast<size_t>(statistics.weather)]);
                ValueRow("lighting.weather_now", statistics.nextWeather == statistics.weather ? std::string(now) :
                    ui::Format("lighting.weather_turning", {{"from", now}, {"to", Text(kWeathers[static_cast<size_t>(statistics.nextWeather)])}}));
            }
            const Weather weather = settings.weather;
            ComboRow("lighting.weather", &Settings::weather, kWeathers, chosen, chosenReason);
            if (settings.weather != weather)
                ApplyWeather(settings, settings.weather);
            SliderRow("lighting.cloud_coverage", &Settings::cloudCoverage, chosen, chosenReason);
            SliderRow("lighting.cloud_type", &Settings::cloudType, chosen, chosenReason);
            SliderRow("lighting.cloud_altitude", &Settings::cloudAltitude, chosen, chosenReason, 0);
            SliderRow("lighting.cloud_thickness", &Settings::cloudThickness, chosen, chosenReason, 0);
            SliderRow("lighting.cloud_density", &Settings::cloudDensity, clouds, cloudsReason);
            SliderRow("lighting.cloud_size", &Settings::cloudSize, clouds, cloudsReason);
            SliderRow("lighting.cloud_detail", &Settings::cloudDetail, clouds, cloudsReason);
            SliderRow("lighting.cloud_droplet_size", &Settings::cloudDropletSize, clouds, cloudsReason, 1);
            SliderRow("lighting.cloud_multiple_scattering", &Settings::cloudMultipleScattering, clouds, cloudsReason);
            StrengthRow("lighting.cloud_shadows", &Settings::cloudShadows, &Settings::cloudShadowStrength, clouds, cloudsReason);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##wind", Text("lighting.section_wind"))) {
        if (widgets::BeginRows("wind")) {
            SliderRow("lighting.wind_speed", &Settings::windSpeed, clouds, cloudsReason, 1);
            SliderRow("lighting.wind_direction", &Settings::windDirection, clouds, cloudsReason, 0);
            SliderRow("lighting.cloud_evolution", &Settings::cloudEvolution, clouds, cloudsReason);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##higher_clouds", Text("lighting.section_higher_clouds"))) {
        if (widgets::BeginRows("higher_clouds")) {
            SwitchRow("lighting.mid_clouds", &Settings::midClouds, clouds, cloudsReason);
            const bool middle = clouds && settings.midClouds;
            const char* const middleReason = clouds ? Text("lighting.mid_clouds_reason") : cloudsReason;
            SliderRow("lighting.mid_cloud_coverage", &Settings::midCloudCoverage, middle && chosen, middle ? chosenReason : middleReason);
            SliderRow("lighting.mid_cloud_altitude", &Settings::midCloudAltitude, middle, middleReason, 0);
            SliderRow("lighting.mid_cloud_thickness", &Settings::midCloudThickness, middle, middleReason, 0);
            SwitchRow("lighting.high_clouds", &Settings::highClouds, clouds, cloudsReason);
            const bool high = clouds && settings.highClouds;
            const char* const highReason = clouds ? Text("lighting.high_clouds_reason") : cloudsReason;
            SliderRow("lighting.high_cloud_coverage", &Settings::highCloudCoverage, high && chosen, high ? chosenReason : highReason);
            SliderRow("lighting.high_cloud_altitude", &Settings::highCloudAltitude, high, highReason, 0);
            SliderRow("lighting.high_cloud_opacity", &Settings::highCloudOpacity, high && chosen, high ? chosenReason : highReason);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    // A weather sets its cloud settings, so changing one of them makes it Custom.
    if (settings.weather != Weather::Custom) {
        Settings picked = settings;
        ApplyWeather(picked, settings.weather);
        if (picked != settings)
            settings.weather = Weather::Custom;
    }
}

// The game's own detail settings (the Advanced video options), applied as the game's Apply button does.
void DrawGameDetailCard(const Settings& settings)
{
    const Statistics& statistics = GetStatistics();
    if (widgets::BeginCard("##game_detail", Text("graphics.section_game"), Text("graphics.game_note"))) {
        if (widgets::BeginRows("game_detail")) {
            constexpr const char* kThreeLevels[] = {"graphics.level_low", "graphics.level_medium", "graphics.level_high"};
            constexpr const char* kFourLevels[] = {"graphics.level_low", "graphics.level_medium", "graphics.level_high", "graphics.level_very_high"};
            constexpr const char* kTwoLevels[] = {"graphics.level_low", "graphics.level_high"};
            // The game's OptionsSubVideo maps these: r_rootlod 2..0, mat_picmip 2..-1, mat_reducefillrate 1/0.
            GameChoiceRow("graphics.model_detail", kThreeLevels, 2 - std::clamp(VarInt("r_rootlod", 0), 0, 2),
                [](int picked) { WriteVar("r_rootlod", std::to_string(2 - picked), true); });
            GameChoiceRow("graphics.texture_detail", kFourLevels, 2 - std::clamp(VarInt("mat_picmip", 0), -1, 2),
                [](int picked) { WriteVar("mat_picmip", std::to_string(2 - picked), true); });
            GameChoiceRow("graphics.shader_detail", kTwoLevels, VarInt("mat_reducefillrate", 0) != 0 ? 0 : 1,
                [](int picked) { WriteVar("mat_reducefillrate", picked == 0 ? "1" : "0", true); });
            // Real-time lighting ray traces water's reflections, the flashlight's shadows and the projected prop shadows, and
            // skips the game's passes for them.
            const bool traced = settings.realtimeLighting && statistics.realtimeBlock != RealtimeBlock::Unsupported;
            constexpr const char* kWater[] = {"graphics.water_simple", "graphics.water_world", "graphics.water_all"};
            const int water = VarInt("r_waterforceexpensive", 0) == 0 ? 0 : VarInt("r_waterforcereflectentities", 0) != 0 ? 2 : 1;
            GameChoiceRow("graphics.water_detail", kWater, water, [](int picked) {
                WriteVar("r_waterforceexpensive", picked > 0 ? "1" : "0", false);
                WriteVar("r_waterforcereflectentities", picked > 1 ? "1" : "0", true);
            }, !traced, Text("graphics.water_detail_reason"));
            const int shadows = VarInt("r_flashlightdepthtexture", 0) != 0 ? 2 : VarInt("r_shadowrendertotexture", 0) != 0 ? 1 : 0;
            GameChoiceRow("graphics.shadow_detail", kThreeLevels, shadows, [](int picked) {
                WriteVar("r_shadowrendertotexture", picked > 0 ? "1" : "0", false);
                WriteVar("r_flashlightdepthtexture", picked > 1 ? "1" : "0", true);
            }, !traced, Text("graphics.shadow_detail_reason"));
            constexpr const char* kFiltering[] = {"graphics.filter_bilinear", "graphics.filter_trilinear", "graphics.filter_aniso_2", "graphics.filter_aniso_4",
                "graphics.filter_aniso_8", "graphics.filter_aniso_16"};
            const int aniso = VarInt("mat_forceaniso", 1);
            const int filtering = aniso >= 16 ? 5 : aniso >= 8 ? 4 : aniso >= 4 ? 3 : aniso >= 2 ? 2 : VarInt("mat_trilinear", 0) != 0 ? 1 : 0;
            GameChoiceRow("graphics.filtering", kFiltering, filtering, [](int picked) {
                constexpr const char* kAniso[] = {"1", "1", "2", "4", "8", "16"};
                WriteVar("mat_trilinear", picked == 1 ? "1" : "0", false);
                WriteVar("mat_forceaniso", kAniso[picked], true);
            });
            GameSwitchRow("graphics.color_correction", "mat_colorcorrection", true);
            GameSwitchRow("graphics.game_motion_blur", "mat_motion_blur_enabled", true, !settings.motionBlur, Text("graphics.game_motion_blur_reason"));
            widgets::EndRows();
        }
    }
    widgets::EndCard();
}

// The ray tracing settings on this page are set by the quality preset, so changing one makes the preset Custom.
void DrawGraphicsPage(Settings& settings)
{
    const Statistics& statistics = GetStatistics();
    if (widgets::BeginCard("##preset", Text("quality.section_preset"), Text("quality.preset_note"))) {
        if (widgets::BeginRows("preset")) {
            constexpr const char* kPresets[] = {"quality.preset_low", "quality.preset_medium", "quality.preset_high", "quality.preset_ultra", "quality.preset_custom"};
            static_assert(std::size(kPresets) == static_cast<size_t>(QualityPreset::Count));
            // The preset row stands for the whole page: its reset puts all of it back.
            const std::vector<FieldId> page = FieldsIn(SectionBit(Section::Quality));
            if (SegmentRow("quality.preset", &Settings::qualityPreset, kPresets, {}, true, nullptr, page))
                ApplyPreset(settings, settings.qualityPreset);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##shadows", Text("quality.section_shadows"))) {
        if (widgets::BeginRows("shadows")) {
            constexpr const char* kSampling[] = {"quality.sampling_legacy", "quality.sampling_ris", "quality.sampling_restir", "quality.sampling_auto"};
            static_assert(std::size(kSampling) == static_cast<size_t>(LightSampling::Count));
            ComboRow("quality.light_sampling", &Settings::lightSampling, kSampling);
            // The direct light pass that uses these runs for Legacy, which Auto picks with DLSS Ray Reconstruction
            // (Lighting::Run).
            const bool directPass = settings.lightSampling == LightSampling::Legacy ||
                (settings.lightSampling == LightSampling::Auto && settings.antiAliasing == AntiAliasing::RayReconstruction &&
                    GetStatistics().rayReconstructionAvailable);
            const char* directReason = Text("quality.direct_pass_reason");
            SliderRow("quality.exact_lights", &Settings::exactLights, directPass, directReason);
            SliderRow("quality.sampled_light_rays", &Settings::sampledLightRays, directPass, directReason);
            SliderRow("quality.viewmodel_shadow_rays", &Settings::viewmodelShadowRays);
            constexpr const char* kFoliage[] = {"quality.foliage_off", "quality.foliage_sun", "quality.foliage_sun_indirect", "quality.foliage_all"};
            static_assert(std::size(kFoliage) == static_cast<size_t>(FoliageShadows::Count));
            ComboRow("quality.foliage_shadows", &Settings::foliageShadows, kFoliage);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##indirect", Text("quality.section_indirect"))) {
        const bool cacheAvailable = statistics.radianceCacheAvailable;
        if (widgets::BeginRows("indirect")) {
            constexpr const char* kSources[] = {"quality.indirect_ray_traced", "quality.indirect_baked"};
            static_assert(std::size(kSources) == static_cast<size_t>(IndirectLighting::Count));
            SegmentRow("quality.indirect_source", &Settings::indirectLighting, kSources);
            const bool baked = settings.indirectLighting == IndirectLighting::Baked;
            SliderRow("quality.indirect_rays", &Settings::indirectRays);
            SliderRow("quality.indirect_bounces", &Settings::indirectBounces);
            SwitchRow("quality.radiance_cache", &Settings::radianceCache, cacheAvailable && !baked,
                Text(cacheAvailable ? "quality.radiance_cache_reason" : "quality.radiance_cache_unavailable"));
            SliderRow("quality.volume_rays", &Settings::volumeRays);
            SliderRow("quality.ao_rays", &Settings::aoRays, baked, Text("quality.ao_rays_reason"));
            widgets::EndRows();
        }
        if (!cacheAvailable)
            widgets::Note(Text("quality.radiance_cache_unavailable"));
    }
    widgets::EndCard();

    if (widgets::BeginCard("##effects", Text("quality.section_effects"))) {
        if (widgets::BeginRows("effects")) {
            constexpr const char* kEmissive[] = {"quality.emissive_off", "quality.emissive_bounce", "quality.emissive_full"};
            static_assert(std::size(kEmissive) == static_cast<size_t>(EmissiveLighting::Count));
            SegmentRow("quality.emissive", &Settings::emissiveLighting, kEmissive);
            SliderRow("quality.volumetric_slices", &Settings::volumetricSlices, settings.volumetricLight, Text("quality.volumetric_slices_reason"));
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##alpha", Text("quality.section_alpha"))) {
        const bool micromapsAvailable = statistics.opacityMicromapsAvailable;
        if (widgets::BeginRows("alpha")) {
            SwitchRow("quality.opacity_micromaps", &Settings::opacityMicromaps, micromapsAvailable, Text("quality.micromaps_unavailable"));
            SwitchRow("quality.fast_alpha_tests", &Settings::fastAlphaTests, micromapsAvailable && settings.opacityMicromaps,
                Text(micromapsAvailable ? "quality.fast_alpha_tests_reason" : "quality.micromaps_unavailable"));
            widgets::EndRows();
        }
        if (!micromapsAvailable)
            widgets::Note(Text("quality.micromaps_unavailable"));
    }
    widgets::EndCard();

    if (widgets::BeginCard("##clouds", Text("quality.section_clouds"))) {
        if (widgets::BeginRows("clouds")) {
            const bool clouds = settings.physicalSky && settings.clouds;
            const char* const cloudsReason = Text("quality.clouds_reason");
            constexpr const char* kResolutions[] = {"quality.cloud_resolution_quarter", "quality.cloud_resolution_half", "quality.cloud_resolution_full"};
            static_assert(std::size(kResolutions) == static_cast<size_t>(CloudResolution::Count));
            SegmentRow("quality.cloud_resolution", &Settings::cloudResolution, kResolutions, {}, clouds, cloudsReason);
            SliderRow("quality.cloud_steps", &Settings::cloudSteps, clouds, cloudsReason);
            SliderRow("quality.cloud_light_steps", &Settings::cloudLightSteps, clouds, cloudsReason);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (settings.qualityPreset != QualityPreset::Custom) {
        Settings preset = settings;
        ApplyPreset(preset, settings.qualityPreset);
        if (preset != settings)
            settings.qualityPreset = QualityPreset::Custom;
    }

    DrawGameDetailCard(settings);
}

void DrawImagePage(Settings& settings)
{
    const Statistics& statistics = GetStatistics();
    const bool rayReconstruction = settings.antiAliasing == AntiAliasing::RayReconstruction;
    if (widgets::BeginCard("##antialiasing", Text("image.section_anti_aliasing"))) {
        if (widgets::BeginRows("antialiasing")) {
            constexpr const char* kModes[] = {"image.anti_aliasing_off", "image.anti_aliasing_taa", "image.anti_aliasing_dlss", "image.anti_aliasing_fsr"};
            static_assert(std::size(kModes) == static_cast<size_t>(AntiAliasing::Count));
            const bool available[] = {true, true, statistics.rayReconstructionAvailable, statistics.fsrAvailable};
            SegmentRow("image.anti_aliasing", &Settings::antiAliasing, kModes, available);
            constexpr const char* kUpscaling[] = {"image.upscaling_native", "image.upscaling_quality", "image.upscaling_balanced", "image.upscaling_performance",
                "image.upscaling_ultra_performance"};
            static_assert(std::size(kUpscaling) == static_cast<size_t>(UpscalingMode::Count));
            const bool upscaling = rayReconstruction || settings.antiAliasing == AntiAliasing::Fsr;
            ComboRow("image.upscaling", &Settings::upscaling, kUpscaling, upscaling, Text("image.upscaling_reason"));
            const bool temporal = settings.antiAliasing == AntiAliasing::Temporal;
            SliderRow("image.history_weight", &Settings::taaHistoryWeight, temporal, Text("image.taa_reason"));
            SliderRow("image.sharpening", &Settings::sharpening, temporal, Text("image.taa_reason"));
            ValueRow("image.render_resolution",
                statistics.renderWidth != 0 ? ui::Format("image.render_resolution_value",
                                                  {{"width", std::to_string(statistics.renderWidth)}, {"height", std::to_string(statistics.renderHeight)}})
                                            : std::string(Text("image.render_resolution_unknown")));
            widgets::EndRows();
        }
        if (!statistics.rayReconstructionAvailable)
            widgets::Note(Text("image.dlss_unavailable"));
        if (!statistics.fsrAvailable)
            widgets::Note(Text("image.fsr_unavailable"));
    }
    widgets::EndCard();

    if (widgets::BeginCard("##latency", Text("image.section_latency"), Text("image.frame_generation_note"))) {
        if (widgets::BeginRows("latency")) {
            constexpr const char* kGeneration[] = {"image.frame_generation_off", "image.frame_generation_dlss", "image.frame_generation_fsr"};
            static_assert(std::size(kGeneration) == static_cast<size_t>(FrameGeneration::Count));
            // Streamline is not loaded when the game starts with FSR's swap chain, so DLSS's support is then unknown.
            const bool available[] = {true, statistics.dlssFrameGenerationAvailable || statistics.startedFrameGeneration == FrameGeneration::Fsr,
                statistics.fsrFrameGenerationAvailable};
            SegmentRow("image.frame_generation", &Settings::frameGeneration, kGeneration, available);
            SliderRow("image.generated_frames", &Settings::generatedFrames,
                settings.frameGeneration == FrameGeneration::Dlss && statistics.generatedFramesMax > 1, Text("image.generated_frames_reason"));
            constexpr const char* kReflex[] = {"image.reflex_off", "image.reflex_on", "image.reflex_boost"};
            static_assert(std::size(kReflex) == static_cast<size_t>(ReflexMode::Count));
            ComboRow("image.reflex", &Settings::reflex, kReflex, statistics.reflexAvailable, Text("image.reflex_reason"));
            SwitchRow("image.anti_lag", &Settings::antiLag, statistics.antiLagAvailable, Text("image.anti_lag_reason"));
            widgets::EndRows();
        }
        const bool restart = (settings.frameGeneration == FrameGeneration::Fsr) != (statistics.startedFrameGeneration == FrameGeneration::Fsr);
        if (restart)
            widgets::Note(Text("image.frame_generation_restart"));
    }
    widgets::EndCard();

    if (widgets::BeginCard("##denoiser", Text("image.section_denoiser"))) {
        if (widgets::BeginRows("denoiser")) {
            SwitchRow("image.denoiser", &Settings::denoiser);
            constexpr const char* kDenoisers[] = {"image.denoiser_built_in", "image.denoiser_relax", "image.denoiser_reblur", "image.denoiser_ray_regeneration"};
            static_assert(std::size(kDenoisers) == static_cast<size_t>(DenoiserType::Count));
            // DLSS Ray Reconstruction denoises by itself.
            ComboRow("image.denoiser_type", &Settings::denoiserType, kDenoisers, settings.denoiser && !rayReconstruction,
                Text(rayReconstruction ? "image.denoiser_type_dlss_reason" : "image.denoiser_type_reason"));
            widgets::EndRows();
        }
        if (settings.denoiserType == DenoiserType::RayRegeneration && !statistics.rayRegenerationAvailable)
            widgets::Note(Text("image.ray_regeneration_unavailable"));
    }
    widgets::EndCard();

    if (widgets::BeginCard("##tone", Text("image.section_tone"), Text("image.tone_note"))) {
        if (widgets::BeginRows("tone")) {
            constexpr const char* kCurves[] = {"image.tone_curve_off", "image.tone_curve_highlights", "image.tone_curve_filmic"};
            static_assert(std::size(kCurves) == static_cast<size_t>(ToneCurve::Count));
            const bool realtime = settings.realtimeLighting;
            SegmentRow("image.tone_curve", &Settings::toneCurve, kCurves, {}, realtime, Text("image.tone_reason"));
            SliderRow("image.highlight_rolloff", &Settings::highlightRolloff, realtime && settings.toneCurve == ToneCurve::Highlights,
                Text(realtime ? "image.highlight_rolloff_reason" : "image.tone_reason"));
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##grading", Text("image.section_grading"), Text("image.grading_note"))) {
        if (widgets::BeginRows("grading")) {
            SwitchRow("image.grading", &Settings::grading);
            const bool grading = settings.grading;
            const char* reason = Text("image.grading_reason");
            SliderRow("image.exposure", &Settings::exposure, grading, reason);
            SliderRow("image.contrast", &Settings::contrast, grading, reason);
            SliderRow("image.saturation", &Settings::saturation, grading, reason);
            SliderRow("image.warmth", &Settings::warmth, grading, reason);
            SliderRow("image.tint", &Settings::tint, grading, reason);
            widgets::EndRows();
        }
    }
    widgets::EndCard();
}

void DrawCameraPage(Settings& settings)
{
    // Depth of field, motion blur and lens flare take the main view's camera, motion and the sun's visibility from the
    // real-time lighting; the lens effects work without it.
    const bool lit = settings.realtimeLighting && GetStatistics().realtimeBlock != RealtimeBlock::Unsupported;
    const char* litReason = Text("camera.realtime_reason");

    if (widgets::BeginCard("##dof", Text("camera.section_dof"), Text("camera.dof_note"))) {
        if (widgets::BeginRows("dof")) {
            SwitchRow("camera.depth_of_field", &Settings::depthOfField, lit, litReason);
            const bool dof = lit && settings.depthOfField;
            const char* reason = lit ? Text("camera.dof_reason") : litReason;
            constexpr const char* kFocus[] = {"camera.dof_focus_auto", "camera.dof_focus_manual"};
            static_assert(std::size(kFocus) == static_cast<size_t>(DofFocus::Count));
            SegmentRow("camera.dof_focus", &Settings::dofFocus, kFocus, {}, dof, reason);
            const bool manual = settings.dofFocus == DofFocus::Manual;
            SliderRow("camera.dof_focus_distance", &Settings::dofFocusDistance, dof && manual, dof ? Text("camera.dof_manual_reason") : reason, 0);
            SliderRow("camera.dof_focus_time", &Settings::dofFocusTime, dof && !manual, dof ? Text("camera.dof_auto_reason") : reason);
            SliderRow("camera.dof_aperture", &Settings::dofAperture, dof, reason, 1);
            SliderRow("camera.dof_sensor_size", &Settings::dofSensorSize, dof, reason);
            SliderRow("camera.dof_max_blur", &Settings::dofMaxBlur, dof, reason, 3);
            SliderRow("camera.dof_rings", &Settings::dofRings, dof, reason);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##motion_blur", Text("camera.section_motion_blur"), Text("camera.motion_blur_note"))) {
        if (widgets::BeginRows("motion_blur")) {
            const bool wasBlurring = settings.motionBlur;
            SwitchRow("camera.motion_blur", &Settings::motionBlur, lit, litReason);
            if (!wasBlurring && settings.motionBlur && VarInt("mat_motion_blur_enabled", 0) != 0)
                WriteVar("mat_motion_blur_enabled", "0", true);
            SliderRow("camera.motion_blur_shutter", &Settings::motionBlurShutter, lit && settings.motionBlur, lit ? Text("camera.motion_blur_reason") : litReason,
                0);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##lens_flare", Text("camera.section_lens_flare"), Text("camera.lens_flare_note"))) {
        if (widgets::BeginRows("lens_flare")) {
            StrengthRow("camera.lens_flare", &Settings::lensFlare, &Settings::lensFlareIntensity, lit, litReason);
            SliderRow("camera.lens_flare_starburst", &Settings::lensFlareStarburst, lit && settings.lensFlare,
                lit ? Text("camera.lens_flare_reason") : litReason);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##lens", Text("camera.section_lens"), Text("camera.lens_note"))) {
        if (widgets::BeginRows("lens")) {
            StrengthRow("camera.chromatic_aberration", &Settings::chromaticAberration, &Settings::chromaticAberrationStrength);
            StrengthRow("camera.lens_distortion", &Settings::lensDistortion, &Settings::lensDistortionAmount);
            StrengthRow("camera.vignette", &Settings::vignette, &Settings::vignetteAmount);
            StrengthRow("camera.film_grain", &Settings::filmGrain, &Settings::grainAmount);
            SliderRow("camera.grain_size", &Settings::grainSize, settings.filmGrain, Text("camera.grain_reason"));
            StrengthRow("camera.sharpening", &Settings::contrastSharpening, &Settings::contrastSharpness);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##exposure", Text("camera.section_exposure"), Text("camera.exposure_note"))) {
        if (widgets::BeginRows("exposure")) {
            SwitchRow("camera.exposure_adaptation", &Settings::exposureAdaptation);
            const bool exposure = settings.exposureAdaptation;
            const char* reason = Text("camera.exposure_reason");
            SliderRow("camera.exposure_bias", &Settings::exposureBias, exposure, reason);
            SliderRow("camera.exposure_minimum", &Settings::exposureMinimum, exposure, reason);
            SliderRow("camera.exposure_maximum", &Settings::exposureMaximum, exposure, reason);
            SliderRow("camera.exposure_time", &Settings::exposureTime, exposure, reason);
            widgets::EndRows();
        }
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

// Frame rate figures over the recorded history of frames shown on screen, generated ones included. Lows average the
// slowest 1% / 0.1% of them; "now" the last 30 rendered frames.
struct FrameRates {
    std::vector<float> chronological;
    std::vector<float> sorted;
    double now = 0.0;
    double average = 0.0;
    double median = 0.0;
    double low1 = 0.0;
    double low01 = 0.0;
    double averageMs = 0.0;
    double nowMs = 0.0;
    double p99Ms = 0.0;
    // The rendered frame rate "now", while frame generation adds frames.
    std::optional<double> rendered;
};

std::optional<FrameRates> MeasureFrames(const Statistics& statistics)
{
    constexpr size_t kRecent = 30;
    const size_t frames = statistics.frameCount;
    const size_t first = frames == Statistics::kFrameHistory ? statistics.nextFrame : 0;
    auto slot = [&](size_t i) { return (first + i) % Statistics::kFrameHistory; };
    FrameRates rates;
    // A rendered frame's time is split evenly over the frames shown for it. Frame generation paces its presents on its
    // own, so the count one present reports can differ from the next; the mean over the last 30 rendered frames gives
    // the split.
    uint32_t windowShown = 0;
    for (size_t i = 0; i < frames; ++i) {
        windowShown += statistics.shownFrames[slot(i)];
        if (i >= kRecent)
            windowShown -= statistics.shownFrames[slot(i - kRecent)];
        if (windowShown == 0)
            continue;
        const float ms = statistics.frameMs[slot(i)] * static_cast<float>(std::min(i + 1, kRecent)) / static_cast<float>(windowShown);
        rates.chronological.insert(rates.chronological.end(), statistics.shownFrames[slot(i)], ms);
    }
    const size_t count = rates.chronological.size();
    if (count == 0)
        return std::nullopt;
    rates.sorted = rates.chronological;
    std::sort(rates.sorted.begin(), rates.sorted.end());
    double total = 0.0;
    for (float ms : rates.sorted)
        total += ms;
    auto slowestAverage = [&](double fraction) {
        const size_t n = std::max<size_t>(1, static_cast<size_t>(static_cast<double>(count) * fraction));
        double sum = 0.0;
        for (size_t i = count - n; i < count; ++i)
            sum += rates.sorted[i];
        return sum / static_cast<double>(n);
    };
    auto fps = [](double ms) { return ms > 0.0 ? 1000.0 / ms : 0.0; };
    const size_t recent = std::min(frames, kRecent);
    double recentMs = 0.0;
    uint32_t recentShown = 0;
    for (size_t i = frames - recent; i < frames; ++i) {
        recentMs += statistics.frameMs[slot(i)];
        recentShown += statistics.shownFrames[slot(i)];
    }
    rates.averageMs = total / static_cast<double>(count);
    rates.nowMs = recentShown > 0 ? recentMs / static_cast<double>(recentShown) : 0.0;
    rates.p99Ms = rates.sorted[std::min(count - 1, count * 99 / 100)];
    rates.now = fps(rates.nowMs);
    rates.average = fps(rates.averageMs);
    rates.median = fps(rates.sorted[count / 2]);
    rates.low1 = fps(slowestAverage(0.01));
    rates.low01 = fps(slowestAverage(0.001));
    if (recentShown != recent)
        rates.rendered = fps(recentMs / static_cast<double>(recent));
    return rates;
}

void DrawFrameRate(const Statistics& statistics)
{
    const std::optional<FrameRates> rates = MeasureFrames(statistics);
    if (!rates)
        return;
    widgets::Heading(Text("performance.section_frame_rate"));
    constexpr const char* kLabels[] = {"performance.fps_now", "performance.fps_average", "performance.fps_median", "performance.fps_low_1",
        "performance.fps_low_01"};
    const double figures[] = {rates->now, rates->average, rates->median, rates->low1, rates->low01};
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = (ImGui::GetContentRegionAvail().x - spacing * static_cast<float>(std::size(kLabels) - 1)) / static_cast<float>(std::size(kLabels));
    for (size_t n = 0; n < std::size(kLabels); ++n) {
        const size_t i = widgets::RightToLeft() ? std::size(kLabels) - 1 - n : n;
        if (n > 0)
            ImGui::SameLine();
        widgets::Tile(ui::Number(figures[i], 0).c_str(), Text(kLabels[i]), width);
    }
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

    if (widgets::BeginCard("##frametime", Text("performance.section_frame_time"))) {
        widgets::Text(ui::Format("performance.frame_time_summary", {{"average", ui::Number(rates->averageMs, 2)}, {"p99", ui::Number(rates->p99Ms, 2)}}).c_str());
        ImGui::PlotLines("##frametimes", rates->chronological.data(), static_cast<int>(rates->chronological.size()), 0, nullptr, 0.0f,
            std::max(rates->sorted.back(), 1.0f), ImVec2(-FLT_MIN, ImGui::GetFontSize() * 4.0f));
        widgets::Note(
            ui::Format("performance.frame_time_range", {{"min", ui::Number(rates->sorted.front(), 2)}, {"max", ui::Number(rates->sorted.back(), 2)}}).c_str());
    }
    widgets::EndCard();
}

constexpr const char* kPasses[] = {"pass.scene_update", "pass.surface", "pass.radiance_cache", "pass.volume", "pass.direct_light", "pass.light_sampling",
    "pass.ambient_occlusion", "pass.indirect_trace", "pass.reflections", "pass.accumulation", "pass.denoiser", "pass.lighting", "pass.resolve"};
static_assert(std::size(kPasses) == static_cast<size_t>(GpuPass::Count));

void DrawPerformancePage(Settings& settings)
{
    const Statistics& statistics = GetStatistics();
    if (widgets::BeginCard("##overlay", Text("general.section_overlay"))) {
        if (widgets::BeginRows("overlay")) {
            SwitchRow("general.overlay", &Settings::performanceOverlay);
            const bool shown = settings.performanceOverlay;
            const char* reason = Text("general.overlay_reason");
            SwitchRow("general.overlay_hide_with_hud", &Settings::overlayHideWithHud, shown, reason);
            constexpr const char* kDetails[] = {"general.overlay_minimal", "general.overlay_normal", "general.overlay_full", "general.overlay_extra"};
            static_assert(std::size(kDetails) == static_cast<size_t>(OverlayDetail::Count));
            ComboRow("general.overlay_detail", &Settings::overlayDetail, kDetails, shown, reason);
            constexpr const char* kCorners[] = {"general.corner_top_left", "general.corner_top_right", "general.corner_bottom_left", "general.corner_bottom_right"};
            static_assert(std::size(kCorners) == static_cast<size_t>(OverlayCorner::Count));
            ComboRow("general.overlay_corner", &Settings::overlayCorner, kCorners, shown, reason);
            SliderRow("general.overlay_opacity", &Settings::overlayOpacity, shown, reason);
            SliderRow("general.overlay_scale", &Settings::overlayScale, shown, reason);
            SliderRow("general.overlay_margin", &Settings::overlayMargin, shown, reason);
            ColorRow("general.overlay_background", &Settings::overlayBackground, false, shown, reason);
            ColorRow("general.overlay_text", &Settings::overlayText, false, shown, reason);
            SwitchRow("general.overlay_border", &Settings::overlayBorder, shown, reason);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    DrawFrameRate(statistics);

    if (widgets::BeginCard("##passes", Text("performance.section_gpu_passes"))) {
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
    }
    widgets::EndCard();

    if (widgets::BeginCard("##statistics", Text("performance.section_statistics"))) {
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
    }
    widgets::EndCard();

    if (widgets::BeginCard("##developer", Text("general.section_developer"))) {
        if (widgets::BeginRows("developer")) {
            constexpr const char* kViews[] = {"debug.final", "debug.albedo", "debug.normals", "debug.coverage", "debug.direct_light", "debug.indirect_light",
                "debug.indirect_raw", "debug.sun_visibility", "debug.additive_light", "debug.reflections", "debug.motion_vectors", "debug.ambient_occlusion",
                "debug.radiance_cache", "debug.light_sample", "debug.reservoir_samples", "debug.denoiser_validation"};
            static_assert(std::size(kViews) == static_cast<size_t>(DebugView::Count));
            ComboRow("general.debug_view", &Settings::debugView, kViews);
            widgets::EndRows();
        }
    }
    widgets::EndCard();
}

// The sections presets and exports offer, in the order of the Toggles' bits.
constexpr Section kShareSections[] = {Section::Lighting, Section::Sky, Section::Quality, Section::Image, Section::Camera};
constexpr const char* kShareSectionNames[] = {"page.lighting", "page.sky", "page.graphics", "page.image", "page.camera"};

SectionMask SectionsOf(uint32_t toggles)
{
    SectionMask sections = 0;
    for (size_t i = 0; i < std::size(kShareSections); ++i) {
        if ((toggles & (1u << i)) != 0)
            sections |= SectionBit(kShareSections[i]);
    }
    return sections;
}

// A modal window in the middle of the screen, as wide as a short paragraph.
bool BeginDialog(const char* id)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f));
    return ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
}

// The confirm and cancel buttons at the end of a dialog, the confirm button outermost; returns 1 for confirm, -1 for
// cancel (or Escape), 0 for neither.
int DialogButtons(const char* confirm, bool canConfirm = true)
{
    ImGui::Spacing();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float confirmWidth = widgets::ButtonWidth(confirm);
    const float cancelWidth = widgets::ButtonWidth(Text("profiles.cancel"));
    const bool rightToLeft = widgets::RightToLeft();
    const float start = ImGui::GetCursorPosX();
    const float end = start + ImGui::GetContentRegionAvail().x;
    int result = 0;
    ImGui::SetCursorPosX(rightToLeft ? start + confirmWidth + spacing : end - confirmWidth - spacing - cancelWidth);
    if (widgets::Button(Text("profiles.cancel")))
        result = -1;
    ImGui::SameLine(0.0f, spacing);
    ImGui::SetCursorPosX(rightToLeft ? start : end - confirmWidth);
    ImGui::BeginDisabled(!canConfirm);
    if (widgets::Button(confirm, true))
        result = 1;
    ImGui::EndDisabled();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        result = -1;
    return result;
}

void OpenDialog(Dialog dialog, std::string preset = {})
{
    g_state.dialog = dialog;
    g_state.dialogPreset = std::move(preset);
    g_state.renameRejected = false;
    if (dialog == Dialog::RenamePreset)
        std::snprintf(g_state.renameBuffer, sizeof(g_state.renameBuffer), "%s", g_state.dialogPreset.c_str());
    g_state.dialogRequested = true;
}

void SavePreset(Scope scope)
{
    const std::string name(g_state.presetName);
    if (config::SavePreset(name, scope, SectionsOf(g_state.presetSections))) {
        SetStatus(ui::Format("status.saved", {{"name", name}}));
        g_state.presetName[0] = '\0';
    } else {
        SetStatus(Text("status.save_failed"));
    }
}

void DrawDialog(Scope scope)
{
    if (std::exchange(g_state.dialogRequested, false))
        ImGui::OpenPopup("##dialog");
    if (!BeginDialog("##dialog"))
        return;
    const std::string& preset = g_state.dialogPreset;
    int result = 0;
    switch (g_state.dialog) {
    case Dialog::DeletePreset:
        widgets::Text(ui::Format("profiles.delete_confirm", {{"name", preset}}).c_str());
        if ((result = DialogButtons(Text("profiles.delete"))) == 1)
            SetStatus(ui::Format(config::DeletePreset(preset) ? "status.deleted" : "status.delete_failed", {{"name", preset}}));
        break;
    case Dialog::OverwritePreset:
        widgets::Text(ui::Format("profiles.overwrite_confirm", {{"name", preset}}).c_str());
        if ((result = DialogButtons(Text("profiles.replace"))) == 1)
            SavePreset(scope);
        break;
    case Dialog::RenamePreset: {
        widgets::Heading(ui::Format("profiles.rename_title", {{"name", preset}}).c_str());
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool entered = widgets::TextField("##name", g_state.renameBuffer, sizeof(g_state.renameBuffer), Text("profiles.preset_name"));
        const std::string name(g_state.renameBuffer);
        const bool valid = config::ValidPresetName(name);
        if (!valid && !name.empty())
            widgets::Note(Text("profiles.name_invalid"));
        else if (g_state.renameRejected)
            widgets::Note(Text("profiles.rename_taken"));
        result = DialogButtons(Text("profiles.rename"), valid);
        if (entered && valid)
            result = 1;
        if (result == 1) {
            if (name == preset || config::RenamePreset(preset, name)) {
                SetStatus(ui::Format("status.renamed", {{"name", name}}));
            } else {
                g_state.renameRejected = true;
                result = 0;
            }
        }
        break;
    }
    case Dialog::ClearMap:
        widgets::Text(ui::Format("profiles.clear_map_confirm", {{"map", config::MapName()}}).c_str());
        if ((result = DialogButtons(Text("profiles.clear_map"))) == 1) {
            config::ResetSections(Scope::Map, ~SectionMask(0));
            SetStatus(ui::Format("status.map_cleared", {{"map", config::MapName()}}));
        }
        break;
    case Dialog::None:
        result = -1;
        break;
    }
    if (result != 0) {
        g_state.dialog = Dialog::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// A row of buttons that wraps onto the next line when it runs out of room, from the start of the line.
class ButtonFlow {
public:
    bool Add(const char* label, bool primary = false, bool enabled = true)
    {
        const float width = widgets::ButtonWidth(label);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        if (m_used > 0.0f && m_used + spacing + width <= m_lineWidth) {
            ImGui::SameLine(0.0f, spacing);
            m_used += spacing + width;
        } else {
            m_start = ImGui::GetCursorPosX();
            m_lineWidth = ImGui::GetContentRegionAvail().x;
            m_used = width;
        }
        // Right to left, the buttons run leftward from the line's right end.
        if (widgets::RightToLeft())
            ImGui::SetCursorPosX(m_start + m_lineWidth - m_used);
        ImGui::BeginDisabled(!enabled);
        const bool clicked = widgets::Button(label, primary);
        ImGui::EndDisabled();
        return clicked;
    }

private:
    float m_start = 0.0f;
    float m_lineWidth = 0.0f;
    float m_used = 0.0f;
};

void DrawProfilesPage(Settings&)
{
    const Scope scope = g_edit.scope;
    const std::string& map = config::MapName();

    if (widgets::BeginCard("##map", Text("profiles.section_map"))) {
        if (map.empty()) {
            widgets::Note(Text("profiles.map_none"));
        } else {
            widgets::Text(ui::Format("profiles.map_count", {{"map", map}, {"count", ui::Number(static_cast<double>(config::MapSetCount()), 0)}}).c_str());
            ButtonFlow buttons;
            if (buttons.Add(Text("profiles.edit_map"), true)) {
                g_state.scope = Scope::Map;
                g_state.page = Page::Lighting;
            }
            if (buttons.Add(Text("profiles.clear_map"), false, config::MapSetCount() > 0))
                OpenDialog(Dialog::ClearMap);
        }
    }
    widgets::EndCard();

    const std::string applies = scope == Scope::Map ? ui::Format("profiles.applies_map", {{"map", map}}) : std::string(Text("profiles.applies_global"));
    if (widgets::BeginCard("##presets", Text("profiles.section_presets"), applies.c_str())) {
        const std::vector<std::string> presets = config::Presets();
        if (presets.empty()) {
            widgets::Note(Text("profiles.presets_none"));
        } else if (ImGui::BeginTable("presets", 2, ImGuiTableFlags_SizingStretchProp)) {
            const char* actions[] = {Text("profiles.apply"), Text("profiles.rename"), Text("profiles.delete")};
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            float actionsWidth = spacing * 2.0f;
            for (const char* action : actions)
                actionsWidth += widgets::ButtonWidth(action);
            const bool rightToLeft = widgets::RightToLeft();
            const int nameColumn = rightToLeft ? 1 : 0;
            for (int column = 0; column < 2; ++column) {
                if (column == nameColumn)
                    ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
                else
                    ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, actionsWidth);
            }
            for (const std::string& preset : presets) {
                ImGui::PushID(preset.c_str());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(nameColumn);
                ImGui::AlignTextToFramePadding();
                widgets::Text(preset.c_str());
                ImGui::TableSetColumnIndex(1 - nameColumn);
                // Apply comes first in the line's direction.
                for (size_t n = 0; n < std::size(actions); ++n) {
                    const size_t i = rightToLeft ? std::size(actions) - 1 - n : n;
                    if (n > 0)
                        ImGui::SameLine(0.0f, spacing);
                    if (!widgets::Button(actions[i], i == 0))
                        continue;
                    if (i == 0)
                        SetStatus(ui::Format(config::LoadPreset(preset, scope) ? "status.applied" : "status.apply_failed", {{"name", preset}}));
                    else
                        OpenDialog(i == 1 ? Dialog::RenamePreset : Dialog::DeletePreset, preset);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        widgets::Heading(Text("profiles.save_heading"));
        bool entered = false;
        if (widgets::BeginRows("save")) {
            BeginRow("profiles.preset_name");
            entered = widgets::TextField("##name", g_state.presetName, sizeof(g_state.presetName), Text("profiles.preset_name_hint"));
            EndRow();
            BeginRow("profiles.sections");
            widgets::Toggles("##sections", &g_state.presetSections, Texts(kShareSectionNames));
            EndRow();
            widgets::EndRows();
        }
        const std::string name(g_state.presetName);
        const bool valid = config::ValidPresetName(name);
        if (!name.empty() && !valid)
            widgets::Note(Text("profiles.name_invalid"));
        const bool anySection = SectionsOf(g_state.presetSections) != 0;
        if (!anySection)
            widgets::Note(Text("status.nothing_selected"));
        const bool save = widgets::EndButton(Text("profiles.save")) || entered;
        if (save && valid && anySection) {
            if (std::ranges::find(presets, name) != presets.end())
                OpenDialog(Dialog::OverwritePreset, name);
            else
                SavePreset(scope);
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##share", Text("profiles.section_share"))) {
        if (widgets::BeginRows("share")) {
            BeginRow("profiles.sections");
            widgets::Toggles("##sections", &g_state.shareSections, Texts(kShareSectionNames));
            EndRow();
            widgets::EndRows();
        }
        const SectionMask shared = SectionsOf(g_state.shareSections);
        const bool idle = !g_state.fileJob;
        ButtonFlow buttons;
        if (buttons.Add(Text("profiles.copy"), false, shared != 0))
            SetStatus(Text(CopyToClipboard(config::Export(scope, shared)) ? "status.copied" : "status.copy_failed"));
        if (buttons.Add(Text("profiles.paste"))) {
            const std::optional<std::string> text = ClipboardText();
            SetStatus(Text(text && config::Import(scope, *text) ? "status.pasted" : "status.paste_empty"));
        }
        if (buttons.Add(Text("profiles.export"), false, idle && shared != 0))
            StartFileJob(FileKind::Settings, true, scope, config::Export(scope, shared));
        if (buttons.Add(Text("profiles.import"), false, idle))
            StartFileJob(FileKind::Settings, false, scope, {});
        if (!idle)
            widgets::Note(Text("profiles.waiting"));
    }
    widgets::EndCard();

    DrawDialog(scope);
}

// ---- Logs ----

std::filesystem::path LogsDirectory() { return ModDirectory() / L"logs"; }

std::string Lowercase(std::string_view text)
{
    std::string lower(text);
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

// Lines carry no level, so one is read from what they say: the debug layer's severity, or words that report a failure
// or something left out.
LogLevel Classify(std::string_view message)
{
    const std::string lower = Lowercase(message);
    // "D3D12 [n] ...": D3D12_MESSAGE_SEVERITY 0 corruption, 1 error, 2 warning.
    if (lower.starts_with("d3d12 [") && lower.size() > 7)
        return lower[7] <= '1' ? LogLevel::Error : lower[7] == '2' ? LogLevel::Warning : LogLevel::Info;
    for (const char* word : {"failed", "failure", "error", "cannot", "could not", "couldn't", "can't", "was lost", "crash", "exception", "corrupt"}) {
        if (lower.find(word) != std::string::npos)
            return LogLevel::Error;
    }
    for (const char* word : {"warn", "not available", "unavailable", "not supported", "unsupported", "not implemented", "not found", "missing", "skipp",
             "ignor", "instead", "left unchanged", "fallback", "fell back", "no text for", "not the expected"}) {
        if (lower.find(word) != std::string::npos)
            return LogLevel::Warning;
    }
    return LogLevel::Info;
}

// How far a "Module:" prefix reaches into a message (past its colon), or 0 without one.
uint32_t PrefixLength(std::string_view message)
{
    const size_t colon = message.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 40 || colon + 1 >= message.size() || message[colon + 1] != ' ')
        return 0;
    for (const char c : message.substr(0, colon)) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '+' || c == '-' || c == '/' || c == '_'))
            return 0;
    }
    return static_cast<uint32_t>(colon + 1);
}

LogEntry MakeEntry(std::string raw)
{
    LogEntry entry;
    // Lines the log writes start "HH:MM:SS.mmm [thread] ".
    if (raw.size() > 14 && raw[2] == ':' && raw[5] == ':' && raw[8] == '.' && raw[12] == ' ' && raw[13] == '[') {
        const size_t close = raw.find("] ", 14);
        if (close != std::string::npos)
            entry.message = static_cast<uint32_t>(close + 2);
    }
    const std::string_view message = std::string_view(raw).substr(entry.message);
    entry.level = Classify(message);
    entry.prefixEnd = PrefixLength(message);
    entry.width = ImGui::CalcTextSize(message.data(), message.data() + message.size()).x;
    entry.raw = std::move(raw);
    return entry;
}

void AddEntry(LogView& logs, LogEntry entry)
{
    logs.widest = std::max(logs.widest, entry.width);
    logs.entries.push_back(std::move(entry));
    logs.selected.push_back(false);
    logs.grew = true;
}

void ClearLog(LogView& logs)
{
    logs.entries.clear();
    logs.selected.clear();
    logs.selectedCount = 0;
    logs.anchor.reset();
    logs.shown.clear();
    logs.filtered = 0;
    logs.widest = 0.0f;
    logs.refilter = true;
}

void ScanLogFolder(LogView& logs)
{
    const auto now = Clock::now();
    if (now - logs.filesScanned < kLogFolderRefresh)
        return;
    logs.filesScanned = now;
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(LogsDirectory(), error)) {
        if (!entry.is_regular_file(error))
            continue;
        const std::wstring extension = entry.path().extension().native();
        if (EqualsNoCase(extension, L".log") || EqualsNoCase(extension, L".txt"))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files != logs.files) {
        // The chosen file keeps being chosen when the list changes around it.
        const std::filesystem::path chosen = logs.source > 0 && logs.source <= static_cast<int>(logs.files.size()) ? logs.files[static_cast<size_t>(logs.source) - 1] : std::filesystem::path();
        logs.files = std::move(files);
        const auto found = std::ranges::find(logs.files, chosen);
        logs.source = found == logs.files.end() ? 0 : static_cast<int>(found - logs.files.begin()) + 1;
    }
}

// Pulls the live log's new lines, or loads the chosen file once.
void UpdateLogSource(LogView& logs)
{
    if (logs.source == 0) {
        if (!logs.loaded.empty()) {
            logs.loaded.clear();
            logs.sequence = 0;
            ClearLog(logs);
        }
        std::vector<log::Line> fresh;
        logs.sequence = log::LinesAfter(logs.sequence, fresh);
        for (log::Line& line : fresh) {
            const uint32_t ms = line.timeMs;
            AddEntry(logs, MakeEntry(std::format("{:02}:{:02}:{:02}.{:03} [{:5}] {}", ms / 3600000u, ms / 60000u % 60u, ms / 1000u % 60u, ms % 1000u,
                line.thread, line.text)));
        }
        return;
    }
    const std::filesystem::path& file = logs.files[static_cast<size_t>(logs.source) - 1];
    if (file == logs.loaded)
        return;
    logs.loaded = file;
    ClearLog(logs);
    std::ifstream stream(file, std::ios::binary);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        AddEntry(logs, MakeEntry(std::move(line)));
        line.clear();
    }
}

bool PassesFilter(const LogView& logs, const LogEntry& entry)
{
    if ((logs.appliedLevels & (1u << static_cast<uint32_t>(entry.level))) == 0)
        return false;
    if (logs.appliedFilter.empty())
        return true;
    return Lowercase(std::string_view(entry.raw).substr(entry.message)).find(logs.appliedFilter) != std::string::npos;
}

void FilterLog(LogView& logs)
{
    const std::string filter = Lowercase(logs.filter);
    if (filter != logs.appliedFilter || logs.levels != logs.appliedLevels)
        logs.refilter = true;
    if (logs.refilter) {
        logs.appliedFilter = filter;
        logs.appliedLevels = logs.levels;
        logs.shown.clear();
        logs.filtered = 0;
        logs.refilter = false;
    }
    for (; logs.filtered < logs.entries.size(); ++logs.filtered) {
        if (PassesFilter(logs, logs.entries[logs.filtered]))
            logs.shown.push_back(logs.filtered);
    }
}

void SelectLogLine(LogView& logs, size_t index, bool on)
{
    if (logs.selected[index] == on)
        return;
    logs.selected[index] = on;
    logs.selectedCount += on ? 1 : static_cast<size_t>(-1);
}

void ClearLogSelection(LogView& logs)
{
    std::fill(logs.selected.begin(), logs.selected.end(), false);
    logs.selectedCount = 0;
}

// The selected lines, or every shown line when none is selected.
std::string LogText(const LogView& logs, bool selectionOnly)
{
    std::string text;
    for (const size_t index : logs.shown) {
        if (!selectionOnly || logs.selected[index]) {
            text += logs.entries[index].raw;
            text += "\r\n";
        }
    }
    return text;
}

void DrawLogLines(LogView& logs)
{
    const widgets::Palette& palette = widgets::GetPalette();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiIO& io = ImGui::GetIO();
    const float lineHeight = ImGui::GetTextLineHeight() + style.CellPadding.y;
    const float timeWidth = logs.showTime ? ImGui::CalcTextSize("00:00:00.000").x + style.ItemInnerSpacing.x * 2.0f : 0.0f;
    const float height = std::max(ImGui::GetContentRegionAvail().y, lineHeight * 5.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.surface);
    const bool visible = ImGui::BeginChild("##loglines", ImVec2(0.0f, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    if (visible) {
        const bool hovered = ImGui::IsWindowHovered();
        if (hovered && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            for (const size_t index : logs.shown)
                SelectLogLine(logs, index, true);
        }
        if (hovered && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && logs.selectedCount > 0)
            SetStatus(Text(CopyToClipboard(LogText(logs, true)) ? "status.log_copied" : "status.copy_failed"));
        // Scrolling up leaves the newest lines; scrolling back down to them follows again.
        if (hovered && io.MouseWheel > 0.0f)
            logs.follow = false;
        if (hovered && io.MouseWheel < 0.0f && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - lineHeight)
            logs.follow = true;

        const float width = std::max(ImGui::GetContentRegionAvail().x, logs.widest + timeWidth + style.FramePadding.x * 2.0f);
        ImFont* strong = widgets::GetFonts().strong;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(logs.shown.size()), lineHeight + style.ItemSpacing.y);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const size_t index = logs.shown[static_cast<size_t>(row)];
                const LogEntry& entry = logs.entries[index];
                ImGui::PushID(row);
                ImGui::Selectable("##line", logs.selected[index], ImGuiSelectableFlags_None, ImVec2(width, lineHeight));
                if (ImGui::IsItemClicked()) {
                    if (io.KeyShift && logs.anchor) {
                        const size_t from = std::min(*logs.anchor, static_cast<size_t>(row));
                        const size_t to = std::max(*logs.anchor, static_cast<size_t>(row));
                        if (!io.KeyCtrl)
                            ClearLogSelection(logs);
                        for (size_t i = from; i <= to && i < logs.shown.size(); ++i)
                            SelectLogLine(logs, logs.shown[i], true);
                    } else if (io.KeyCtrl) {
                        SelectLogLine(logs, index, !logs.selected[index]);
                        logs.anchor = static_cast<size_t>(row);
                    } else {
                        ClearLogSelection(logs);
                        SelectLogLine(logs, index, true);
                        logs.anchor = static_cast<size_t>(row);
                    }
                }
                ImGui::PopID();
                const ImVec2 min = ImGui::GetItemRectMin();
                const float y = std::round(min.y + (lineHeight - ImGui::GetFontSize()) * 0.5f);
                float x = min.x + style.FramePadding.x;
                const char* text = entry.raw.c_str();
                if (logs.showTime && entry.message > 0) {
                    draw->AddText(ImVec2(x, y), ImGui::GetColorU32(palette.textDim), text, text + 12);
                    x += timeWidth;
                }
                const ImVec4 color = entry.level == LogLevel::Error ? palette.error : entry.level == LogLevel::Warning ? palette.warning : palette.text;
                const char* message = text + entry.message;
                if (entry.prefixEnd > 0) {
                    draw->AddText(strong, strong->FontSize, ImVec2(x, y), ImGui::GetColorU32(color), message, message + entry.prefixEnd);
                    x += strong->CalcTextSizeA(strong->FontSize, FLT_MAX, 0.0f, message, message + entry.prefixEnd).x;
                    message += entry.prefixEnd;
                }
                draw->AddText(ImVec2(x, y), ImGui::GetColorU32(color), message);
            }
        }
        clipper.End();
        if (logs.follow && logs.grew)
            ImGui::SetScrollY(1e9f);
    }
    ImGui::EndChild();
    logs.grew = false;
}

void DrawLogsPage(Settings&)
{
    LogView& logs = g_state.logs;
    ScanLogFolder(logs);
    UpdateLogSource(logs);
    FilterLog(logs);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float font = ImGui::GetFontSize();
    if (ImGui::BeginTable("logtools", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) {
        const bool rightToLeft = widgets::RightToLeft();
        const int sourceColumn = rightToLeft ? 2 : 0;
        const int levelsColumn = 2 - sourceColumn;
        for (int column = 0; column < 3; ++column) {
            if (column == sourceColumn)
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, font * 13.0f);
            else if (column == levelsColumn)
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, font * 15.0f);
            else
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(sourceColumn);
        std::vector<std::string> names;
        names.push_back(Text("logs.source_live"));
        for (const std::filesystem::path& file : logs.files)
            names.push_back(Narrow(file.filename().native()));
        std::vector<const char*> texts;
        for (const std::string& name : names)
            texts.push_back(name.c_str());
        if (widgets::Combo("##source", &logs.source, texts))
            logs.follow = logs.source == 0;
        ImGui::TableSetColumnIndex(1);
        widgets::TextField("##filter", logs.filter, sizeof(logs.filter), Text("logs.filter_hint"));
        ImGui::TableSetColumnIndex(levelsColumn);
        constexpr const char* kLevels[] = {"logs.level_info", "logs.level_warnings", "logs.level_errors"};
        widgets::Toggles("##levels", &logs.levels, Texts(kLevels));
        ImGui::EndTable();
    }

    ButtonFlow buttons;
    if (buttons.Add(Text("logs.follow"), logs.follow))
        logs.follow = !logs.follow;
    if (buttons.Add(Text("logs.show_time"), logs.showTime))
        logs.showTime = !logs.showTime;
    const bool any = !logs.shown.empty();
    if (buttons.Add(Text(logs.selectedCount > 0 ? "logs.copy_selection" : "logs.copy_all"), false, any))
        SetStatus(Text(CopyToClipboard(LogText(logs, logs.selectedCount > 0)) ? "status.log_copied" : "status.copy_failed"));
    if (buttons.Add(Text("logs.export"), false, any && !g_state.fileJob))
        StartFileJob(FileKind::Log, true, Scope::Global, LogText(logs, false));
    if (buttons.Add(Text("logs.open_folder")))
        OpenInShell(LogsDirectory().native());
    if (buttons.Add(Text("logs.select_all"), false, any)) {
        for (const size_t index : logs.shown)
            SelectLogLine(logs, index, true);
    }
    if (buttons.Add(Text("logs.clear_selection"), false, logs.selectedCount > 0))
        ClearLogSelection(logs);

    std::string status = ui::Format("logs.status", {{"shown", ui::Number(static_cast<double>(logs.shown.size()), 0)},
                                                      {"total", ui::Number(static_cast<double>(logs.entries.size()), 0)},
                                                      {"selected", ui::Number(static_cast<double>(logs.selectedCount), 0)}});
    if (logs.source == 0 && log::DroppedLines() > 0)
        status += " " + ui::Format("logs.dropped", {{"count", ui::Number(static_cast<double>(log::DroppedLines()), 0)}});
    widgets::Note(status.c_str());
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.5f));
    DrawLogLines(logs);
}

// ---- Menu ----

void DrawMenuPage(Settings& settings)
{
    if (widgets::BeginCard("##keys", Text("menupage.section_keys"), Text("menupage.keys_note"))) {
        if (widgets::BeginRows("keys")) {
            BeginRow("menupage.toggle_key", {FieldOf(&Settings::menuKey)});
            const std::string name = KeyName(static_cast<UINT>(settings.menuKey));
            if (widgets::Button(g_state.listeningForKey ? Text("menupage.press_key") : name.c_str(), g_state.listeningForKey, ImGui::GetContentRegionAvail().x))
                g_state.listeningForKey = true;
            EndRow();
            if (g_state.capturedKey) {
                settings.menuKey = static_cast<int>(*g_state.capturedKey);
                g_state.capturedKey.reset();
            }
            SwitchRow("menupage.escape_closes", &Settings::menuEscapeCloses);
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##appearance", Text("menupage.section_appearance"), Text("menupage.appearance_note"))) {
        if (widgets::BeginRows("appearance")) {
            SliderRow("menupage.opacity", &Settings::menuOpacity);
            SliderRow("menupage.scale", &Settings::menuScale);
            ColorRow("menupage.accent", &Settings::menuAccent, false);
            static std::vector<std::string> languages;
            static Clock::time_point scanned = {};
            if (Clock::now() - scanned > kLogFolderRefresh) {
                scanned = Clock::now();
                languages = ui::AvailableLanguages();
            }
            std::vector<std::string> names;
            names.push_back(ui::Format("menupage.language_auto", {{"language", ui::LanguageName(ui::Language())}}));
            for (const std::string& language : languages)
                names.push_back(ui::LanguageName(language));
            std::vector<const char*> texts;
            for (const std::string& name : names)
                texts.push_back(name.c_str());
            BeginRow("menupage.language", {FieldOf(&Settings::menuLanguage)});
            const auto current = std::ranges::find(languages, settings.menuLanguage);
            int index = settings.menuLanguage.empty() || current == languages.end() ? 0 : static_cast<int>(current - languages.begin()) + 1;
            if (widgets::Combo("##language", &index, texts))
                settings.menuLanguage = index == 0 ? std::string() : languages[static_cast<size_t>(index) - 1];
            EndRow();
            widgets::EndRows();
        }
    }
    widgets::EndCard();

    if (widgets::BeginCard("##behaviour", Text("menupage.section_behaviour"))) {
        if (widgets::BeginRows("behaviour")) {
            SwitchRow("menupage.remember_page", &Settings::menuRememberPage);
            BeginRow("menupage.layout");
            if (widgets::Button(Text("menupage.reset_layout"), false, ImGui::GetContentRegionAvail().x))
                g_state.resetLayout = true;
            EndRow();
            widgets::EndRows();
        }
    }
    widgets::EndCard();
}

// ---- About ----

constexpr wchar_t kRepositoryUrl[] = L"https://github.com/DeisDev/GarrysModPlusPlus";
constexpr wchar_t kIssuesUrl[] = L"https://github.com/DeisDev/GarrysModPlusPlus/issues";

void DrawAboutPage(Settings&)
{
    if (widgets::BeginCard("##about")) {
        ImGui::PushFont(widgets::GetFonts().title);
        widgets::Text(Text("menu.title"));
        ImGui::PopFont();
        widgets::Text(ui::Format("about.version", {{"version", kVersion}}).c_str());
        widgets::Text(Text("about.summary"));
    }
    widgets::EndCard();

    if (widgets::BeginCard("##author", Text("about.section_author"))) {
        widgets::Text(Text("about.author"));
        widgets::Note(Text("about.license"));
    }
    widgets::EndCard();

    if (widgets::BeginCard("##links", Text("about.section_links"))) {
        if (widgets::Link(Text("about.repository")))
            OpenInShell(kRepositoryUrl);
        if (widgets::Link(Text("about.issues")))
            OpenInShell(kIssuesUrl);
        widgets::Note(Text("about.repository_note"));
        ButtonFlow buttons;
        if (buttons.Add(Text("about.open_licenses")))
            OpenInShell((ModDirectory() / L"licenses").native());
        if (buttons.Add(Text("about.open_mod_folder")))
            OpenInShell(ModDirectory().native());
    }
    widgets::EndCard();

    if (widgets::BeginCard("##credits", Text("about.section_credits"))) {
        for (const char* key : {"general.credits_nvidia", "about.credits_amd", "about.credits_microsoft", "about.credits_valve", "about.credits_libraries",
                 "about.credits_fonts"})
            widgets::Note(Text(key));
    }
    widgets::EndCard();

    if (widgets::BeginCard("##references", Text("about.section_references"))) {
        for (const char* key : {"about.ref_sky", "about.ref_clouds", "about.ref_volumetrics", "about.ref_restir", "about.ref_skin", "about.ref_motion_blur",
                 "about.ref_moon", "about.ref_stars"})
            widgets::Note(Text(key));
    }
    widgets::EndCard();
}

struct PageInfo {
    const char* key;
    void (*draw)(Settings&);
    // What "Reset page" resets; none for pages without settings.
    SectionMask sections;
    // Whether the page edits settings, which the scope bar then says the scope of.
    bool scoped;
};

constexpr PageInfo kPages[] = {
    {"page.display", &DrawDisplayPage, SectionBit(Section::Display), false},
    {"page.graphics", &DrawGraphicsPage, SectionBit(Section::Quality), true},
    {"page.image", &DrawImagePage, SectionBit(Section::Image) | SectionBit(Section::Latency), true},
    {"page.lighting", &DrawLightingPage, SectionBit(Section::Lighting) | SectionBit(Section::General), true},
    {"page.sky", &DrawSkyPage, SectionBit(Section::Sky), true},
    {"page.camera", &DrawCameraPage, SectionBit(Section::Camera), true},
    {"page.performance", &DrawPerformancePage, SectionBit(Section::Overlay) | SectionBit(Section::Developer), false},
    {"page.profiles", &DrawProfilesPage, 0, true},
    {"page.logs", &DrawLogsPage, 0, false},
    {"page.menu", &DrawMenuPage, SectionBit(Section::Menu), false},
    {"page.about", &DrawAboutPage, 0, false},
};
static_assert(std::size(kPages) == static_cast<size_t>(Page::Count));

// Whose settings the pages show and change: every map's, or the current map's own.
void DrawScopeBar()
{
    const std::string& map = config::MapName();
    if (map.empty())
        g_state.scope = Scope::Global;
    const std::string mapLabel = map.empty() ? std::string(Text("scope.map_none")) : ui::Format("scope.map", {{"map", map}});
    const char* options[] = {Text("scope.all_maps"), mapLabel.c_str()};
    const bool available[] = {true, !map.empty()};
    int index = g_state.scope == Scope::Map ? 1 : 0;
    const std::string help = g_state.scope == Scope::Map ? ui::Format("scope.map_note", {{"map", map}})
        : config::MapSetCount() > 0                     ? ui::Format("scope.global_note", {{"map", map}})
                                                        : std::string(Text("scope.help"));
    // The scope bar with its help mark after it, at the end of the line.
    if (ImGui::BeginTable("scope", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) {
        const int barColumn = widgets::RightToLeft() ? 1 : 0;
        for (int column = 0; column < 2; ++column) {
            if (column == barColumn)
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
            else
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, widgets::HelpMarkSpan());
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(barColumn);
        widgets::Segmented("##scope", &index, options, available);
        ImGui::TableSetColumnIndex(1 - barColumn);
        widgets::HelpMark(help.c_str());
        ImGui::EndTable();
    }
    g_state.scope = index == 1 ? Scope::Map : Scope::Global;
    ImGui::Spacing();
}

// What is running in the background that the player may notice, or nothing. The loading screen shows its own progress.
std::string NoticeText()
{
    if (loading::Active())
        return {};
    if (const std::optional<std::pair<size_t, size_t>> pipelines = world::LightingPipelineProgress())
        return ui::Format("notice.pipelines", {{"done", ui::Number(static_cast<double>(pipelines->first), 0)},
                                                  {"total", ui::Number(static_cast<double>(pipelines->second), 0)}});
    size_t left = 0;
    for (const shader_precache::Priority priority : {shader_precache::Priority::Map, shader_precache::Priority::Water}) {
        const shader_precache::Progress progress = shader_precache::Status(priority);
        left += progress.total - progress.done;
    }
    if (left > 0)
        return ui::Format("notice.shaders", {{"left", ui::Number(static_cast<double>(left), 0)}});
    const shader_precache::Progress background = shader_precache::Status(shader_precache::Priority::Background);
    if (background.done < background.total)
        return ui::Format("notice.shaders_background", {{"done", ui::Number(static_cast<double>(background.done), 0)},
                                                           {"total", ui::Number(static_cast<double>(background.total), 0)}});
    return {};
}

constexpr ImGuiWindowFlags kInfoBoxFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

// Places the next info box in `corner`, `offset` pixels further in from its top or bottom edge.
void PlaceInfoBox(OverlayCorner corner, float offset, float margin, float alpha)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool right = corner == OverlayCorner::TopRight || corner == OverlayCorner::BottomRight;
    const bool bottom = corner == OverlayCorner::BottomLeft || corner == OverlayCorner::BottomRight;
    const float x = right ? viewport->WorkPos.x + viewport->WorkSize.x - margin : viewport->WorkPos.x + margin;
    const float y = bottom ? viewport->WorkPos.y + viewport->WorkSize.y - margin - offset : viewport->WorkPos.y + margin + offset;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(alpha);
}

// The notice's corner: bottom right, or bottom left for right-to-left languages.
OverlayCorner NoticeCorner() { return widgets::RightToLeft() ? OverlayCorner::BottomLeft : OverlayCorner::BottomRight; }

void DrawNotice(const std::string& text, float offset)
{
    PlaceInfoBox(NoticeCorner(), offset, ImGui::GetFontSize(), widgets::InfoBoxAlpha());
    if (ImGui::Begin("###gmodpp_notice", nullptr, kInfoBoxFlags)) {
        ImGui::PushFont(widgets::GetFonts().strong);
        ImGui::TextUnformatted(ui::Visual(Text("notice.title")).c_str());
        ImGui::PopFont();
        ImGui::TextUnformatted(ui::Visual(text).c_str());
    }
    ImGui::End();
}

void Line(const std::string& text) { ImGui::TextUnformatted(ui::Visual(text).c_str()); }

void UpdateFigures(const Statistics& statistics)
{
    const auto now = Clock::now();
    if (now - g_state.figuresTime < kOverlayRefresh)
        return;
    g_state.figuresTime = now;
    const std::optional<FrameRates> rates = MeasureFrames(statistics);
    if (!rates)
        return;
    OverlayFigures& figures = g_state.figures;
    figures.fps = rates->now;
    figures.renderedFps = rates->rendered;
    figures.frameMs = rates->nowMs;
    figures.low1 = rates->low1;
    figures.low01 = rates->low01;
    // The graph shows the last 240 frames, which fit its width.
    const size_t shown = std::min<size_t>(rates->chronological.size(), 240);
    figures.frameTimes.assign(rates->chronological.end() - static_cast<std::ptrdiff_t>(shown), rates->chronological.end());
    figures.longestFrame = std::max(*std::max_element(figures.frameTimes.begin(), figures.frameTimes.end()), 1.0f);
}

void UpdateMemory()
{
    const auto now = Clock::now();
    if (!g_state.adapter || now - g_state.memoryTime < kMemoryRefresh)
        return;
    g_state.memoryTime = now;
    g_state.memoryKnown = SUCCEEDED(g_state.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &g_state.memory));
}

// The always-visible performance overlay; returns its height.
// Whether the game hides its HUD: its cl_drawhud cvar, or its Lua (the camera, HUD-hiding addons: client::HudHidden).
bool GameHudHidden()
{
    if (!engine::IsInGame())
        return false;
    if (client::HudHidden())
        return true;
    const std::optional<std::string> drawHud = engine::ConVarString("cl_drawhud");
    return drawHud && std::strtol(drawHud->c_str(), nullptr, 10) == 0;
}

float DrawPerformanceOverlay(const Settings& settings)
{
    const Statistics& statistics = GetStatistics();
    UpdateFigures(statistics);
    const OverlayFigures& figures = g_state.figures;
    const OverlayDetail detail = settings.overlayDetail;
    PlaceInfoBox(settings.overlayCorner, 0.0f, ImGui::GetFontSize() * settings.overlayMargin, settings.overlayOpacity);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(settings.overlayBackground[0], settings.overlayBackground[1], settings.overlayBackground[2], 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(settings.overlayText[0], settings.overlayText[1], settings.overlayText[2], 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, settings.overlayBorder ? ImGui::GetStyle().WindowBorderSize : 0.0f);
    float height = 0.0f;
    if (ImGui::Begin("###gmodpp_performance", nullptr, kInfoBoxFlags)) {
        ImGui::SetWindowFontScale(settings.overlayScale);
        ImGui::PushFont(widgets::GetFonts().strong);
        if (figures.renderedFps)
            Line(ui::Format("overlay.fps_generated", {{"value", ui::Number(figures.fps, 0)}, {"rendered", ui::Number(*figures.renderedFps, 0)}}));
        else
            Line(ui::Format("overlay.fps", {{"value", ui::Number(figures.fps, 0)}}));
        ImGui::PopFont();
        if (detail >= OverlayDetail::Normal) {
            Line(Milliseconds(figures.frameMs));
            Line(ui::Format("overlay.low_1", {{"value", ui::Number(figures.low1, 0)}}));
        }
        if (detail >= OverlayDetail::Full) {
            Line(ui::Format("overlay.low_01", {{"value", ui::Number(figures.low01, 0)}}));
            if (!figures.frameTimes.empty())
                ImGui::PlotLines("##frametimes", figures.frameTimes.data(), static_cast<int>(figures.frameTimes.size()), 0, nullptr, 0.0f, figures.longestFrame,
                    ImVec2(ImGui::GetFontSize() * 13.0f, ImGui::GetFontSize() * 2.2f));
            Line(ui::Format("overlay.gpu_lighting", {{"value", ui::Number(statistics.lightingMs, 2)}}));
            const ImVec2 output = ImGui::GetIO().DisplaySize;
            Line(ui::Format("overlay.output", {{"width", ui::Number(output.x, 0)}, {"height", ui::Number(output.y, 0)}}));
            if (statistics.renderWidth != 0)
                Line(ui::Format("overlay.render", {{"width", ui::Number(statistics.renderWidth, 0)}, {"height", ui::Number(statistics.renderHeight, 0)}}));
        }
        if (detail >= OverlayDetail::Extra) {
            UpdateMemory();
            if (g_state.memoryKnown) {
                constexpr double kGigabyte = 1024.0 * 1024.0 * 1024.0;
                Line(ui::Format("overlay.memory", {{"used", ui::Number(static_cast<double>(g_state.memory.CurrentUsage) / kGigabyte, 1)},
                                                      {"budget", ui::Number(static_cast<double>(g_state.memory.Budget) / kGigabyte, 1)}}));
            }
            constexpr const char* kPresetNames[] = {"quality.preset_low", "quality.preset_medium", "quality.preset_high", "quality.preset_ultra",
                "quality.preset_custom"};
            Line(ui::Format("overlay.preset", {{"preset", Text(kPresetNames[static_cast<size_t>(settings.qualityPreset)])}}));
            Line(ui::Format("overlay.lights", {{"map", ui::Number(statistics.mapLights, 0)}, {"dynamic", ui::Number(statistics.dynamicLights, 0)},
                                                  {"projected", ui::Number(statistics.projectedLights, 0)}}));
            Line(ui::Format("overlay.instances", {{"count", ui::Number(statistics.sceneInstances, 0)}}));
            if (ImGui::BeginTable("passes", 2, ImGuiTableFlags_SizingFixedFit)) {
                const int nameColumn = widgets::RightToLeft() ? 1 : 0;
                auto passRow = [&](const char* label, double ms) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(nameColumn);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    Line(Text(label));
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1 - nameColumn);
                    Line(Milliseconds(ms));
                };
                for (size_t i = 0; i < std::size(kPasses); ++i)
                    passRow(kPasses[i], statistics.passMs[i]);
                passRow("pass.other_views", statistics.otherViewsMs);
                ImGui::EndTable();
            }
        }
        height = ImGui::GetWindowHeight();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    return height;
}

// Returns false when the close button was clicked.
bool DrawMenu()
{
    const float font = ImGui::GetFontSize();
    const ImGuiCond placement = std::exchange(g_state.resetLayout, false) ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(font * 1.5f, font * 1.5f), placement);
    ImGui::SetNextWindowSize(ImVec2(font * 48.0f, font * 38.0f), placement);
    ImGui::SetNextWindowSizeConstraints(ImVec2(font * 32.0f, font * 26.0f), ImVec2(FLT_MAX, FLT_MAX));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    bool keepOpen = true;
    if (!OnMap())
        g_state.scope = Scope::Global;
    Settings edited = config::View(g_state.scope);
    const Settings before = edited;
    g_edit = {&edited, g_state.scope, {}};
    bool resetPage = false;
    if (ImGui::Begin("###gmodpp_menu", nullptr, kFlags)) {
        const std::string closeHint = ui::Format("menu.close_hint", {{"key", KeyName(static_cast<UINT>(GetSettings().menuKey))}});
        keepOpen = !widgets::Header(Text("menu.title"), closeHint.c_str());
        // Escape closes the menu unless a popup or dialog takes it, or a key is being picked.
        if (GetSettings().menuEscapeCloses && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
            !g_state.listeningForKey)
            keepOpen = false;

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
                // The version sits at the bottom of the page list, or right under it when the list fills the window.
                const float bottom = ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y;
                ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + style.ItemSpacing.y, bottom - ImGui::GetFrameHeight()));
                const std::string version = ui::Visual(ui::Format("menu.version", {{"version", kVersion}}));
                const float padding = style.FramePadding.x * 1.6f;
                const float start = ImGui::GetCursorPosX() + padding;
                const float end = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - padding;
                ImGui::SetCursorPosX(widgets::RightToLeft() ? end - ImGui::CalcTextSize(version.c_str()).x : start);
                ImGui::TextDisabled("%s", version.c_str());
            }
            ImGui::EndChild();
        };
        const PageInfo& page = kPages[static_cast<size_t>(g_state.page)];
        auto drawPage = [&](float width) {
            const bool reset = page.sections != 0;
            // The footer holds the outcome of the last action (any page) at the start of the line and "Reset page" at
            // its end.
            const bool status = !g_state.status.empty() && Clock::now() - g_state.statusTime < kStatusTime;
            const float footer = reset || status ? ImGui::GetFrameHeight() + style.ItemSpacing.y : 0.0f;
            ImGui::BeginGroup();
            // Each page keeps its own scroll position. Its scrollbar always takes its room, so the content's width doesn't
            // change a frame after a page is shown, when Dear ImGui finds the page taller than the view; with nothing
            // to scroll it is invisible.
            ImGui::PushID(static_cast<int>(g_state.page));
            const bool scrolls = g_state.pageScrolls[static_cast<size_t>(g_state.page)];
            if (!scrolls) {
                for (const ImGuiCol grab : {ImGuiCol_ScrollbarGrab, ImGuiCol_ScrollbarGrabHovered, ImGuiCol_ScrollbarGrabActive})
                    ImGui::PushStyleColor(grab, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            }
            const bool visible = ImGui::BeginChild("##page", ImVec2(width, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            if (!scrolls)
                ImGui::PopStyleColor(3);
            if (visible) {
                // A scope picked here takes effect next frame: this frame edits the scope its settings came from.
                if (page.scoped)
                    DrawScopeBar();
                page.draw(edited);
            }
            g_state.pageScrolls[static_cast<size_t>(g_state.page)] = ImGui::GetScrollMaxY() > 0.0f;
            ImGui::EndChild();
            ImGui::PopID();
            if (status) {
                const float lineY = ImGui::GetCursorPosY();
                ImGui::AlignTextToFramePadding();
                const std::string visual = ui::Visual(g_state.status);
                if (widgets::RightToLeft())
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(visual.c_str()).x);
                ImGui::TextUnformatted(visual.c_str());
                ImGui::SetCursorPosY(lineY);
            }
            if (reset && widgets::EndButton(Text(g_edit.scope == Scope::Map ? "menu.reset_page_map" : "menu.reset_page")))
                resetPage = true;
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

    if (edited.menuRememberPage)
        edited.menuLastPage = static_cast<int>(g_state.page);
    edited.menuCollapsed = widgets::CollapsedCards();
    const Scope scope = g_edit.scope;
    if (edited != before)
        config::Commit(scope, before, edited);
    if (!g_edit.resets.empty())
        config::Reset(scope, g_edit.resets);
    if (resetPage)
        config::ResetSections(scope, kPages[static_cast<size_t>(g_state.page)].sections);
    g_edit = {};
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
        {"SetCursor", {reinterpret_cast<void*>(&HookSetCursor), reinterpret_cast<void**>(&g_state.setCursor)}},
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

// The adapter the device runs on, for the performance overlay's video memory figures.
void FindAdapter(ID3D12Device* device)
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&g_state.adapter))))
        log::Info("Overlay: the device's adapter was not found; the performance overlay shows no video memory");
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
    g_state.fontDirectory = GetOptions().gameDirectory / L"garrysmod" / L"resource" / L"fonts";
    g_state.displayHeight = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
    g_state.appliedScale = GetSettings().menuScale;
    ui::SetLanguageOverride(GetSettings().menuLanguage);
    widgets::LoadTheme(g_state.fontDirectory, ModDirectory() / L"fonts" / L"lucide.ttf", g_state.displayHeight, g_state.appliedScale);
    widgets::SetCollapsedCards(GetSettings().menuCollapsed);
    widgets::SetAccent(GetSettings().menuAccent);
    widgets::SetOpacity(GetSettings().menuOpacity);

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
    FindAdapter(device);
    g_state.initialized = true;
    log::Info("Overlay: ready ({} opens the menu)", KeyName(static_cast<UINT>(GetSettings().menuKey)));
    return true;
}

namespace {

// Applies the Menu page's settings: the language, accent and opacity at once; a new scale once its slider is released,
// as the fonts are built again at the new size (between frames, after the GPU has finished with the old atlas).
void ApplyMenuSettings(const Settings& settings)
{
    ui::SetLanguageOverride(settings.menuLanguage);
    widgets::SetAccent(settings.menuAccent);
    widgets::SetOpacity(settings.menuOpacity);
    if (settings.menuScale == g_state.appliedScale || ImGui::IsMouseDown(ImGuiMouseButton_Left))
        return;
    native::GetBackend().Flush();
    ImGui_ImplDX12_InvalidateDeviceObjects();
    ImGui::GetIO().Fonts->Clear();
    widgets::LoadTheme(g_state.fontDirectory, ModDirectory() / L"fonts" / L"lucide.ttf", g_state.displayHeight, settings.menuScale);
    widgets::SetAccent(settings.menuAccent);
    widgets::SetOpacity(settings.menuOpacity);
    ImGui_ImplDX12_CreateDeviceObjects();
    g_state.appliedScale = settings.menuScale;
    // The Logs page measured its lines in the old font.
    g_state.logs.loaded.clear();
    g_state.logs.sequence = 0;
    ClearLog(g_state.logs);
    log::Info("Overlay: menu scale {:.2f}", settings.menuScale);
}

}

bool Open() { return g_state.initialized && g_state.open; }

bool Visible()
{
    if (!g_state.initialized)
        return false;
    g_state.notice = NoticeText();
    const auto now = Clock::now();
    if (g_state.notice.empty())
        g_state.noticeSince.reset();
    else if (!g_state.noticeSince)
        g_state.noticeSince = now;
    return g_state.open || g_state.fileJob || GetSettings().performanceOverlay || (g_state.noticeSince && now - *g_state.noticeSince >= kNoticeDelay);
}

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
    ApplyMenuSettings(GetSettings());
    ui::UpdateLanguage();
    FinishFileJob();
    // With the menu closed the game owns the cursor: Dear ImGui neither draws one nor changes the system's.
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_state.open;
    if (g_state.open)
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    const Settings& settings = GetSettings();
    // The notice moves above the performance overlay when they share a corner.
    float noticeOffset = 0.0f;
    if (settings.performanceOverlay && !(settings.overlayHideWithHud && GameHudHidden())) {
        const float height = DrawPerformanceOverlay(settings);
        if (settings.overlayCorner == NoticeCorner())
            noticeOffset = height + ImGui::GetStyle().ItemSpacing.y;
    }
    if (!g_state.notice.empty() && g_state.noticeSince && Clock::now() - *g_state.noticeSince >= kNoticeDelay)
        DrawNotice(g_state.notice, noticeOffset);
    if (g_state.open && !DrawMenu())
        CloseMenu();
    ImGui::Render();
    ID3D12DescriptorHeap* heaps[] = {g_state.descriptors->Heap()};
    list->SetDescriptorHeaps(1, heaps);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
}

}
