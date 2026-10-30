#include "gbuffer_shaders.h"

#include "log.h"
#include "settings.h"

#include <g_forwardSnippetPS.h>
#include <g_forwardSnippetPS2.h>
#include <g_waterFlowSurfaceAlphaFogPS.h>
#include <g_waterFlowSurfaceColorLightmapPS.h>
#include <g_waterFlowSurfaceColorPS.h>
#include <g_waterFlowSurfaceLightmapFogPS.h>
#include <g_waterFlowSurfaceOpaqueFogPS.h>
#include <g_waterFlowSurfacePS.h>
#include <g_waterSurfaceAlphaFogPS.h>
#include <g_waterSurfaceLightmapFogPS.h>
#include <g_waterSurfaceOpaqueFogPS.h>
#include <g_waterSurfacePS.h>
#include <g_waterTexturedSurfacePS.h>

#include <d3d9types.h>

#include <algorithm>
#include <string_view>

namespace gmodpp::gbuffer {

namespace {

// Hammer's lighting preview assumes gamma-space textures and converts with pow(x, 2.2); the game samples
// them as sRGB already, so the conversion is turned into pow(x, 1).
constexpr float kPreviewGamma = 2.2f;
// LightmappedGeneric's g_TintValuesAndLightmapScale: $color times $color2 times the lightmap scale, which
// cLightScale.y also holds. The game multiplies its lightmap by it; the lighting preview leaves it out of the albedo.
constexpr UINT kLightmapTintRegister = 12;
// The ps_2_b snippet's constants start at c0 before the patch moves them.
constexpr UINT kSnippetConstantsPs2 = 0;
ClassStatistics g_statistics;
uint32_t g_tintFailures = 0;

std::span<const DWORD> Code(const BYTE* bytes)
{
    const auto* code = reinterpret_cast<const DWORD*>(bytes);
    return {code, bytecode::TokenCount(code)};
}

bool HasConstant(const std::vector<bytecode::Constant>& constants, std::string_view name)
{
    return std::any_of(constants.begin(), constants.end(), [&](const bytecode::Constant& c) { return c.name == name; });
}

bool HasRegister(const std::vector<bytecode::Constant>& constants, std::string_view name, bytecode::RegisterSet set, UINT index)
{
    return std::any_of(constants.begin(), constants.end(), [&](const bytecode::Constant& c) { return c.name == name && c.set == set && c.index == index; });
}

bool IsFlashlightPass(const std::vector<bytecode::Constant>& constants)
{
    return std::any_of(constants.begin(), constants.end(), [](const bytecode::Constant& c) {
        return c.set == bytecode::RegisterSet::Sampler && (c.name.find("Flashlight") != std::string::npos || c.name == "SpotSampler");
    });
}

// Skin (phong) shader constants that VertexLitGeneric's other shaders do not use.
bool IsSkinShader(const std::vector<bytecode::Constant>& constants)
{
    constexpr std::string_view kSkinOnly[] = {"NormalMapSampler", "SpecExponentSampler", "SpecularWarpSampler", "g_FresnelSpecParams",
        "g_SpecularRimParams", "g_EnvMapFresnel", "g_SelfIllumTint_and_DetailBlendFactorOrPhongAlbedoBoost"};
    return HasConstant(constants, "g_DiffuseModulation") &&
        std::any_of(std::begin(kSkinOnly), std::end(kSkinOnly), [&](std::string_view name) { return HasConstant(constants, name); });
}

// A lightmap ($lightmapwaterfog) only comes with the refraction's alpha fog.
bool LightmapFogFits(const std::vector<bytecode::Constant>& constants)
{
    return !HasConstant(constants, "LightmapSampler") ||
        (HasRegister(constants, "LightmapSampler", bytecode::RegisterSet::Sampler, 3) && !HasConstant(constants, "g_WaterFogParams"));
}

// The Water shader's combos that refract (water_ps20b), with the inputs and registers shaders/water_surface.hlsl reads.
// Flowing water has its own replacement (IsFlowingWater); textured water keeps the game's shader.
bool IsRefractingWater(const std::vector<bytecode::Constant>& constants)
{
    using bytecode::RegisterSet;
    return HasRegister(constants, "RefractSampler", RegisterSet::Sampler, 0) && HasRegister(constants, "NormalSampler", RegisterSet::Sampler, 4) &&
        HasRegister(constants, "g_ReflectRefractScale", RegisterSet::Float4, 5) && HasRegister(constants, "g_WaterFogColor", RegisterSet::Float4, 6) &&
        !HasConstant(constants, "FlowmapSampler") && !HasConstant(constants, "BaseTextureSampler") && LightmapFogFits(constants);
}

// The flowing ($flowmap) refracting combos without a base texture, with the registers shaders/water_flow_surface.hlsl
// reads.
bool IsFlowingWater(const std::vector<bytecode::Constant>& constants)
{
    using bytecode::RegisterSet;
    return HasRegister(constants, "RefractSampler", RegisterSet::Sampler, 0) && HasRegister(constants, "NormalSampler", RegisterSet::Sampler, 4) &&
        HasRegister(constants, "FlowmapSampler", RegisterSet::Sampler, 5) && HasRegister(constants, "FlowNoiseSampler", RegisterSet::Sampler, 6) &&
        HasRegister(constants, "g_vReflectTint", RegisterSet::Float4, 4) && HasRegister(constants, "g_ReflectRefractScale", RegisterSet::Float4, 5) &&
        HasRegister(constants, "g_WaterFogColor", RegisterSet::Float4, 6) && HasRegister(constants, "g_EyePos", RegisterSet::Float4, 9) &&
        HasRegister(constants, "g_vFlowParams1", RegisterSet::Float4, 13) && HasRegister(constants, "g_vFlowParams2", RegisterSet::Float4, 14) &&
        !HasConstant(constants, "BaseTextureSampler") && LightmapFogFits(constants);
}

// The Water shader's textured combos ($basetexture), which reflect but do not refract; flowing ones keep the game's
// shader.
bool IsTexturedWater(const std::vector<bytecode::Constant>& constants)
{
    using bytecode::RegisterSet;
    return HasRegister(constants, "BaseTextureSampler", RegisterSet::Sampler, 1) && HasRegister(constants, "NormalSampler", RegisterSet::Sampler, 4) &&
        HasRegister(constants, "g_vReflectTint", RegisterSet::Float4, 4) && !HasConstant(constants, "RefractSampler") &&
        !HasConstant(constants, "FlowmapSampler");
}

// The flowing textured combos, which flow their base texture instead of refracting (shaders/water_flow_surface.hlsl,
// FLOW_COLOR).
bool IsFlowingColorWater(const std::vector<bytecode::Constant>& constants)
{
    using bytecode::RegisterSet;
    return HasRegister(constants, "BaseTextureSampler", RegisterSet::Sampler, 1) && HasRegister(constants, "NormalSampler", RegisterSet::Sampler, 4) &&
        HasRegister(constants, "FlowmapSampler", RegisterSet::Sampler, 5) && HasRegister(constants, "FlowNoiseSampler", RegisterSet::Sampler, 6) &&
        HasRegister(constants, "g_vReflectTint", RegisterSet::Float4, 4) && HasRegister(constants, "g_WaterFogColor", RegisterSet::Float4, 6) &&
        HasRegister(constants, "g_EyePos", RegisterSet::Float4, 9) && HasRegister(constants, "g_vFlowParams1", RegisterSet::Float4, 13) &&
        HasRegister(constants, "g_vFlowParams2", RegisterSet::Float4, 14) && HasRegister(constants, "g_vColorFlowParams1", RegisterSet::Float4, 26) &&
        !HasConstant(constants, "RefractSampler") &&
        (!HasConstant(constants, "LightmapSampler") || HasRegister(constants, "LightmapSampler", RegisterSet::Sampler, 3));
}

// Which of a water replacement's variants (still or flowing) matches a combo: depth or alpha fog, and with
// $lightmapwaterfog whether the combo blends the refraction's colour or draws the lit fog colour alone. Combos without
// a lightmap that leave the refraction's colour out have no replacement.
std::span<const DWORD> WaterReplacement(std::span<const DWORD> code, const std::vector<bytecode::Constant>& constants, bool flowing)
{
    const bool refractionColor = bytecode::ReadsSampledColor(code, 0);
    if (HasConstant(constants, "LightmapSampler")) {
        if (!refractionColor)
            return Code(flowing ? g_waterFlowSurfaceOpaqueFogPS : g_waterSurfaceOpaqueFogPS);
        return Code(flowing ? g_waterFlowSurfaceLightmapFogPS : g_waterSurfaceLightmapFogPS);
    }
    if (!refractionColor)
        return {};
    if (!HasRegister(constants, "g_WaterFogParams", bytecode::RegisterSet::Float4, 7))
        return Code(flowing ? g_waterFlowSurfaceAlphaFogPS : g_waterSurfaceAlphaFogPS);
    return Code(flowing ? g_waterFlowSurfacePS : g_waterSurfacePS);
}

int RegisterOf(const std::vector<bytecode::Constant>& constants, std::initializer_list<std::string_view> names, bytecode::RegisterSet set, UINT limit)
{
    for (const bytecode::Constant& c : constants) {
        if (c.set != set || c.index >= limit)
            continue;
        for (std::string_view name : names) {
            if (c.name == name)
                return static_cast<int>(c.index);
        }
    }
    return -1;
}

FlashlightRegisters FlashlightRegistersOf(const std::vector<bytecode::Constant>& constants, UINT limit)
{
    using bytecode::RegisterSet;
    FlashlightRegisters r;
    r.position = RegisterOf(constants, {"g_FlashlightPos", "g_FlashlightPos_RimBoost", "g_vFlashlightPos"}, RegisterSet::Float4, limit);
    r.worldToTexture = RegisterOf(constants, {"g_FlashlightWorldToTexture"}, RegisterSet::Float4, limit - 3);
    r.attenuation = RegisterOf(constants, {"g_FlashlightAttenuationFactors", "g_FlashlightAttenuationFactors_RimMask", "g_FlashlightAttenuation",
        "g_vFlashlightAttenuationFactors"}, RegisterSet::Float4, limit);
    r.color = RegisterOf(constants, {"cFlashlightColor"}, RegisterSet::Float4, limit);
    r.cookie = RegisterOf(constants, {"FlashlightSampler", "SpotSampler", "g_tFlashlightCookieSampler"}, RegisterSet::Sampler, kTextureStages);
    return r;
}

}

PixelShaderClass ClassifyPixelShader(std::span<const DWORD> code, UINT registerLimit)
{
    PixelShaderClass result;
    if (code.empty())
        return result;
    const std::vector<bytecode::Constant> constants = bytecode::ConstantTable(code);

    if (bytecode::WritesColorOutput(code, kNormalTarget) && bytecode::WritesColorOutput(code, kFlagsTarget)) {
        result.kind = ShaderKind::GBuffer;
        result.patched.assign(code.begin(), code.end());
        bytecode::ReplaceDefLiteral(result.patched, kPreviewGamma, 1.0f);
        if (HasRegister(constants, "g_TintValuesAndLightmapScale", bytecode::RegisterSet::Float4, kLightmapTintRegister)) {
            if (std::optional<std::vector<DWORD>> tinted = bytecode::TintColorOutput(result.patched, kLightmapTintRegister, kLightScaleRegister))
                result.patched = std::move(*tinted);
            else if (++g_tintFailures <= 5)
                log::Info("G-buffer: a LightmappedGeneric preview shader keeps its albedo without $color and $color2");
        }
        // The same shader lights translucent surfaces after the capture once the snippet takes its outputs.
        const bool shaderModel3 = D3DSHADER_VERSION_MAJOR(result.patched[0]) >= 3;
        const char* reason = "";
        if (std::optional<bytecode::ForwardPatch> patch = bytecode::AppendForwardLighting(result.patched,
                Code(shaderModel3 ? g_forwardSnippetPS : g_forwardSnippetPS2), shaderModel3 ? kForwardConstants : kSnippetConstantsPs2, reason)) {
            result.forward = std::move(patch->code);
            result.probeSamplers = patch->samplers;
            result.forwardUniforms = patch->uniforms;
            ++g_statistics.forward;
        } else if (++g_statistics.forwardFailures <= 5) {
            log::Info("G-buffer: a preview shader gets no forward-lit variant: {}", reason);
        }
        ++g_statistics.patched;
        ++GetStatistics().gbufferShaders;
        return result;
    }

    if (HasConstant(constants, "ShadowSampler") && HasConstant(constants, "g_ShadowColor")) {
        result.kind = ShaderKind::ProjectedShadow;
        return result;
    }
    if (IsFlashlightPass(constants)) {
        result.kind = ShaderKind::Flashlight;
        result.flashlight = FlashlightRegistersOf(constants, registerLimit);
        return result;
    }
    if (IsFlowingColorWater(constants)) {
        result.kind = ShaderKind::Water;
        result.replacement = Code(HasConstant(constants, "LightmapSampler") ? g_waterFlowSurfaceColorLightmapPS : g_waterFlowSurfaceColorPS);
        return result;
    }
    if (IsTexturedWater(constants)) {
        // A surface lit like the world's (shaders/water_textured_surface.hlsl).
        const std::span<const DWORD> surface = Code(g_waterTexturedSurfacePS);
        result.kind = ShaderKind::GBuffer;
        result.patched.assign(surface.begin(), surface.end());
        return result;
    }
    if (IsRefractingWater(constants) || IsFlowingWater(constants)) {
        result.replacement = WaterReplacement(code, constants, HasConstant(constants, "FlowmapSampler"));
        if (!result.replacement.empty()) {
            result.kind = ShaderKind::Water;
            return result;
        }
    }
    // Model shaders without a lighting preview combo (bumped VertexLitGeneric, skin) are drawn by their G-buffer
    // twins (shader_twins.h); without one they keep the game's shader.
    if ((HasConstant(constants, "g_DiffuseModulation") && HasConstant(constants, "BumpmapSampler")) || IsSkinShader(constants)) {
        result.kind = ShaderKind::GBuffer;
        ++g_statistics.replaced;
        ++GetStatistics().replacedShaders;
    }
    return result;
}

VertexShaderClass ClassifyVertexShader(std::span<const DWORD> code, UINT registerLimit)
{
    VertexShaderClass result;
    if (code.empty())
        return result;
    const std::vector<bytecode::Constant> constants = bytecode::ConstantTable(code);
    result.flashlight = FlashlightRegistersOf(constants, registerLimit);
    if (HasConstant(constants, "g_vMiscParams4"))
        result.swayRegister = RegisterOf(constants, {"g_vMiscParams2"}, bytecode::RegisterSet::Float4, registerLimit);
    return result;
}

const ClassStatistics& ShaderStatistics() { return g_statistics; }

}
