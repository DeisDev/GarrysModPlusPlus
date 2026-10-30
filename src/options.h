#pragma once

#include <filesystem>

namespace gmodpp {

struct Options {
    bool enabled = false;
    bool devMode = false;
    bool d3d12Debug = false;
    bool gpuValidation = false;
    std::filesystem::path gameDirectory;
};

// Defined by each module: mod.cpp in d3d9.dll, shaderapi_shim.cpp in gmodpp.dll.
const Options& GetOptions();

}
