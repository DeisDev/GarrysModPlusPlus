#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace gmodpp::log {

void Open(const wchar_t* path);
void Write(std::string_view line);

template <class... Args>
void Info(std::format_string<Args...> fmt, Args&&... args)
{
    Write(std::format(fmt, std::forward<Args>(args)...));
}

// A line as written to the file: its sequence number (1 for the first line since the log opened), the local time of
// day it was written at in milliseconds, the thread that wrote it and its text.
struct Line {
    uint64_t sequence;
    uint32_t timeMs;
    uint32_t thread;
    std::string text;
};

// The lines kept in memory for the menu's log viewer: the last kKeptLines at most. Appends to `lines` every kept line
// whose sequence is above `after` and returns the sequence of the last line written (0 before the first).
inline constexpr size_t kKeptLines = 50000;
uint64_t LinesAfter(uint64_t after, std::vector<Line>& lines);
// How many lines have been dropped from memory because more than kKeptLines were written.
uint64_t DroppedLines();
// The path the log is written to, or empty before Open.
const std::wstring& Path();

}
