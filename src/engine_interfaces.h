#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gmodpp::engine {

// Resolves the engine and filesystem interfaces once their modules are loaded; safe to call every frame.
void TryInitialize();
bool Ready();

// Current map path as reported by the engine, e.g. "maps/gm_construct.bsp"; empty when no map is loaded.
std::string LevelName();

// Queues console commands as if typed into the console.
void ExecuteCommand(const std::string& command);

// Reads a file through the game's filesystem (loose files, mounted content and addons).
bool ReadGameFile(const std::string& path, std::vector<std::byte>& data);

}
