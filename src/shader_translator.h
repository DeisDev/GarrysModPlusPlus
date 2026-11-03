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
    std::uint8_t alphaTest : 4 = 0;
    // Pixel shaders of draws Source leaves unfogged (particles): seen through the volumetric light's medium as
    // gmodpp_alpha.z says the draw blends (gmodpp_FogBlend in shaders/game_fog.hlsli).
    std::uint8_t fogEpilogue : 1 = 0;
    // Vertex shaders: how many of gmodpp_clipPlanes (clip-space user clip planes) clip the output.
    std::uint8_t clipPlanes : 3 = 0;
    // Pixel shaders writing a forward twin's albedo, normal, emissive and world position (oC0-oC3): the outputs are
    // lit in the shader (shaders/forward_twin.hlsli) and written to SV_Target0 alone.
    std::uint8_t forwardLighting : 1 = 0;
    // Pixel shaders writing oC0 alone that also write 0 to SV_Target1, the G-buffer's coverage, so the lighting leaves
    // what they draw as the game drew it.
    std::uint8_t clearCoverage : 1 = 0;
    // With clearCoverage, write a mirror's mark, (0, 0, 1, 0), instead (IsMirror in shaders/water.hlsli).
    std::uint8_t markMirror : 1 = 0;
    // G-buffer twins of light-warped materials: the flags target's alpha (oC3.a) takes gmodpp_alpha.y, the warp
    // texture's slot in the view's table (shaderapi_capture.h, LightWarpSlot).
    std::uint8_t lightWarpSlot : 1 = 0;
    // With forwardLighting, on devices with opacity micromaps: compiled for shader model 6.9 with OPACITY_MICROMAPS, so
    // the lighting's ray queries may meet the micromaps of the scene they trace (shaders/scene.hlsli, SCENE_RAY_QUERY).
    std::uint8_t opacityMicromaps : 1 = 0;
    // Zero, so that variants compare equal byte for byte.
    std::uint8_t unused : 3 = 0;
};

// The target a translation compiles for.
const wchar_t* Profile(Stage stage, const Variant& variant);

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

// Whether a translation includes shaders/game_fog.hlsli (its fog colour or fog epilogue), so its cache key must follow
// the lighting headers.
bool UsesGameFog(std::span<const DWORD> code, const Variant& variant);

}
