#pragma once

#include <filesystem>
#include <string_view>

namespace gmodpp {

bool EqualsNoCase(std::wstring_view a, std::wstring_view b);
bool HasArgument(std::wstring_view argument);
std::filesystem::path ExecutablePath();
bool IsGameExecutable(const std::filesystem::path& exe);

}
