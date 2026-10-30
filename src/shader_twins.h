#pragma once

#include "vcs.h"

#include <windows.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace gmodpp::twins {

// The G-buffer twin (shaders/twins) of a game pixel shader combo, as Direct3D 9 bytecode compiled on first use; empty
// when the shader file has no twin, its .vcs header differs from the one the twin was made for, or the combo is left to
// the game's shader.
// Safe to call from several threads.
std::span<const DWORD> GBufferShader(std::string_view file, const vcs::Header& header, int staticIndex, int dynamicIndex);

// Names the twin sources' contents (shaders/twins as deployed), so caches built from them can tell they changed.
std::uint64_t SourceHash();

// The same twin for translucent draws: its fourth output holds the world position instead of flags, which
// shaders/forward_twin.hlsli lights (shader_translator.h, Variant::forwardLighting).
std::span<const DWORD> ForwardShader(std::string_view file, const vcs::Header& header, int staticIndex, int dynamicIndex);

// Whether a game shader file (.vcs name, any case) has a twin.
bool HasTwin(std::string_view file);

}
