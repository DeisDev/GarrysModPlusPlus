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

// Points of the cell's tail distribution its lights are taken at.
static const uint kForwardTailSamples = 4;

float3 ForwardLocalLight(float3 position, float3 normal)
{
    LightSet set = LightSetAt(position);
    uint top[kForwardLights];
    float topWeight[kForwardLights];
    [unroll]
    for (uint k = 0; k < kForwardLights; ++k) {
        top[k] = 0;
        topWeight[k] = 0.0;
    }
    float3 toLight;
    float distance;
    // The lights left out of the strongest, and the cell's tail at fixed points of its distribution, share the shadow
    // ray of the strongest of them: with only the strongest, surfaces lit by many lamps, such as a room's far end in a
    // mirror, came out darker than the room, and a light picked at random per pixel sparkled on glass, which is drawn
    // after anti-aliasing and never denoised.
    float3 remainder = 0.0;
    uint strongest = 0;
    float strongestWeight = 0.0;
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
        if (candidateWeight > 0.0) {
            remainder += SetLightIrradiance(set, candidate, position, normal, toLight, distance);
            if (candidateWeight > strongestWeight) {
                strongest = candidate;
                strongestWeight = candidateWeight;
            }
        }
    }
    for (uint t = 0; t < kForwardTailSamples; ++t) {
        uint tailLight;
        float probability;
        if (!SampleTailLight(set.ranges, (t + 0.5) / kForwardTailSamples, tailLight, probability))
            break;
        float3 irradiance = SetLightIrradiance(set, set.count + tailLight, position, normal, toLight, distance);
        remainder += irradiance / (probability * kForwardTailSamples);
        if (Luminance(irradiance) > strongestWeight) {
            strongest = set.count + tailLight;
            strongestWeight = Luminance(irradiance);
        }
    }
    float3 light = 0.0;
    [unroll]
    for (uint k = 0; k < kForwardLights; ++k) {
        if (topWeight[k] <= 0.0)
            break;
        float3 irradiance = SetLightIrradiance(set, top[k], position, normal, toLight, distance);
        light += irradiance * LightTransmittance(set, top[k], position + toLight * 0.5, position + toLight * distance, kLightCentre);
    }
    if (strongestWeight > 0.0) {
        SetLightIrradiance(set, strongest, position, normal, toLight, distance);
        light += remainder * LightTransmittance(set, strongest, position + toLight * 0.5, position + toLight * distance, kLightCentre);
    }
    for (uint p = set.pointLights; p < set.count; ++p) {
        float3 irradiance = SetLightIrradiance(set, p, position, normal, toLight, distance);
        if (Luminance(irradiance) > VisibleIrradiance())
            light += irradiance * LightTransmittance(set, p, position + toLight * 0.5, position + toLight * distance, kLightCentre);
    }
    return light;
}

// oC0 albedo (alpha: opacity), oC1 normal, oC2 light the material shows unlit, oC3 world position: the colour the
// game's lit shaders would write, with the tonemap scale and range fog as on opaque surfaces
// (shaders/deferred_lighting.hlsl).
float4 gmodpp_ForwardTwin(float4 albedo, float4 normal, float4 emissive, float4 position)
{
    float3 n = dot(normal.xyz, normal.xyz) > 0.0625 ? normalize(normal.xyz) : float3(0.0, 0.0, 1.0);
    // Beyond the volume's window, its nearest probes: the sky's colour lit far walls of dark rooms in mirrors.
    float3 indirect = VolumeIrradiance(position.xyz, n, true);
    if (indirect.x < 0.0)
        indirect = g_frame.skyColor;
    float3 light = ForwardSunLight(position.xyz, n) + ForwardLocalLight(position.xyz, n) + indirect * g_frame.indirectScale;
    // Debug views show the light alone, opaque.
    bool lightOnly = g_frame.debugView != 0;
    float3 color = ((lightOnly ? 1.0 : albedo.rgb) * light + emissive.rgb) * g_frame.tonemapScale;
    if (g_frame.fogEnabled != 0) {
        float fog = FogOpacity(mul(g_frame.viewProj, float4(position.xyz, 1.0)).w);
        // Lit fog (shaders/volumetrics.hlsl) is lit where it meets the surface, a unit towards the eye.
        float3 fogColor = g_frame.fogColor;
        if (g_frame.volumetricFog != 0) {
            float3 air = position.xyz + normalize(g_frame.eyePosition - position.xyz);
            fogColor = LitFog(air, uint3(g_frame.probeRedView, g_frame.probeGreenView, g_frame.probeBlueView)) * g_frame.tonemapScale;
        }
        color = lerp(color, fogColor, fog);
    }
    return float4(color, lightOnly ? 1.0 : albedo.a);
}
