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
// w: 1 in debug views, which show the light alone on opaque surfaces.
float4 g_ForwardFallback : FORWARD_CONSTANT(c218, c1);
// w: with the map's fog lit (shaders/volumetrics.hlsl), 1 / the volume's average irradiance in open sunlit air; 0 keeps
// the fog's colour.
float4 g_ForwardFogColor : FORWARD_CONSTANT(c219, c2);
// xyz: texture coordinates per world unit (1 / (spacing * grid size)); the textures wrap. w: the probe spacing.
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
    // The probes are read a cell (g_ProbeScale.w) out along the normal, so all eight blended lie in front of the
    // surface: probes just behind a wall see open space there and leaked its light through the wall. Beyond the
    // window, its nearest probes: the sky's light lit far walls of dark rooms in mirrors.
    position += normal * g_ProbeScale.w;
    float3 halfWindow = 1.0 / g_ForwardDraw.yzw;
    position = g_ProbeWindow.xyz + clamp(position - g_ProbeWindow.xyz, -halfWindow, halfWindow);
    float3 uvw = position * g_ProbeScale.xyz;
    float4 r = SampleProbe(red, uvw);
    float4 g = SampleProbe(green, uvw);
    float4 b = SampleProbe(blue, uvw);
    return max(float3(r.x + dot(r.yzw, normal), g.x + dot(g.yzw, normal), b.x + dot(b.yzw, normal)), 0.0);
}

// The irradiance averaged over all directions at a point: the volume's constant terms.
float3 ProbeAverage(float3 position, sampler3D red, sampler3D green, sampler3D blue)
{
    float3 halfWindow = 1.0 / g_ForwardDraw.yzw;
    float3 uvw = (g_ProbeWindow.xyz + clamp(position - g_ProbeWindow.xyz, -halfWindow, halfWindow)) * g_ProbeScale.xyz;
    return max(float3(SampleProbe(red, uvw).x, SampleProbe(green, uvw).x, SampleProbe(blue, uvw).x), 0.0);
}

// The colour the game's lit shaders would write: albedo times light, times the tonemap scale, with range fog.
// `emissive` is added unlit.
float4 ForwardLight(float4 albedo, float3 normal, float3 position, float3 emissive, sampler3D red, sampler3D green, sampler3D blue)
{
    float3 n = dot(normal, normal) > 0.0625 ? normalize(normal) : float3(0.0, 0.0, 1.0);
    float3 light = lerp(ProbeIrradiance(position, n, red, green, blue), 1.0, g_ForwardDraw.x);
    float3 color = (lerp(albedo.rgb, 1.0, g_ForwardFallback.w) * light + emissive) * g_ForwardEye.w;
    float fog = saturate(min(g_ForwardFog.z, length(position - g_ForwardEye.xyz) * g_ForwardFog.w - g_ForwardFog.x));
    // Lit fog keeps its colour in open sunlit air and darkens where less light reaches (shaders/volumetrics.hlsl).
    float3 fogColor = g_ForwardFogColor.rgb;
    if (g_ForwardFogColor.w > 0.0)
        fogColor *= saturate(dot(ProbeAverage(position, red, green, blue), float3(0.2126, 0.7152, 0.0722)) * g_ForwardFogColor.w);
    // Debug views draw translucent surfaces opaque so their light shows.
    return float4(lerp(color, fogColor, g_ForwardFog.y > 0.0 ? fog : fog * fog), lerp(albedo.a, 1.0, g_ForwardFallback.w));
}
