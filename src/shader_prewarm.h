#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>

namespace gmodpp {

struct PrewarmProgress {
    // Unique combos handled so far, of all; total is known after a first pass over the files.
    std::atomic<size_t> done = 0;
    std::atomic<size_t> total = 0;
    std::atomic<bool> cancel = false;
};

// Whether the shader cache holds a finished prewarm for the current translator and the game's shader files.
bool PrewarmIsCurrent(const std::filesystem::path& gameDirectory);

// Translates and compiles every combo of the game's .vcs shaders into the shader cache on worker threads, in the
// variants draws usually take: the default one, and for vertex shaders reading a normal also the one for compressed
// vertices. Combos already cached are skipped. Logs failures; marks the cache current when it finishes uncancelled.
void PrewarmShaders(const std::filesystem::path& gameDirectory, const std::filesystem::path& compilerDirectory, PrewarmProgress& progress);

// Runs PrewarmShaders behind a window with a progress bar; closing the window cancels it. Blocks until it ends.
void PrewarmWithProgressWindow(const std::filesystem::path& gameDirectory, const std::filesystem::path& compilerDirectory);

}
