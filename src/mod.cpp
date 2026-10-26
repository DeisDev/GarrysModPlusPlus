#include "mod.h"

#include "log.h"

#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string_view>

namespace gmodpp {

namespace {

bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool HasArgument(std::wstring_view commandLine, std::wstring_view argument)
{
    size_t pos = 0;
    while (pos < commandLine.size()) {
        while (pos < commandLine.size() && std::iswspace(commandLine[pos]))
            ++pos;
        size_t end = pos;
        while (end < commandLine.size() && !std::iswspace(commandLine[end]))
            ++end;
        if (EqualsNoCase(commandLine.substr(pos, end - pos), argument))
            return true;
        pos = end;
    }
    return false;
}

std::filesystem::path ExecutablePath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

Options LoadOptions()
{
    Options options;
    std::filesystem::path exe = ExecutablePath();
    if (!EqualsNoCase(exe.filename().native(), L"gmod_win64.exe"))
        return options;

    options.gameDirectory = exe.parent_path();
    log::Open((options.gameDirectory / L"gmodpp.log").c_str());

    std::wstring_view commandLine = GetCommandLineW();
    options.devMode = HasArgument(commandLine, L"-gmodpp_dev");
    options.d3d12Debug = HasArgument(commandLine, L"-gmodpp_debug");
    options.gpuValidation = HasArgument(commandLine, L"-gmodpp_gbv");
    options.enabled = HasArgument(commandLine, L"-insecure");

    if (options.enabled)
        log::Info("GarrysMod++ enabled (dev mode: {}, debug layer: {}, GPU validation: {})", options.devMode, options.d3d12Debug, options.gpuValidation);
    else
        log::Info("GarrysMod++ disabled: launch with -insecure to enable it. Passing through to the system Direct3D 9.");
    return options;
}

}

const Options& GetOptions()
{
    static const Options options = LoadOptions();
    return options;
}

HMODULE SystemD3D9()
{
    static const HMODULE module = [] {
        wchar_t directory[MAX_PATH];
        UINT length = GetSystemDirectoryW(directory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            log::Info("GetSystemDirectoryW failed: {}", GetLastError());
            return HMODULE{};
        }
        std::filesystem::path path = std::filesystem::path(directory) / L"d3d9.dll";
        HMODULE loaded = LoadLibraryW(path.c_str());
        if (!loaded)
            log::Info("Failed to load the system d3d9.dll: {}", GetLastError());
        return loaded;
    }();
    return module;
}

}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
