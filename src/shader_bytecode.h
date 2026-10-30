#pragma once

#include <windows.h>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gmodpp::bytecode {

enum class RegisterSet { Bool, Int4, Float4, Sampler };

struct Constant {
    std::string name;
    RegisterSet set;
    UINT index;
    UINT count;
};

// Reads the D3DX constant table (CTAB comment) that Source keeps in its compiled shaders.
std::vector<Constant> ConstantTable(std::span<const DWORD> code);

// Size of a D3D9 shader token stream in DWORDs, including the end token; 0 when it is malformed or older
// than shader model 2.0 (whose instructions cannot be walked without a per-opcode table).
size_t TokenCount(const DWORD* code);

// True when the shader writes the given colour output register (oCn).
bool WritesColorOutput(std::span<const DWORD> code, UINT index);

// Replaces every `def` literal equal to `from` with `to`; returns how many were replaced.
int ReplaceDefLiteral(std::span<DWORD> code, float from, float to);

// A pixel shader (ps_3_0 or ps_2_b) whose oC0.rgb is multiplied by c[tintRegister].rgb / c[scaleRegister].y; nothing
// when it does not write oC0, has too few free temporary registers or cannot be walked.
std::optional<std::vector<DWORD>> TintColorOutput(std::span<const DWORD> shader, UINT tintRegister, UINT scaleRegister);

inline constexpr UINT kForwardUniforms = 7;

struct ForwardPatch {
    std::vector<DWORD> code;
    // The samplers the snippet's s0-s2 moved to.
    std::array<UINT, 3> samplers;
    // The registers the snippet's kForwardUniforms uniform constants moved to.
    std::array<UINT, kForwardUniforms> uniforms;
};

// A pixel shader (ps_3_0 or ps_2_b) that writes oC0-oC2 with its colour outputs moved to free temporary registers
// and `snippet`, compiled for the same model, appended to write oC0 instead. The snippet's TEXCOORD0-2 inputs
// stand for the moved oC0-oC2, its samplers s0-s2 and its uniform constants (kForwardUniforms registers from
// `uniformBase`) move to registers the shader leaves free, and so do its literal constants. Nothing when the
// shader cannot take it, with the reason in `reason`.
std::optional<ForwardPatch> AppendForwardLighting(std::span<const DWORD> shader, std::span<const DWORD> snippet, UINT uniformBase, const char*& reason);

}
