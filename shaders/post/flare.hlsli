#ifndef POST_FLARE_HLSLI
#define POST_FLARE_HLSLI

#include "post.hlsli"

// A lens flare from the sun or the Moon, added to the view: ghosts (images of the six-bladed aperture reflected between
// the lens elements, along the line from the light through the centre), a halo ring around the centre on the light's
// side, and a starburst (Starburst) around the light.

static const uint kGhostCount = 6;
// Per ghost: its place on the line (0 the light, 1 the centre, 2 the light mirrored), radius (share of the height) and
// brightness; and its coating's tint.
static const float3 kGhosts[kGhostCount] = {
    float3(0.45, 0.022, 0.12), float3(0.78, 0.055, 0.05), float3(1.22, 0.030, 0.09),
    float3(1.48, 0.090, 0.035), float3(1.75, 0.040, 0.07), float3(2.10, 0.120, 0.025)};
static const float3 kGhostTints[kGhostCount] = {
    float3(1.0, 0.75, 0.40), float3(0.40, 0.85, 1.0), float3(0.80, 0.50, 1.0),
    float3(0.55, 1.0, 0.60), float3(1.0, 0.55, 0.70), float3(0.45, 0.60, 1.0)};
static const float kApertureRotation = 0.26;

// A regular hexagon's distance measure: 1 on its edge for an inner radius of 1.
float Hexagon(float2 p)
{
    float c = cos(kApertureRotation);
    float s = sin(kApertureRotation);
    p = abs(float2(c * p.x - s * p.y, s * p.x + c * p.y));
    return max(dot(p, float2(0.8660254, 0.5)), p.y);
}

// The light reaching the lens per colour (CameraMain's visibility times its brightness), faded out as it leaves the frame
// by a quarter of the height.
float3 FlareLight()
{
    if (g_post.flareInFront == 0.0 || g_post.flareVisibilityView == kNoView)
        return 0.0;
    RWStructuredBuffer<float4> camera = ResourceDescriptorHeap[g_post.cameraView];
    float3 visibility = float3(camera[0].zw, camera[1].x);
    float2 outside = max(max(-g_post.flarePosition, g_post.flarePosition - float2(g_post.size)), 0.0);
    float fade = saturate(1.0 - length(outside) / (0.25 * float(g_post.size.y)));
    return g_post.flareColor * visibility * fade;
}

float FlareHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x) * (1.0 / 4294967296.0);
}

static const uint kCoronaStreaks = 120;

// The glare around the light, `toLight` pixels from it: the six-bladed aperture's diffraction spikes, a thin line each
// that fades with distance and spreads each colour in proportion to its wavelength (red reaches furthest); fine streaks
// of uneven length all around (the ciliary corona, Ritschel et al., "Temporal Glare", 2009); and the glow.
float3 Starburst(float2 toLight, float height)
{
    float r = length(toLight);
    float angle = atan2(toLight.y, toLight.x) - kApertureRotation;
    const float sector = 1.0471976;
    float across = r * abs(sin(angle - sector * round(angle / sector)));
    float width = 1.0 + 0.004 * r;
    float3 reach = 0.05 * height * float3(1.2, 1.0, 0.8);
    float3 spikes = 0.35 * exp(-across * across / (width * width)) * exp(-r / reach);
    float turns = (angle / 6.2831853 + 0.5) * float(kCoronaStreaks);
    uint streak = uint(floor(turns)) % kCoronaStreaks;
    float centre = (float(streak) + 0.2 + 0.6 * FlareHash(streak * 3u + 1u)) / float(kCoronaStreaks) * 6.2831853 - 3.1415927;
    float streakAcross = r * abs(sin(angle - centre));
    float strength = pow(FlareHash(streak * 3u), 6.0);
    float streakReach = (0.01 + 0.03 * FlareHash(streak * 3u + 2u)) * height;
    float corona = 0.25 * strength * exp(-streakAcross * streakAcross) * exp(-r / streakReach);
    float glow = exp(-r / (0.004 * height)) + 0.12 * exp(-r / (0.025 * height)) + 0.02 * exp(-r / (0.12 * height));
    return spikes + corona + glow;
}

float3 LensFlare(float2 pixel, float3 light)
{
    float height = float(g_post.size.y);
    float2 centre = 0.5 * float2(g_post.size);
    float2 axis = centre - g_post.flarePosition;
    float3 flare = 0.0;
    [unroll]
    for (uint i = 0; i < kGhostCount; ++i) {
        float2 position = g_post.flarePosition + axis * kGhosts[i].x;
        float shape = Hexagon((pixel - position) / (kGhosts[i].y * height));
        float body = smoothstep(1.0, 0.85, shape) * (0.55 + 0.45 * smoothstep(0.4, 1.0, shape));
        flare += kGhostTints[i] * (kGhosts[i].z * body);
    }
    // The halo: a thin ring 0.38 of the height around the centre, brightest toward the light and the further the light is
    // from the centre, each colour at a slightly different radius.
    float2 fromCentre = pixel - centre;
    float distance = length(fromCentre);
    float lightDistance = length(axis);
    float facing = lightDistance > 0.0 ? saturate(dot(fromCentre, -axis) / max(distance * lightDistance, 1e-3)) : 0.0;
    float3 radius = 0.38 * height * float3(0.985, 1.0, 1.015);
    float3 ringOffset = (distance - radius) / (0.012 * height);
    float3 ring = exp(-ringOffset * ringOffset);
    flare += ring * (0.06 * facing * facing * saturate(lightDistance / (0.5 * height)));
    flare += g_post.flareStarburst * Starburst(pixel - g_post.flarePosition, height);
    return flare * light;
}

#endif
