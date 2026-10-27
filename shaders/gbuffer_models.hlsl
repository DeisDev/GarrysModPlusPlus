// G-buffer replacements for Source model shaders that have no LIGHTING_PREVIEW variant
// (vertexlit_and_unlit_generic_bump and skin). Inputs and registers follow those shaders, and the outputs
// match the stock LIGHTING_PREVIEW == 2 variants: oC0 albedo, oC1 world normal, oC3 coverage flag. oC2 is
// the self-illuminated colour, which the lighting pass adds unlit. All colours are linear.
//
// SELFILLUM selects the original shader's self-illumination: 0 none, 1 base alpha mask, 2 base alpha or
// mask texture (skin), 3 base alpha times a fresnel term. SKIN_DETAIL applies a skin detail texture in the
// $detailblendmode g_SkinDetailMode.x holds (TextureCombine and TextureCombinePostLighting in common_ps_fxc.h):
// 0 base times detail times 2, 1 detail added, 3 fade to detail, 5 detail added after lighting. FORWARD 1 builds
// the variant for draws after the capture, which writes the lit colour to oC0 alone
// (shaders/forward_light.hlsli).

#ifndef SELFILLUM
#define SELFILLUM 0
#endif
#ifndef FORWARD
#define FORWARD 0
#endif

#if FORWARD
#include "forward_light.hlsli"
// gbuffer::kModelProbeSamplers.
sampler3D ProbeRed : register(s5);
sampler3D ProbeGreen : register(s6);
sampler3D ProbeBlue : register(s7);
#define OUTPUT float4
#define OUTPUT_SEMANTIC : COLOR0
#else
#define OUTPUT GBufferOutput
#define OUTPUT_SEMANTIC
#endif

sampler BaseTextureSampler : register(s0);
sampler NormalSampler : register(s3);
float4 g_DiffuseModulation : register(c1);

// vertexlit_and_unlit_generic_bump
float4 g_BumpSelfIllumTint : register(c4);
float4 g_BumpSelfIllumScaleBiasExpBrightness : register(c11);

// skin; g_ShaderControls.x selects the base map alpha as the phong mask, which disables the normal map.
float4 g_SkinSelfIllumTint : register(c0);
float4 g_SkinSelfIllumScaleBiasExpBrightness : register(c3);
float4 g_SkinEnvMapFresnel : register(c10);
float4 g_ShaderControls : register(c27);
sampler SkinSelfIllumMaskSampler : register(s14);
sampler SkinDetailSampler : register(s13);
// gbuffer::kDetailModeRegister.
float4 g_SkinDetailMode : register(c216);

struct GBufferOutput
{
    float4 albedo : COLOR0;
    float4 normal : COLOR1;
    float4 emissive : COLOR2;
    float4 flags : COLOR3;
};

float SelfIllumFresnel(float3 vertexNormal, float3 toEye, float4 scaleBiasExpBrightness)
{
    float facing = saturate(dot(normalize(vertexNormal), normalize(toEye)));
    return saturate(pow(facing, scaleBiasExpBrightness.z) * scaleBiasExpBrightness.x + scaleBiasExpBrightness.y);
}

// The original shaders blend the lit diffuse colour towards tint * albedo by the mask, so the mask moves
// that share of the albedo from lighting to emission. `glow` is light added after lighting.
OUTPUT Output(float4 base, float3 normal, float3 mask, float3 emissiveTint, float3 worldVertToEye, float3 glow)
{
    float3 albedo = base.rgb * g_DiffuseModulation.rgb;
    float alpha = SELFILLUM != 0 ? g_DiffuseModulation.a : base.a * g_DiffuseModulation.a;
#if FORWARD
    return ForwardLight(float4(albedo * (1.0 - mask), alpha), normal, g_ForwardEye.xyz - worldVertToEye, emissiveTint * albedo * mask + glow,
        ProbeRed, ProbeGreen, ProbeBlue);
#else
    GBufferOutput output;
    output.albedo = float4(albedo * (1.0 - mask), alpha);
    output.normal = float4(normalize(normal), alpha);
    output.emissive = float4(emissiveTint * albedo * mask + glow, alpha);
    output.flags = float4(1.0, 1.0, 1.0, alpha);
    return output;
#endif
}

struct BumpInput
{
    float4 baseTexCoord : TEXCOORD0;
    float4 worldVertToEye : TEXCOORD2;
    float3 worldNormal : TEXCOORD3;
    float4 worldTangent : TEXCOORD4;
};

OUTPUT BumpMain(BumpInput i) OUTPUT_SEMANTIC
{
    float4 base = tex2D(BaseTextureSampler, i.baseTexCoord.xy);
    float3 tangentNormal = tex2D(NormalSampler, i.baseTexCoord.xy).xyz * 2.0 - 1.0;
    float3 binormal = cross(i.worldNormal, i.worldTangent.xyz) * i.worldTangent.w;
    float3 normal = tangentNormal.x * i.worldTangent.xyz + tangentNormal.y * binormal + tangentNormal.z * i.worldNormal;
    float3 mask = 0.0;
    float3 tint = g_BumpSelfIllumTint.rgb;
#if SELFILLUM == 1
    mask = base.a;
#elif SELFILLUM == 3
    mask = base.a * SelfIllumFresnel(i.worldNormal, i.worldVertToEye.xyz, g_BumpSelfIllumScaleBiasExpBrightness);
    tint *= g_BumpSelfIllumScaleBiasExpBrightness.w;
#endif
    return Output(base, normal, mask, tint, i.worldVertToEye.xyz, 0.0);
}

struct SkinInput
{
    float4 baseTexCoord : TEXCOORD0;
    float3 worldVertToEye : TEXCOORD2;
    float3x3 tangentSpaceTranspose : TEXCOORD3;
};

OUTPUT SkinMain(SkinInput i) OUTPUT_SEMANTIC
{
    float4 base = tex2D(BaseTextureSampler, i.baseTexCoord.xy);
    float3 glow = 0.0;
#if SKIN_DETAIL
    // c0.w is the detail blend factor.
    float4 detail = tex2D(SkinDetailSampler, i.baseTexCoord.zw);
    float blend = g_SkinSelfIllumTint.w;
    if (g_SkinDetailMode.x == 0.0)
        base.rgb *= lerp(1.0, 2.0 * detail.rgb, blend);
    else if (g_SkinDetailMode.x == 1.0)
        base.rgb += blend * detail.rgb;
    else if (g_SkinDetailMode.x == 3.0)
        base = lerp(base, detail, blend);
    else if (g_SkinDetailMode.x == 5.0)
        glow = blend * detail.rgb;
#endif
#if SKIN_NORMAL_MAP
    float3 tangentNormal = lerp(tex2D(NormalSampler, i.baseTexCoord.xy).xyz * 2.0 - 1.0, float3(0.0, 0.0, 1.0), g_ShaderControls.x);
#else
    float3 tangentNormal = float3(0.0, 0.0, 1.0);
#endif
    float3 mask = 0.0;
    float3 tint = g_SkinSelfIllumTint.rgb;
#if SELFILLUM == 2
    mask = lerp(base.aaa, tex2D(SkinSelfIllumMaskSampler, i.baseTexCoord.xy).rgb, g_SkinEnvMapFresnel.w);
#elif SELFILLUM == 3
    float3 vertexNormal = mul(i.tangentSpaceTranspose, float3(0.0, 0.0, 1.0));
    mask = base.a * SelfIllumFresnel(vertexNormal, i.worldVertToEye, g_SkinSelfIllumScaleBiasExpBrightness);
    tint *= g_SkinSelfIllumScaleBiasExpBrightness.w;
#endif
    return Output(base, mul(i.tangentSpaceTranspose, tangentNormal), mask, tint, i.worldVertToEye, glow);
}
