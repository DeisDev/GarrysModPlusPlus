// Real-time light for translucent surfaces drawn by forward twins (shaders/twins with FORWARD_TWIN): the translator
// includes this in their translated pixel shaders (Variant::forwardLighting), which the game's command lists draw with
// the forward frame constants, the ray-traced scene and the indirect-only probe volume in space1. The sun, the
// strongest local lights and projected lights are traced per pixel; the volume adds the indirect light.

#include "scene.hlsli"

static const uint kForwardLights = 4;

float3 ForwardSunLight(float3 position, float3 normal)
{
    float lambert = Lambert(normal, g_frame.sunDirection);
    if (g_frame.sunEnabled == 0.0 || lambert <= 0.0)
        return 0.0;
    return g_frame.sunColor * lambert * SunVisibility(position + g_frame.sunDirection * 0.5, g_frame.sunShadowMask);
}

float3 ForwardLocalLight(float3 position, float3 normal)
{
    LightSet set = LightSetAt(position, normal);
    uint top[kForwardLights];
    float topWeight[kForwardLights];
    [unroll]
    for (uint k = 0; k < kForwardLights; ++k) {
        top[k] = 0;
        topWeight[k] = 0.0;
    }
    float3 toLight;
    float distance;
    for (uint i = 0; i < set.pointLights; ++i) {
        uint candidate = i;
        float candidateWeight = Luminance(SetLightIrradiance(set, i, position, normal, toLight, distance));
        [unroll]
        for (uint k = 0; k < kForwardLights; ++k) {
            if (candidateWeight > topWeight[k]) {
                uint index = top[k];
                float w = topWeight[k];
                top[k] = candidate;
                topWeight[k] = candidateWeight;
                candidate = index;
                candidateWeight = w;
            }
        }
    }
    float3 light = 0.0;
    [unroll]
    for (uint k = 0; k < kForwardLights; ++k) {
        if (topWeight[k] <= VisibleIrradiance())
            break;
        float3 irradiance = SetLightIrradiance(set, top[k], position, normal, toLight, distance);
        light += irradiance * LightTransmittance(set, top[k], position + toLight * 0.5, toLight, max(distance - 0.5, 0.0));
    }
    for (uint p = set.pointLights; p < set.count; ++p) {
        float3 irradiance = SetLightIrradiance(set, p, position, normal, toLight, distance);
        if (Luminance(irradiance) > VisibleIrradiance())
            light += irradiance * LightTransmittance(set, p, position + toLight * 0.5, toLight, max(distance - 0.5, 0.0));
    }
    return light;
}

// oC0 albedo (alpha: opacity), oC1 normal, oC2 light the material shows unlit, oC3 world position: the colour the
// game's lit shaders would write, with the tonemap scale and range fog as on opaque surfaces
// (shaders/deferred_lighting.hlsl).
float4 gmodpp_ForwardTwin(float4 albedo, float4 normal, float4 emissive, float4 position)
{
    float3 n = dot(normal.xyz, normal.xyz) > 0.0625 ? normalize(normal.xyz) : float3(0.0, 0.0, 1.0);
    float3 indirect = VolumeIrradiance(position.xyz, n);
    if (indirect.x < 0.0)
        indirect = g_frame.skyColor;
    float3 light = ForwardSunLight(position.xyz, n) + ForwardLocalLight(position.xyz, n) + indirect * g_frame.indirectScale;
    // Debug views show the light alone, opaque.
    bool lightOnly = g_frame.debugView != 0;
    float3 color = ((lightOnly ? 1.0 : albedo.rgb) * light + emissive.rgb) * g_frame.tonemapScale;
    if (g_frame.fogEnabled != 0) {
        float viewDepth = mul(g_frame.viewProj, float4(position.xyz, 1.0)).w;
        float fog = saturate(min(g_frame.fogParams.z, viewDepth * g_frame.fogParams.w - g_frame.fogParams.x));
        color = lerp(color, g_frame.fogColor, g_frame.fogParams.y > 0.0 ? fog : fog * fog);
    }
    return float4(color, lightOnly ? 1.0 : albedo.a);
}
