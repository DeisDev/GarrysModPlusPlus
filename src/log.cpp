#include "log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>

namespace gmodpp::log {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
}

void Open(const wchar_t* path)
{
    std::scoped_lock lock(g_mutex);
    if (g_file == nullptr)
        g_file = _wfsopen(path, L"w", _SH_DENYWR);
}

void Write(std::string_view line)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::scoped_lock lock(g_mutex);
    if (g_file == nullptr)
        return;
    std::fprintf(g_file, "%02u:%02u:%02u.%03u [%5lu] %.*s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        GetCurrentThreadId(), static_cast<int>(line.size()), line.data());
    std::fflush(g_file);
}

}
