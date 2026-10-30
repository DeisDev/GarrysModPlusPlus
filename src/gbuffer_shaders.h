#pragma once

#include "shader_bytecode.h"

#include <windows.h>

#include <array>
#include <span>
#include <vector>

// How the G-buffer capture treats each of the game's Direct3D 9 shaders, worked out from their constant tables, and the
// code drawn in their place. Shared by the Direct3D 9 build (gbuffer.cpp) and the native backend (shaderapi_capture.cpp).
namespace gmodpp::gbuffer {

// ProjectedShadow: Source's render-to-texture prop shadows, which would darken the albedo; ray-traced shadows
// replace them.
// Water: the refracting combos of the Water shader, whose surface shaders/water_surface.hlsl writes into the G-buffer.
enum class ShaderKind { Other, GBuffer, Flashlight, ProjectedShadow, Water };

// Where a flashlight shader keeps the projected light's parameters, or -1.
struct FlashlightRegisters {
    int position = -1;
    int worldToTexture = -1;
    int attenuation = -1;
    int color = -1;
    int cookie = -1;
};

// Albedo, normal, self-illumination and flags (coverage) of G-buffer draws go to render targets 0 to 3.
inline constexpr UINT kNormalTarget = 1;
inline constexpr UINT kEmissiveTarget = 2;
inline constexpr UINT kFlagsTarget = 3;
// Pixel constants: the Water shader's g_WaterFogColor (shaders/water_surface.hlsl), cLightScale, g_LinearFogColor,
// and g_SkinDetailMode in shaders/gbuffer_models.hlsl.
inline constexpr UINT kWaterFogColorRegister = 6;
inline constexpr UINT kLightScaleRegister = 30;
inline constexpr UINT kFogColorRegister = 29;
inline constexpr UINT kDetailModeRegister = 216;
// Vertex constants: cFogParams, and Source's model-view-projection (c4-c7) and view-projection (c8-c11) rows;
// clip = dot(row, position).
inline constexpr UINT kVertexFogRegister = 16;
inline constexpr UINT kModelViewProjection = 4;
inline constexpr UINT kViewProjection = 8;
inline constexpr UINT kProjectionRegisters = 8;
// The pixel shader constants shaders/forward_light.hlsli reads in our ps_3_0 shaders (c217-c223).
inline constexpr UINT kForwardConstants = 217;
inline constexpr UINT kTextureStages = 16;

struct PixelShaderClass {
    ShaderKind kind = ShaderKind::Other;
    FlashlightRegisters flashlight;
    // A lighting preview shader made to write the G-buffer (gamma and tint patches), drawn in place of the game's.
    std::vector<DWORD> patched;
    // Our shader drawn in place of the game's during a capture (model shaders without a preview combo, refracting
    // water), and the one lighting it from the probe volume after the capture; static code.
    std::span<const DWORD> replacement;
    std::span<const DWORD> forwardReplacement;
    // The patched preview shader with the probe volume lighting appended, for translucent surfaces.
    std::vector<DWORD> forward;
    // The samplers the forward variant reads the volume from and the registers of its constants
    // (shaders/forward_light.hlsli).
    std::array<UINT, 3> probeSamplers = {};
    std::array<UINT, bytecode::kForwardUniforms> forwardUniforms = {};
    // The capture shader writes self-illumination to the emissive target.
    bool emissive = false;
    // The skin detail blend mode its replacement applies (shaders/gbuffer_models.hlsl), or -1.
    int detailMode = -1;
};

// `registerLimit`: flashlight parameters in registers from this one on are not tracked.
PixelShaderClass ClassifyPixelShader(std::span<const DWORD> code, UINT registerLimit);

struct VertexShaderClass {
    FlashlightRegisters flashlight;
    // The tree sway shader's g_vMiscParams2 register (y time, zw wind), or -1.
    int swayRegister = -1;
};

VertexShaderClass ClassifyVertexShader(std::span<const DWORD> code, UINT registerLimit);

// Shaders patched and replaced so far.
struct ClassStatistics {
    uint32_t patched = 0;
    uint32_t replaced = 0;
    uint32_t forward = 0;
    uint32_t forwardFailures = 0;
};
const ClassStatistics& ShaderStatistics();

}
