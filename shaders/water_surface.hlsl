// Replaces the game's Water pixel shader (water_ps20b's refracting combos) during the G-buffer capture: instead of
// shading the surface it writes what shaders/water.hlsli needs into the G-buffer's targets, which hold no surface there.
// The inputs, samplers and constants are the game shader's (the Source SDK's water_ps2x.fxc, as GMod's compiled
// combos declare them): t0 the normal map's coordinates, t1 the eye vector in the surface's tangent space, t2 the
// projected reflection (xy) and refraction (wz) coordinates and t3 their w (w) and the water fog depth (z).
// Combos with g_WaterFogParams fog the refraction by that depth; the others (REFRACT_ALPHA_FOG) by the refraction's
// alpha, where the refraction view wrote the water's depth, which also scales the distortion and fades reflections out
// at the shore.
// oC0: the refraction fogged by the water, as the game would write it; oC1: the tangent-space normal's slope (xy), the
// reflect tint's brightness and the direction of the tangent-space eye vector (an angle, or kNoEyeAngle where the game's
// vertex shader gives none); oC3: g 1 (water), a the share of the reflection the shore leaves. The emissive target is
// left alone: surfaces drawn over the water later do not write it, and would keep what the water wrote.

sampler RefractSampler : register(s0);
sampler NormalSampler : register(s4);

float4 g_ReflectTint : register(c4);
// xy: reflection distortion, zw: refraction distortion.
float4 g_ReflectRefractScale : register(c5);
float4 g_WaterFogColor : register(c6);
// x: fog start, y: fog end minus start.
float4 g_WaterFogParams : register(c7);
float4 cLightScale : register(c30);

struct Input
{
    float2 bumpCoord : TEXCOORD0;
    float3 tangentEye : TEXCOORD1;
    float4 reflectRefract : TEXCOORD2;
    float4 projection : TEXCOORD3;
};

struct Output
{
    float4 refraction : COLOR0;
    float4 normal : COLOR1;
    // Outputs must be contiguous; the emissive target's writes are disabled for water draws.
    float4 emissive : COLOR2;
    float4 flags : COLOR3;
};

static const float kNoEyeAngle = 10.0;

Output main(Input input)
{
    float4 normalMap = tex2D(NormalSampler, input.bumpCoord);
    float3 normal = normalMap.xyz * 2.0 - 1.0;
    float2 screenCoord = input.reflectRefract.wz / input.projection.w;
#if REFRACT_ALPHA_FOG
    float2 distortion = g_ReflectRefractScale.wz * tex2D(RefractSampler, screenCoord).a;
#else
    float2 distortion = g_ReflectRefractScale.wz;
#endif
    float4 refraction = tex2D(RefractSampler, screenCoord + normal.xy * normalMap.a * distortion);
#if REFRACT_ALPHA_FOG
    float fog = saturate(refraction.a - 0.05);
    float shore = saturate((refraction.a - 0.05) * 20.0);
#else
    float fog = saturate((input.projection.z - g_WaterFogParams.x) / g_WaterFogParams.y);
    float shore = 1.0;
#endif
    refraction.rgb = lerp(refraction.rgb, g_WaterFogColor.rgb * cLightScale.x, fog);

    Output output;
    output.refraction = float4(refraction.rgb, 1.0);
    float eyeAngle = dot(input.tangentEye.xy, input.tangentEye.xy) > 1e-6 ? atan2(input.tangentEye.y, input.tangentEye.x) : kNoEyeAngle;
    output.normal = float4(normal.xy, dot(g_ReflectTint.rgb, 1.0 / 3.0), eyeAngle);
    output.emissive = 0.0;
    output.flags = float4(0.0, 1.0, 0.0, shore);
    return output;
}
