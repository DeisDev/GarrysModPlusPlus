#pragma once

#include <windows.h>

#include <filesystem>

namespace gmodpp {

struct Options {
    bool enabled = false;
    bool devMode = false;
    bool d3d12Debug = false;
    bool gpuValidation = false;
    std::filesystem::path gameDirectory;
};

const Options& GetOptions();
HMODULE SystemD3D9();

template <class Fn>
Fn SystemProc(const char* name)
{
    HMODULE module = SystemD3D9();
    return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

}
