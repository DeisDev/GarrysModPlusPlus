#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gmodpp::translator {

enum class Stage { Vertex, Pixel };
enum class TextureType : std::uint8_t { None, Texture2D, Cube, Volume };

// What the same bytecode compiles to differently depending on the draw.
struct Variant {
    // Samplers bound to depth textures, which D3D9 samples as a depth comparison (hardware shadow maps).
    std::uint16_t shadowSamplers = 0;
    // Vertex shader input registers fed from integer vertex formats (UBYTE4, SHORT2), declared uint4 (int4 when also
    // in signedInputs) and converted.
    std::uint16_t integerInputs = 0;
    std::uint16_t signedInputs = 0;
    // Pixel shaders: Direct3D 9's alpha test as a D3DCMPFUNC (0 off), against gmodpp_alpha.x (ALPHAREF, 0-255).
    std::uint8_t alphaTest = 0;
    // Vertex shaders: how many of gmodpp_clipPlanes (clip-space user clip planes) clip the output.
    std::uint8_t clipPlanes : 7 = 0;
    // Pixel shaders writing a forward twin's albedo, normal, emissive and world position (oC0-oC3): the outputs are
    // lit in the shader (shaders/forward_twin.hlsli) and written to SV_Target0 alone.
    std::uint8_t forwardLighting : 1 = 0;
};

struct VertexInput {
    BYTE usage;
    BYTE usageIndex;
    BYTE reg;
};

struct Translation {
    Stage stage = Stage::Vertex;
    std::string hlsl;
    // The float constants the shader reads, c[first] to c[first + count - 1]; its constant buffer starts at c[first].
    UINT firstFloatConstant = 0;
    UINT floatConstantCount = 0;
    std::array<TextureType, 16> samplers = {};
    std::vector<VertexInput> inputs;
    UINT colorOutputs = 0;
    bool writesDepth = false;
};

// Translates a D3D9 vertex or pixel shader (models 2.0 to 3.0) to HLSL for shader model 6.6. The binding model is
// described in shader_translator.cpp. Returns false with the reason in `error`.
bool Translate(std::span<const DWORD> code, const Variant& variant, Translation& out, std::string& error);

}
