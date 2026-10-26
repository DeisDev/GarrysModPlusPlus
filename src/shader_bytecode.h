#pragma once

#include <windows.h>

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

// Replaces every `def` literal equal to `from` with `to`; returns how many were replaced.
int ReplaceDefLiteral(std::span<DWORD> code, float from, float to);

}
