#include "mod.h"

#include "command_line.h"
#include "log.h"

#include <filesystem>

namespace gmodpp {

namespace {

Options LoadOptions()
{
    Options options;
    std::filesystem::path exe = ExecutablePath();
    // The native backend's vanilla reference loads the real shaderapidx9.dll, which must get the system Direct3D 9
    // untouched; gmodpp.log then belongs to gmodpp.dll.
    if (!IsGameExecutable(exe) || HasArgument(L"-gmodpp_vanilla"))
        return options;

    options.gameDirectory = exe.parent_path();
    log::Open((options.gameDirectory / L"gmodpp.log").c_str());

    options.devMode = HasArgument(L"-gmodpp_dev");
    options.d3d12Debug = HasArgument(L"-gmodpp_debug");
    options.gpuValidation = HasArgument(L"-gmodpp_gbv");
    options.enabled = HasArgument(L"-insecure");

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
