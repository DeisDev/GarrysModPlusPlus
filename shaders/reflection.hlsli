#ifndef GMODPP_REFLECTION_HLSLI
#define GMODPP_REFLECTION_HLSLI

#include "scene.hlsli"

// Radiance along reflected rays, shared by the reflections pass and water (shaders/water.hlsli).

static const float kMaxDistance = 16384.0;
// Ray Reconstruction's hit distance for reflections of the sky.
static const float kSkyHitDistance = 65504.0;

// Last frame's colour (`previousView`, the main view's shaded colour) at a screen position, before the tonemap scale;
// the surface there must be `distance` from last frame's eye (0 for the sky). Debug views wrote their debug image
// instead, so they trace every hit.
float3 PreviousColor(uint previousView, float4 clip, float distance)
{
    if (g_frame.historyValid == 0 || g_frame.debugView != 0 || clip.w <= 0.0)
        return -1.0;
    float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    if (any(uv < 0.0) || any(uv >= 1.0))
        return -1.0;
    int2 pixel = int2(uv * float2(g_frame.size));
    float4 surface = SurfaceAt(g_frame.prevSurfaceView, pixel);
    // Water marks its pixels with a negative distance; only the sky has none.
    bool matches = distance > 0.0 ? surface.w > 0.0 && abs(surface.w - distance) <= 4.0 + distance * 0.02 : surface.w == 0.0;
    if (!matches)
        return -1.0;
    if (previousView == kNoView)
        return -1.0;
    Texture2D<float4> previous = ResourceDescriptorHeap[previousView];
    float3 color = previous.Load(int3(pixel, 0)).rgb;
    return (g_frame.outputLinear != 0 ? color : Decode(color)) / g_frame.tonemapScale;
}

float3 ReflectedRadiance(uint previousView, float3 origin, float3 direction, float3 ambient, inout uint seed, out float hitDistance)
{
    HitSurface hit;
    HitInfo hitInfo;
    if (!TraceClosest(origin, direction, kMaxDistance, g_frame.traceMask, hit, hitInfo)) {
        hitDistance = kSkyHitDistance;
        // A direction at infinity: w = 0 projects it to where last frame showed that part of the sky.
        float3 sky = PreviousColor(previousView, mul(g_frame.prevViewProj, float4(direction, 0.0)), 0.0);
        return sky.x >= 0.0 ? sky : g_frame.skyColor;
    }
    hitDistance = length(hit.position - origin);
    // Positions in the 3D skybox are in its own scaled-down space, which last frame's main view does not show.
    if ((g_frame.traceMask & kSkyboxMask) == 0) {
        float3 previous = PreviousColor(previousView, mul(g_frame.prevViewProj, float4(hit.position, 1.0)), length(hit.position - g_frame.prevEyePosition));
        if (previous.x >= 0.0)
            return previous;
    }
    float3 shadowOrigin = hit.position + hit.normal;
    float3 irradiance = SunIrradiance(hit, shadowOrigin, g_frame.sunShadowMask) + ambient;
    float3 light;
    float3 toLight;
    float distance;
    uint picked;
    LightSet set = LightSetAt(hit.position, hit.normal);
    if (SampleLight(set, hit.position, hit.normal, seed, light, toLight, distance, picked))
        irradiance += light * LightTransmittance(set, picked, shadowOrigin, toLight, distance);
    return ClampSample(hit.albedo * irradiance + HitEmission(hit, hitInfo, hitDistance));
}

#endif
