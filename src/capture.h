#pragma once

#include <windows.h>

#include <filesystem>

namespace gmodpp {

// Writes 32-bit BGRA or BGRX rows as a 24-bit top-down BMP.
bool WriteBmp(const std::filesystem::path& path, UINT width, UINT height, const void* bgra, size_t pitch);

}
