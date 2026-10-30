#include "gbuffer_shaders.h"

#include "log.h"
#include "settings.h"

#include <g_forwardBumpPS.h>
#include <g_forwardBumpSelfIllumFresnelPS.h>
#include <g_forwardBumpSelfIllumPS.h>
#include <g_forwardSkinDetailPS.h>
#include <g_forwardSkinDetailSelfIllumFresnelPS.h>
#include <g_forwardSkinDetailSelfIllumPS.h>
#include <g_forwardSkinNoBumpDetailPS.h>
#include <g_forwardSkinNoBumpDetailSelfIllumFresnelPS.h>
#include <g_forwardSkinNoBumpDetailSelfIllumPS.h>
#include <g_forwardSkinNoBumpPS.h>
#include <g_forwardSkinNoBumpSelfIllumFresnelPS.h>
#include <g_forwardSkinNoBumpSelfIllumPS.h>
#include <g_forwardSkinPS.h>
#include <g_forwardSkinSelfIllumFresnelPS.h>
#include <g_forwardSkinSelfIllumPS.h>
#include <g_forwardSnippetPS.h>
#include <g_forwardSnippetPS2.h>
#include <g_gbufferBumpPS.h>
#include <g_gbufferBumpSelfIllumFresnelPS.h>
#include <g_gbufferBumpSelfIllumPS.h>
#include <g_gbufferSkinDetailPS.h>
#include <g_gbufferSkinDetailSelfIllumFresnelPS.h>
#include <g_gbufferSkinDetailSelfIllumPS.h>
#include <g_gbufferSkinNoBumpDetailPS.h>
#include <g_gbufferSkinNoBumpDetailSelfIllumFresnelPS.h>
#include <g_gbufferSkinNoBumpDetailSelfIllumPS.h>
#include <g_gbufferSkinNoBumpPS.h>
#include <g_gbufferSkinNoBumpSelfIllumFresnelPS.h>
#include <g_gbufferSkinNoBumpSelfIllumPS.h>
#include <g_gbufferSkinPS.h>
#include <g_gbufferSkinSelfIllumFresnelPS.h>
#include <g_gbufferSkinSelfIllumPS.h>
#include <g_waterSurfaceAlphaFogPS.h>
#include <g_waterSurfacePS.h>

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
// The samplers shaders/gbuffer_models.hlsl reads the probe volume from in its FORWARD variants, and its constants.
constexpr std::array<UINT, 3> kModelProbeSamplers = {5, 6, 7};
constexpr std::array<UINT, bytecode::kForwardUniforms> kModelForwardUniforms = {217, 218, 219, 220, 221, 222, 223};
constexpr UINT kSkinDetailSampler = 13;
// The skin shader's g_SelfIllumTint_and_DetailBlendFactor (its .w is the detail blend factor).
constexpr UINT kSkinDetailFactor = 0;
// A post-lighting detail add comes after the lighting math; a pre-lighting one right after the sample.
constexpr size_t kLateDetailGap = 12;

enum class SelfIllum { None, Mask, Fresnel };

// Indexed by [shader][SelfIllum].
enum class ReplacedShader { Bump, Skin, SkinNoBump, SkinDetail, SkinNoBumpDetail, Count };
const BYTE* const kReplacementCode[][3] = {
    {g_gbufferBumpPS, g_gbufferBumpSelfIllumPS, g_gbufferBumpSelfIllumFresnelPS},
    {g_gbufferSkinPS, g_gbufferSkinSelfIllumPS, g_gbufferSkinSelfIllumFresnelPS},
    {g_gbufferSkinNoBumpPS, g_gbufferSkinNoBumpSelfIllumPS, g_gbufferSkinNoBumpSelfIllumFresnelPS},
    {g_gbufferSkinDetailPS, g_gbufferSkinDetailSelfIllumPS, g_gbufferSkinDetailSelfIllumFresnelPS},
    {g_gbufferSkinNoBumpDetailPS, g_gbufferSkinNoBumpDetailSelfIllumPS, g_gbufferSkinNoBumpDetailSelfIllumFresnelPS},
};
const BYTE* const kForwardReplacementCode[][3] = {
    {g_forwardBumpPS, g_forwardBumpSelfIllumPS, g_forwardBumpSelfIllumFresnelPS},
    {g_forwardSkinPS, g_forwardSkinSelfIllumPS, g_forwardSkinSelfIllumFresnelPS},
    {g_forwardSkinNoBumpPS, g_forwardSkinNoBumpSelfIllumPS, g_forwardSkinNoBumpSelfIllumFresnelPS},
    {g_forwardSkinDetailPS, g_forwardSkinDetailSelfIllumPS, g_forwardSkinDetailSelfIllumFresnelPS},
    {g_forwardSkinNoBumpDetailPS, g_forwardSkinNoBumpDetailSelfIllumPS, g_forwardSkinNoBumpDetailSelfIllumFresnelPS},
};

ClassStatistics g_statistics;
uint32_t g_tintFailures = 0;
// Skin shaders seen with each detail blend mode (-1 counted at 7).
std::array<uint32_t, 8> g_detailModes = {};

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

// The Water shader's combos that refract (water_ps20b), with the inputs and registers shaders/water_surface.hlsl reads.
// Flowing, lightmapped and textured water keep the game's shader.
bool IsRefractingWater(const std::vector<bytecode::Constant>& constants)
{
    using bytecode::RegisterSet;
    return HasRegister(constants, "RefractSampler", RegisterSet::Sampler, 0) && HasRegister(constants, "NormalSampler", RegisterSet::Sampler, 4) &&
        HasRegister(constants, "g_ReflectRefractScale", RegisterSet::Float4, 5) && HasRegister(constants, "g_WaterFogColor", RegisterSet::Float4, 6) &&
        !HasConstant(constants, "FlowmapSampler") && !HasConstant(constants, "LightmapSampler") && !HasConstant(constants, "BaseTextureSampler");
}

// The replacements read self-illumination from the registers the stock shaders use, so a shader whose
// registers differ gets no self-illumination rather than wrong values.
SelfIllum BumpSelfIllum(const std::vector<bytecode::Constant>& constants)
{
    using bytecode::RegisterSet;
    if (!HasRegister(constants, "g_SelfIllumTint_and_BlendFactor", RegisterSet::Float4, 4))
        return SelfIllum::None;
    if (HasRegister(constants, "g_SelfIllumScaleBiasExpBrightness", RegisterSet::Float4, 11))
        return SelfIllum::Fresnel;
    // With a detail texture the tint register also holds the detail blend factor, so it does not show whether
    // the material is self-illuminated.
    return HasConstant(constants, "DetailSampler") ? SelfIllum::None : SelfIllum::Mask;
}

SelfIllum SkinSelfIllum(const std::vector<bytecode::Constant>& constants)
{
    using bytecode::RegisterSet;
    if (!HasRegister(constants, "g_SelfIllumTint_and_DetailBlendFactorOrPhongAlbedoBoost", RegisterSet::Float4, 0))
        return SelfIllum::None;
    if (HasRegister(constants, "SelfIllumMaskSampler", RegisterSet::Sampler, 14) && HasRegister(constants, "g_EnvMapFresnel", RegisterSet::Float4, 10))
        return SelfIllum::Mask;
    if (HasRegister(constants, "g_SelfIllumScaleBiasExpBrightness", RegisterSet::Float4, 3))
        return SelfIllum::Fresnel;
    return SelfIllum::None;
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
    if (IsRefractingWater(constants)) {
        const bool alphaFog = !HasRegister(constants, "g_WaterFogParams", bytecode::RegisterSet::Float4, 7);
        result.kind = ShaderKind::Water;
        result.replacement = Code(alphaFog ? g_waterSurfaceAlphaFogPS : g_waterSurfacePS);
        return result;
    }
    SelfIllum selfIllum = SelfIllum::None;
    ReplacedShader replaced = ReplacedShader::Count;
    if (HasConstant(constants, "g_DiffuseModulation") && HasConstant(constants, "BumpmapSampler")) {
        selfIllum = BumpSelfIllum(constants);
        replaced = ReplacedShader::Bump;
    } else if (IsSkinShader(constants)) {
        selfIllum = SkinSelfIllum(constants);
        const bool normalMap = HasConstant(constants, "NormalMapSampler");
        // Detail blend modes the replacement does not know leave the base texture alone.
        if (HasRegister(constants, "DetailSampler", bytecode::RegisterSet::Sampler, kSkinDetailSampler)) {
            result.detailMode = bytecode::DetailBlendMode(code, kSkinDetailSampler, kSkinDetailFactor, kLateDetailGap);
            if (g_detailModes[result.detailMode >= 0 ? result.detailMode : 7]++ == 0)
                log::Info("G-buffer: skin shader with detail blend mode {}", result.detailMode);
        }
        const bool detail = result.detailMode >= 0;
        replaced = detail ? (normalMap ? ReplacedShader::SkinDetail : ReplacedShader::SkinNoBumpDetail)
                          : (normalMap ? ReplacedShader::Skin : ReplacedShader::SkinNoBump);
    }
    if (replaced == ReplacedShader::Count)
        return result;
    const size_t s = static_cast<size_t>(replaced);
    const size_t v = static_cast<size_t>(selfIllum);
    result.kind = ShaderKind::GBuffer;
    result.replacement = Code(kReplacementCode[s][v]);
    result.forwardReplacement = Code(kForwardReplacementCode[s][v]);
    result.probeSamplers = kModelProbeSamplers;
    result.forwardUniforms = kModelForwardUniforms;
    result.emissive = selfIllum != SelfIllum::None || result.detailMode == 5;
    ++g_statistics.replaced;
    ++GetStatistics().replacedShaders;
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
