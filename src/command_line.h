#pragma once

#include <filesystem>
#include <string_view>

namespace gmodpp {

bool EqualsNoCase(std::wstring_view a, std::wstring_view b);
bool HasArgument(std::wstring_view argument);
std::filesystem::path ExecutablePath();
// <game>\gmodplusplus: everything of the mod's but the loader, which Windows finds only in the game folder.
const std::filesystem::path& ModDirectory();
bool IsGameExecutable(const std::filesystem::path& exe);

}
