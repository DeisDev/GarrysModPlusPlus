#ifndef POST_FLARE_HLSLI
#define POST_FLARE_HLSLI

#include "post.hlsli"

// A lens flare from the sun or the Moon, added to the view: ghosts (images of the six-bladed aperture reflected between
// the lens elements, along the line from the light through the centre), a halo ring around the centre on the light's
// side, and a starburst (the aperture's diffraction spikes and glare) around the light.

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
    // The starburst: six diffraction spikes (a six-bladed aperture), six fainter ones between, and the glare's glow.
    float2 toLight = pixel - g_post.flarePosition;
    float r = length(toLight);
    float angle = atan2(toLight.y, toLight.x) + kApertureRotation;
    float spikes = pow(abs(cos(3.0 * angle)), 400.0) + 0.3 * pow(abs(sin(3.0 * angle)), 600.0);
    float glow = 0.25 * exp(-r / (0.02 * height)) + exp(-r / (0.004 * height));
    flare += g_post.flareStarburst * (spikes * exp(-r / (0.12 * height)) + glow);
    return flare * light;
}

#endif
