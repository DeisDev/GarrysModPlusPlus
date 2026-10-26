#pragma once

#include <format>
#include <string_view>

namespace gmodpp::log {

void Open(const wchar_t* path);
void Write(std::string_view line);

template <class... Args>
void Info(std::format_string<Args...> fmt, Args&&... args)
{
    Write(std::format(fmt, std::forward<Args>(args)...));
}

}
