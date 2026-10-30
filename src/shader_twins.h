#pragma once

#include "vcs.h"

#include <windows.h>

#include <span>
#include <string_view>

namespace gmodpp::twins {

// The G-buffer twin (shaders/twins) of a game pixel shader combo, as Direct3D 9 bytecode compiled on first use; empty
// when the shader file has no twin, its .vcs header differs from the one the twin was made for, or the combo is left to
// the game's shader.
std::span<const DWORD> GBufferShader(std::string_view file, const vcs::Header& header, int staticIndex, int dynamicIndex);

}
