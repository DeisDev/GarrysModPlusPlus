#include "command_line.h"
#include "gmodpp_version.h"
#include "log.h"

#include <windows.h>

#include <MinHook.h>

#include <cstring>
#include <filesystem>
#include <string_view>

extern "C" void* gmodpp_winmm_table[];
extern "C" const char* const gmodpp_winmm_names[];
extern "C" const DWORD gmodpp_winmm_count;

using namespace gmodpp;

namespace {

std::filesystem::path g_shaderApiPath;
decltype(&LoadLibraryExA) g_loadLibraryExA = nullptr;

// Loaded while the game loads us, not on first use: GMod's Chromium child processes, which run the same exe and
// load us too, can no longer load the system winmm.dll once their sandbox is up.
bool LoadSystemWinmm()
{
    wchar_t directory[MAX_PATH];
    UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    HMODULE winmm = LoadLibraryW((std::filesystem::path(directory) / L"winmm.dll").c_str());
    if (winmm == nullptr)
        return false;
    for (DWORD i = 0; i < gmodpp_winmm_count; ++i) {
        gmodpp_winmm_table[i] = reinterpret_cast<void*>(GetProcAddress(winmm, gmodpp_winmm_names[i]));
        if (gmodpp_winmm_table[i] == nullptr)
            return false;
    }
    return true;
}

bool IsShaderApiPath(const char* path)
{
    const char* name = path;
    for (const char* c = path; *c != '\0'; ++c) {
        if (*c == '\\' || *c == '/')
            name = c + 1;
    }
    return _stricmp(name, "shaderapidx9.dll") == 0;
}

// materialsystem.dll's SetShaderAPI (CMaterialSystem vtable +0x30, called by launcher.dll with "shaderapidx9.dll")
// loads <game>\bin\win64\shaderapidx9.dll with LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH), either
// directly or on a worker thread, then takes its CreateInterface. Confirmed in materialsystem.dll at 0x1800237c0
// (SetShaderAPI), 0x18006ad80 (Sys_LoadModule) and 0x18006ac10 / 0x18006af20 (the two LoadLibraryExA calls).
HMODULE WINAPI LoadLibraryExAHook(LPCSTR fileName, HANDLE file, DWORD flags)
{
    if (fileName == nullptr || !IsShaderApiPath(fileName))
        return g_loadLibraryExA(fileName, file, flags);
    log::Info("Loading {} instead of {}", g_shaderApiPath.string(), fileName);
    HMODULE module = LoadLibraryExW(g_shaderApiPath.c_str(), file, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr)
        log::Info("Failed to load {}: {}", g_shaderApiPath.string(), GetLastError());
    return module;
}

// gmod_win64.exe's WinMain (0x140056a30) runs as a Chromium child (CefExecuteProcess, 0x140057100) when its command
// line contains "--type=", a case-sensitive substring search; otherwise it loads launcher.dll. Confirmed with REA.
bool IsChromiumChild()
{
    return std::wstring_view(GetCommandLineW()).find(L"--type=") != std::wstring_view::npos;
}

void InstallShaderApiRedirect()
{
    std::filesystem::path exe = ExecutablePath();
    if (!IsGameExecutable(exe) || IsChromiumChild())
        return;
    std::error_code error;
    std::filesystem::create_directories(ModDirectory() / L"logs", error);
    log::Open((ModDirectory() / L"logs" / L"loader.log").c_str());
    log::Info("GarrysMod++ {} loader", kVersion);

    g_shaderApiPath = ModDirectory() / L"gmodpp.dll";
    MH_STATUS status = MH_Initialize();
    if (status == MH_OK)
        status = MH_CreateHookApi(L"kernel32", "LoadLibraryExA", reinterpret_cast<LPVOID>(&LoadLibraryExAHook), reinterpret_cast<LPVOID*>(&g_loadLibraryExA));
    if (status == MH_OK)
        status = MH_EnableHook(MH_ALL_HOOKS);
    if (status == MH_OK)
        log::Info("Redirecting shaderapidx9.dll to {}", g_shaderApiPath.string());
    else
        log::Info("Could not hook LoadLibraryExA ({}): the game's shaderapidx9.dll will be used", MH_StatusToString(status));
}

}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(instance);
    if (!LoadSystemWinmm())
        return FALSE;
    InstallShaderApiRedirect();
    return TRUE;
}
