#include "command_line.h"

#include <windows.h>

#include <cwctype>
#include <string>

namespace gmodpp {

bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool HasArgument(std::wstring_view argument)
{
    std::wstring_view commandLine = GetCommandLineW();
    size_t pos = 0;
    while (pos < commandLine.size()) {
        while (pos < commandLine.size() && std::iswspace(commandLine[pos]))
            ++pos;
        size_t end = pos;
        while (end < commandLine.size() && !std::iswspace(commandLine[end]))
            ++end;
        if (EqualsNoCase(commandLine.substr(pos, end - pos), argument))
            return true;
        pos = end;
    }
    return false;
}

std::filesystem::path ExecutablePath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool IsGameExecutable(const std::filesystem::path& exe)
{
    return EqualsNoCase(exe.filename().native(), L"gmod_win64.exe");
}

}
