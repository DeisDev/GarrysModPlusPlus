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

// True when the first use of the colour of the shader's sample of sampler `index` remaps it as
// sample * 2 - 1 with `def` constants, as TextureCombine's TCOMBINE_RGB_EQUALS_BASE_x_DETAILx2 compiles.
bool ModulatesSampleBy2x(std::span<const DWORD> code, UINT index);

// The $detailblendmode (TextureCombine in Source's common_ps_fxc.h) a shader applies to its sample of sampler
// `index`, found from the first instruction that uses the sample's colour: 0 (times 2 - 1, then multiplied in),
// 1 (added with the blend factor `factorRegister`.w), 5 (the same, after lighting: more than `lateGap`
// instructions after the sample) or 3 (lerp towards it by the blend factor); -1 for anything else.
int DetailBlendMode(std::span<const DWORD> code, UINT index, UINT factorRegister, size_t lateGap);

// Replaces every `def` literal equal to `from` with `to`; returns how many were replaced.
int ReplaceDefLiteral(std::span<DWORD> code, float from, float to);

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
