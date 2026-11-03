#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace gmodpp {

bool EqualsNoCase(std::wstring_view a, std::wstring_view b);
bool HasArgument(std::wstring_view argument);
// The word after `argument` on the command line, or empty when it is absent or last.
std::wstring ArgumentValue(std::wstring_view argument);
std::filesystem::path ExecutablePath();
// <game>\gmodplusplus: everything of the mod's but the loader, which Windows finds only in the game folder.
const std::filesystem::path& ModDirectory();
bool IsGameExecutable(const std::filesystem::path& exe);
// Keeps the DLL holding this code loaded until the process exits, for threads that outlive the game's use of it: the
// engine frees the shader API's DLL at shutdown, and a thread still running in it would run in unmapped code.
void PinThisModule();

}
