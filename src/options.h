#pragma once

#include <filesystem>

namespace gmodpp {

struct Options {
    bool devMode = false;
    bool d3d12Debug = false;
    bool gpuValidation = false;
    std::filesystem::path gameDirectory;
};

// Defined in shaderapi_shim.cpp.
const Options& GetOptions();

}
