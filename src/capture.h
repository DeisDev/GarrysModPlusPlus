#pragma once

#include <d3d9.h>

#include <filesystem>

namespace gmodpp {

// Writes 32-bit BGRA or BGRX rows as a 24-bit top-down BMP.
bool WriteBmp(const std::filesystem::path& path, UINT width, UINT height, const void* bgra, size_t pitch);
bool CaptureBackBuffer(IDirect3DDevice9* device, const std::filesystem::path& path);

}
