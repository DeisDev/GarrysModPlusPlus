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
// Textured flowing water (FLOW_COLOR, no refraction or lightmap) flows its base texture the same way with its own
// period, parallax-shifted by the normal map's alpha, and fades from the fog colour to it by its alpha times the flow
// map's; the reflection's share fades out where that alpha is high (combo 74 of GMod's water_ps20b). With
// $lightmapwaterfog (LIGHTMAP_FOG) the lightmap lights both colours (combo 842).

sampler RefractSampler : register(s0);
sampler BaseTextureSampler : register(s1);
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
// x: flow map coordinate scale, y: normal map coordinate scale, z: bump strength, w: parallax scale.
float4 g_FlowParams1 : register(c13);
// x: the period in seconds, y: the scroll distance per period, z: noise coordinate scale.
float4 g_FlowParams2 : register(c14);
// The base texture's flow: x coordinate scale, y period in seconds, z scroll distance per period, w the exponent of
// the blend between its two samples.
float4 g_ColorFlowParams : register(c26);
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
    float noise = tex2D(FlowNoiseSampler, world * g_FlowParams2.z).y;
    float phase = g_EyePos.w / (2.0 * g_FlowParams2.x) + noise;
    float4 flowSample = tex2D(FlowmapSampler, input.flowCoord * g_FlowParams1.x);
    float2 flow = flowSample.xy * 2.0 - 1.0;
    float2 scroll = flow * g_FlowParams2.y;
    float2 normalCoord = world * g_FlowParams1.y;
    float4 first = tex2D(NormalSampler, normalCoord + floor(phase) * kPeriodOffset + frac(phase) * scroll);
    float shifted = phase + 0.5;
    float4 second = tex2D(NormalSampler, normalCoord + floor(shifted) * kPeriodOffset + 0.5 + frac(shifted) * scroll);
    float4 bump = lerp(first, second, abs(2.0 * frac(phase) - 1.0));
    float2 slope = (bump.xy * 2.0 - 1.0) * (g_FlowParams1.z * (dot(flow, flow) + 0.1));
    float eyeAngle = dot(input.tangentEye.xy, input.tangentEye.xy) > 1e-6 ? atan2(input.tangentEye.y, input.tangentEye.x) : kNoEyeAngle;
    Output output;
    output.normal = float4(slope, dot(g_ReflectTint.rgb, 1.0 / 3.0), eyeAngle);
    output.emissive = 0.0;
#if FLOW_COLOR
    float colorPhase = g_EyePos.w / (2.0 * g_ColorFlowParams.y) + noise;
    float2 colorScroll = flow * g_ColorFlowParams.z;
    float2 colorCoord = world * g_ColorFlowParams.x + bump.w * g_FlowParams1.w * (normalize(input.tangentEye).xy - slope);
    float colorShifted = colorPhase + 0.5;
    float4 colorFirst = tex2D(BaseTextureSampler, colorCoord + floor(colorPhase) * kPeriodOffset + (frac(colorPhase) - 0.5) * colorScroll);
    float4 colorSecond = tex2D(BaseTextureSampler, colorCoord + floor(colorShifted) * kPeriodOffset + 0.5 + (frac(colorShifted) - 0.5) * colorScroll);
    float4 color = colorFirst * pow(abs(2.0 * frac(colorShifted) - 1.0), g_ColorFlowParams.w) +
        colorSecond * pow(abs(2.0 * frac(colorPhase) - 1.0), g_ColorFlowParams.w);
    float opacity = color.a * flowSample.a;
#if LIGHTMAP_FOG
    float3 light = tex2D(LightmapSampler, input.lightmapCoord).rgb * cLightScale.y * cLightScale.x;
#else
    float3 light = 1.0;
#endif
    output.refraction = float4(lerp(g_WaterFogColor.rgb * light, color.rgb * flowSample.a * light, saturate(2.0 * opacity)), 1.0);
    output.flags = float4(0.0, 1.0, 0.0, 1.0 - smoothstep(0.5, 0.7, opacity));
    return output;
#else

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

    output.refraction = float4(refraction.rgb, 1.0);
    output.flags = float4(0.0, 1.0, 0.0, shore);
    return output;
#endif
}
