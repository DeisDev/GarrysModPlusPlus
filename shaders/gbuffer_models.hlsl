// G-buffer replacements for Source model shaders that have no LIGHTING_PREVIEW variant
// (vertexlit_and_unlit_generic_bump and skin). Inputs and registers follow those shaders, and the outputs
// match the stock LIGHTING_PREVIEW == 2 variants: oC0 albedo, oC1 world normal, oC3 coverage flag. oC2 is
// the self-illuminated colour, which the lighting pass adds unlit. All colours are linear.
//
// SELFILLUM selects the original shader's self-illumination: 0 none, 1 base alpha mask, 2 base alpha or
// mask texture (skin), 3 base alpha times a fresnel term. SKIN_DETAIL applies a skin detail texture in
// $detailblendmode 0 (base times detail times 2).

#ifndef SELFILLUM
#define SELFILLUM 0
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
// that share of the albedo from lighting to emission.
GBufferOutput Output(float4 base, float3 normal, float3 mask, float3 emissiveTint)
{
    GBufferOutput output;
    float3 albedo = base.rgb * g_DiffuseModulation.rgb;
    float alpha = SELFILLUM != 0 ? g_DiffuseModulation.a : base.a * g_DiffuseModulation.a;
    output.albedo = float4(albedo * (1.0 - mask), alpha);
    output.normal = float4(normalize(normal), alpha);
    output.emissive = float4(emissiveTint * albedo * mask, alpha);
    output.flags = float4(1.0, 1.0, 1.0, alpha);
    return output;
}

struct BumpInput
{
    float4 baseTexCoord : TEXCOORD0;
    float4 worldVertToEye : TEXCOORD2;
    float3 worldNormal : TEXCOORD3;
    float4 worldTangent : TEXCOORD4;
};

GBufferOutput BumpMain(BumpInput i)
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
    return Output(base, normal, mask, tint);
}

struct SkinInput
{
    float4 baseTexCoord : TEXCOORD0;
    float3 worldVertToEye : TEXCOORD2;
    float3x3 tangentSpaceTranspose : TEXCOORD3;
};

GBufferOutput SkinMain(SkinInput i)
{
    float4 base = tex2D(BaseTextureSampler, i.baseTexCoord.xy);
#if SKIN_DETAIL
    // TextureCombine's TCOMBINE_RGB_EQUALS_BASE_x_DETAILx2; c0.w is the detail blend factor.
    base.rgb *= lerp(1.0, 2.0 * tex2D(SkinDetailSampler, i.baseTexCoord.zw).rgb, g_SkinSelfIllumTint.w);
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
    return Output(base, mul(i.tangentSpaceTranspose, tangentNormal), mask, tint);
}
