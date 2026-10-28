// Real-time light for surfaces the game draws after the G-buffer capture (translucent surfaces), Direct3D 9
// pixel shaders (ps_3_0, or ps_2_b with FORWARD_PS2). Irradiance comes from the ray-traced probe volume
// (shaders/probes.hlsl), copied into three volume textures that hold a + b.n per colour channel. The seven
// constants are gbuffer's forward constants: c217-c223 in ps_3_0, c0-c6 in ps_2_b, where the bytecode patch moves
// them to registers the game's shader leaves free.

#ifdef FORWARD_PS2
#define FORWARD_CONSTANT(ps3, ps2) register(ps2)
#else
#define FORWARD_CONSTANT(ps3, ps2) register(ps3)
#endif

// x: 1 for additive draws, which add light and are not lit again; yzw: 1 / half the probe window's size.
float4 g_ForwardDraw : FORWARD_CONSTANT(c217, c0);
// rgb: irradiance outside the volume; w: 1 in debug views, which show the light alone on opaque surfaces.
float4 g_ForwardFallback : FORWARD_CONSTANT(c218, c1);
float4 g_ForwardFogColor : FORWARD_CONSTANT(c219, c2);
// xyz: texture coordinates per world unit (1 / (spacing * grid size)); the textures wrap.
float4 g_ProbeScale : FORWARD_CONSTANT(c220, c3);
// xyz: centre of the probe window, which ends where its outermost probes sit.
float4 g_ProbeWindow : FORWARD_CONSTANT(c221, c4);
// xyz: eye position; w: the tonemap scale (cLightScale.x).
float4 g_ForwardEye : FORWARD_CONSTANT(c222, c5);
// Source's g_FogParams: x start over range, z maximum density, w 1 / range; zero without fog. y: 1 under water, whose
// fog is blended in linearly.
float4 g_ForwardFog : FORWARD_CONSTANT(c223, c6);

float4 SampleProbe(sampler3D probe, float3 uvw)
{
#ifdef FORWARD_PS2
    return tex3D(probe, uvw);
#else
    return tex3Dlod(probe, float4(uvw, 0.0));
#endif
}

float3 ProbeIrradiance(float3 position, float3 normal, sampler3D red, sampler3D green, sampler3D blue)
{
    float3 uvw = position * g_ProbeScale.xyz;
    float4 r = SampleProbe(red, uvw);
    float4 g = SampleProbe(green, uvw);
    float4 b = SampleProbe(blue, uvw);
    float3 irradiance = max(float3(r.x + dot(r.yzw, normal), g.x + dot(g.yzw, normal), b.x + dot(b.yzw, normal)), 0.0);
    float3 offset = abs(position - g_ProbeWindow.xyz) * g_ForwardDraw.yzw;
    float fade = saturate((max(offset.x, max(offset.y, offset.z)) - 0.8) * 5.0);
    return lerp(irradiance, g_ForwardFallback.rgb, fade);
}

// The colour the game's lit shaders would write: albedo times light, times the tonemap scale, with range fog.
// `emissive` is added unlit.
float4 ForwardLight(float4 albedo, float3 normal, float3 position, float3 emissive, sampler3D red, sampler3D green, sampler3D blue)
{
    float3 n = dot(normal, normal) > 0.0625 ? normalize(normal) : float3(0.0, 0.0, 1.0);
    float3 light = lerp(ProbeIrradiance(position, n, red, green, blue), 1.0, g_ForwardDraw.x);
    float3 color = (lerp(albedo.rgb, 1.0, g_ForwardFallback.w) * light + emissive) * g_ForwardEye.w;
    float fog = saturate(min(g_ForwardFog.z, length(position - g_ForwardEye.xyz) * g_ForwardFog.w - g_ForwardFog.x));
    // Debug views draw translucent surfaces opaque so their light shows.
    return float4(lerp(color, g_ForwardFogColor.rgb, g_ForwardFog.y > 0.0 ? fog : fog * fog), lerp(albedo.a, 1.0, g_ForwardFallback.w));
}
