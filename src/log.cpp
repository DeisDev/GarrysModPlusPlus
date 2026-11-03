#include "log.h"

#include <windows.h>

#include <cstdio>
#include <deque>
#include <mutex>

namespace gmodpp::log {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
std::wstring g_path;
std::deque<Line> g_lines;
uint64_t g_sequence = 0;
uint64_t g_dropped = 0;
}

void Open(const wchar_t* path)
{
    std::scoped_lock lock(g_mutex);
    if (g_file == nullptr) {
        g_file = _wfsopen(path, L"w", _SH_DENYWR);
        g_path = path;
    }
}

void Write(std::string_view line)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    const uint32_t timeMs = ((t.wHour * 60u + t.wMinute) * 60u + t.wSecond) * 1000u + t.wMilliseconds;
    const DWORD thread = GetCurrentThreadId();
    std::scoped_lock lock(g_mutex);
    if (g_file == nullptr)
        return;
    std::fprintf(g_file, "%02u:%02u:%02u.%03u [%5lu] %.*s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, thread, static_cast<int>(line.size()),
        line.data());
    std::fflush(g_file);
    g_lines.push_back({++g_sequence, timeMs, thread, std::string(line)});
    if (g_lines.size() > kKeptLines) {
        g_lines.pop_front();
        ++g_dropped;
    }
}

uint64_t LinesAfter(uint64_t after, std::vector<Line>& lines)
{
    std::scoped_lock lock(g_mutex);
    if (g_lines.empty() || g_lines.back().sequence <= after)
        return g_sequence;
    // The kept lines have consecutive sequences, so the first one wanted is found by its distance from the last.
    const uint64_t first = g_lines.front().sequence;
    const size_t start = after < first ? 0 : static_cast<size_t>(after - first + 1);
    lines.insert(lines.end(), g_lines.begin() + static_cast<std::ptrdiff_t>(start), g_lines.end());
    return g_sequence;
}

uint64_t DroppedLines()
{
    std::scoped_lock lock(g_mutex);
    return g_dropped;
}

const std::wstring& Path()
{
    std::scoped_lock lock(g_mutex);
    return g_path;
}

}
