#pragma once

#include <d3d9.h>

#include <filesystem>

namespace gmodpp {

bool CaptureBackBuffer(IDirect3DDevice9* device, const std::filesystem::path& path);

}
