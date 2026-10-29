#pragma once

#include <filesystem>

namespace gmodpp {

// Translates and compiles every combo of the game's .vcs shaders on worker threads and logs failures per file and
// in total. Blocks until done.
void PrewarmShaders(const std::filesystem::path& gameDirectory, const std::filesystem::path& compilerDirectory);

}
