#include "command_line.h"

#include <windows.h>

#include <cwctype>
#include <string>

namespace gmodpp {

bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

namespace {

// The command line's next whitespace-separated word from `pos`, which moves past it; empty at the end.
std::wstring_view NextWord(std::wstring_view commandLine, size_t& pos)
{
    while (pos < commandLine.size() && std::iswspace(commandLine[pos]))
        ++pos;
    const size_t start = pos;
    while (pos < commandLine.size() && !std::iswspace(commandLine[pos]))
        ++pos;
    return commandLine.substr(start, pos - start);
}

}

bool HasArgument(std::wstring_view argument)
{
    const std::wstring_view commandLine = GetCommandLineW();
    size_t pos = 0;
    while (pos < commandLine.size()) {
        if (EqualsNoCase(NextWord(commandLine, pos), argument))
            return true;
    }
    return false;
}

std::wstring ArgumentValue(std::wstring_view argument)
{
    const std::wstring_view commandLine = GetCommandLineW();
    size_t pos = 0;
    while (pos < commandLine.size()) {
        if (EqualsNoCase(NextWord(commandLine, pos), argument))
            return std::wstring(NextWord(commandLine, pos));
    }
    return {};
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

void PinThisModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&PinThisModule), &module);
}

const std::filesystem::path& ModDirectory()
{
    static const std::filesystem::path directory = ExecutablePath().parent_path() / L"gmodplusplus";
    return directory;
}

bool IsGameExecutable(const std::filesystem::path& exe)
{
    return EqualsNoCase(exe.filename().native(), L"gmod_win64.exe");
}

}
