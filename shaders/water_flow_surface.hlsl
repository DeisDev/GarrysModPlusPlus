// Replaces the game's flowing Water pixel shader (water_ps20b's refracting combos with $flowmap) during the G-buffer
// capture, writing what shaders/water_surface.hlsl writes. GMod's flow is not in the Source SDK 2013; it follows GMod's
// compiled combos: the normal map is sampled twice, scrolled along the flow map half a period apart and moved to a new
// offset each period, and the two are blended by a triangle wave; the noise texture shifts each texel's phase. The
// flow's strength scales the bump. Inputs: t0 the flow map's coordinates, t1 the eye vector in the surface's tangent
// space, t2 the projected reflection (xy) and refraction (wz) coordinates, t3 their w (w) and the water fog depth (z),
// t4 the world position (xy) and t7 the lightmap coordinates.
// Combos with g_WaterFogParams fog the refraction by that depth; the others (REFRACT_ALPHA_FOG) by the refraction's
// alpha. With $lightmapwaterfog (LIGHTMAP_FOG) the fog colour is lit by the lightmap and, as GMod's combos do, scaled
// by the tonemap scale twice; some of those combos (OPAQUE_FOG) draw the lit fog colour alone, without the refraction.

sampler RefractSampler : register(s0);
sampler LightmapSampler : register(s3);
sampler NormalSampler : register(s4);
sampler FlowmapSampler : register(s5);
sampler FlowNoiseSampler : register(s6);

float4 g_ReflectTint : register(c4);
// xy: reflection distortion, zw: refraction distortion.
float4 g_ReflectRefractScale : register(c5);
float4 g_WaterFogColor : register(c6);
// x: fog start, y: fog end minus start.
float4 g_WaterFogParams : register(c7);
// w: the time in seconds.
float4 g_EyePos : register(c9);
// x: flow map coordinate scale, y: normal map coordinate scale, z: bump strength.
float4 g_FlowParams1 : register(c13);
// x: the period in seconds, y: the scroll distance per period, z: noise coordinate scale.
float4 g_FlowParams2 : register(c14);
float4 cLightScale : register(c30);

struct Input
{
    float2 flowCoord : TEXCOORD0;
    float3 tangentEye : TEXCOORD1;
    float4 reflectRefract : TEXCOORD2;
    float4 projection : TEXCOORD3;
    float2 world : TEXCOORD4;
#if LIGHTMAP_FOG
    centroid float2 lightmapCoord : TEXCOORD7;
#endif
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
// How far each period moves the normal map's coordinates, and the second sample's extra offset.
static const float kPeriodOffset = 0.311;

Output main(Input input)
{
    float2 world = input.world * float2(1.0, -1.0);
    float phase = g_EyePos.w / (2.0 * g_FlowParams2.x) + tex2D(FlowNoiseSampler, world * g_FlowParams2.z).y;
    float2 flow = tex2D(FlowmapSampler, input.flowCoord * g_FlowParams1.x).xy * 2.0 - 1.0;
    float2 scroll = flow * g_FlowParams2.y;
    float2 normalCoord = world * g_FlowParams1.y;
    float2 first = tex2D(NormalSampler, normalCoord + floor(phase) * kPeriodOffset + frac(phase) * scroll).xy;
    float shifted = phase + 0.5;
    float2 second = tex2D(NormalSampler, normalCoord + floor(shifted) * kPeriodOffset + 0.5 + frac(shifted) * scroll).xy;
    float2 slope = (lerp(first, second, abs(2.0 * frac(phase) - 1.0)) * 2.0 - 1.0) * (g_FlowParams1.z * (dot(flow, flow) + 0.1));

    float2 screenCoord = input.reflectRefract.wz / input.projection.w;
#if REFRACT_ALPHA_FOG
    float2 distortion = g_ReflectRefractScale.wz * tex2D(RefractSampler, screenCoord).a;
#else
    float2 distortion = g_ReflectRefractScale.wz;
#endif
    float4 refraction = tex2D(RefractSampler, screenCoord + slope * distortion);
#if REFRACT_ALPHA_FOG
    float fog = saturate(refraction.a - 0.05);
    float shore = saturate((refraction.a - 0.05) * 20.0);
#else
    float fog = saturate((input.projection.z - g_WaterFogParams.x) / g_WaterFogParams.y);
    float shore = 1.0;
#endif
#if LIGHTMAP_FOG
    float3 fogColor = tex2D(LightmapSampler, input.lightmapCoord).rgb * cLightScale.y * cLightScale.x * g_WaterFogColor.rgb;
#else
    float3 fogColor = g_WaterFogColor.rgb;
#endif
#if OPAQUE_FOG
    refraction.rgb = fogColor;
#else
    refraction.rgb = lerp(refraction.rgb, fogColor * cLightScale.x, fog);
#endif

    Output output;
    output.refraction = float4(refraction.rgb, 1.0);
    float eyeAngle = dot(input.tangentEye.xy, input.tangentEye.xy) > 1e-6 ? atan2(input.tangentEye.y, input.tangentEye.x) : kNoEyeAngle;
    output.normal = float4(slope, dot(g_ReflectTint.rgb, 1.0 / 3.0), eyeAngle);
    output.emissive = 0.0;
    output.flags = float4(0.0, 1.0, 0.0, shore);
    return output;
}
