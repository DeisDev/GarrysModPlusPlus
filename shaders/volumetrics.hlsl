#include "scene.hlsli"

// Volumetric light at half resolution: sunlight and projected lights (flashlights, lamps) scattered by the air between
// the eye and each surface. On maps without fog the air is a thin, even haze; with the map's fog (g_frame.volumetricFog)
// it is that fog, lit (MarchFog).
// March (step 0): g_frame.volumetricSteps samples along each eye ray, offset per pixel and frame, each with a shadow ray to the
// sun and to every projected light; a Henyey-Greenstein phase function favours light seen against its direction.
// input0-2: the main view's indirect light volume (shaders/probes.hlsl) or kNoView; input3: 1 to scatter the map's and
// the engine's lights in the fog; output0: in-scattered radiance before the tonemap scale, and the length marched.
// Accumulate (step 1): blends the march, averaged with its neighbours of similar depth, into last frame's
// result, found with the motion vectors and clipped to the current neighbourhood. input0: march (rgb, march
// length); input1: last frame's accumulation; output0: this frame's (rgb, sample count); input2: 1 when last
// frame's accumulation may be used.

// Far surfaces scatter over this much haze at most.
static const float kMaxDistance = 4096.0;
// Sky pixels scatter over less: the sky texture already shows the haze outside.
static const float kSkyDistance = 1024.0;
// Strongly forward scattering: the haze shows as shafts and beams seen against the light, not as a wash over distant
// surfaces (g = 0.6 hazed whole maps over, 2026-10-31).
static const float kPhaseG = 0.8;
static const float kMaxSamples = 32.0;

float Phase(float cosine)
{
    float g2 = kPhaseG * kPhaseG;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(1.0 + g2 - 2.0 * kPhaseG * cosine, 1e-4), 1.5));
}

// The full-resolution pixel a half-resolution pixel stands for this frame.
int2 FullPixel(int2 pixel)
{
    uint frame = g_frame.frameIndex & 3u;
    return min(pixel * 2 + int2(frame & 1u, frame >> 1), int2(g_frame.size) - 1);
}

// Projected lights' irradiance at a point times the phase function towards the eye, with their shadows.
float3 ProjectorScatter(float3 position, float3 direction)
{
    float3 scattered = 0.0;
    for (uint p = 0; p < g_frame.projectorCount; ++p) {
        float3 toLight;
        float distance;
        Projector projector = LoadProjector(p);
        float3 irradiance = ProjectorIrradiance(projector, position, 0.0, toLight, distance);
        if (Luminance(irradiance) > 0.0)
            scattered += irradiance * Phase(dot(direction, toLight)) * ProjectorTransmittance(projector, position);
    }
    return scattered;
}

// Map and dynamic lights' irradiance at a point times the phase function towards the eye: one light picked by
// SampleLight, with its shadow ray. Projected lights are ProjectorScatter's.
float3 LampScatter(float3 position, float3 direction, inout uint seed)
{
    LightSet set = LightSetAt(position);
    set.count = set.pointLights;
    float3 irradiance;
    float3 toLight;
    float distance;
    uint picked;
    if (!SampleLight(set, position, 0.0, seed, irradiance, toLight, distance, picked))
        return 0.0;
    return irradiance * Phase(dot(direction, toLight)) *
        LightTransmittance(set, picked, position, position + toLight * distance, float2(Random(seed), Random(seed)));
}

// The map's fog as lit air. Source blends its fog colour over a surface by FogOpacity at the surface's view depth; here
// each step adds the opacity it gains, lit by LitFog, so open sunlit air shows the fog as the game draws it, and
// ApplyFog (shaders/deferred_lighting.hlsl) only hides the surface behind. Lamps (input3: 1 for the map's and the
// engine's lights) and projected lights scatter in it as in the haze.
float4 MarchFog(float3 direction, float distance, float depthPerUnit, uint seed)
{
    if (g_frame.fogParams.w <= 0.0 || depthPerUnit <= 0.0)
        return 0.0;
    // Source's fog starts at view depth x / w and stops thickening at (min(z, 1) + x) / w.
    float start = max(g_frame.fogParams.x / (g_frame.fogParams.w * depthPerUnit), 0.0);
    float end = min(distance, (min(g_frame.fogParams.z, 1.0) + g_frame.fogParams.x) / (g_frame.fogParams.w * depthPerUnit));
    if (end <= start)
        return float4(0.0, 0.0, 0.0, max(end, 0.0));
    uint3 volume = uint3(g_pass.input0, g_pass.input1, g_pass.input2);
    float step = (end - start) / g_frame.volumetricSteps;
    float opacity = FogOpacity(start * depthPerUnit);
    float3 light = 0.0;
    for (uint s = 0; s < g_frame.volumetricSteps; ++s) {
        float next = FogOpacity((start + (s + 1) * step) * depthPerUnit);
        float gained = next - opacity;
        opacity = next;
        float3 position = g_frame.eyePosition + direction * (start + (s + Random(seed)) * step);
        float3 scattered = ProjectorScatter(position, direction);
        if (g_pass.input3 != 0)
            scattered += LampScatter(position, direction, seed);
        // Radiance scattered from irradiance E is opacity * phase * pi * E, as in the haze.
        light += gained * (LitFog(position, volume) + 3.14159265 * scattered);
    }
    return float4(ClampSample(light), end);
}

// The water the eye is in (g_frame.eyeWater) as far as the surface: its own light (ThroughWater: sunlight bent and
// focused by the surface, shafts where the water's shadows break it, the sky's light by depth), and the light lamps and
// projected lights scatter in it (WaterLampsAlong). Their shadow rays follow the light through the water (LightPath),
// which costs too much at every step: lamps are sampled at a quarter as many points, and the history averages the noise.
float4 MarchWater(float3 direction, float rayLength, uint seed)
{
    uint water = g_frame.eyeWater;
    float3 light = ThroughWater(0.0, g_frame.eyePosition, direction, rayLength, water, g_frame.volumetricSteps, seed) +
        WaterLampsAlong(g_frame.eyePosition, direction, rayLength, water, max(g_frame.volumetricSteps / 4, 1u), seed);
    return float4(ClampSample(light), rayLength);
}

float4 MarchHaze(float3 direction, float rayLength, uint seed)
{
    float step = rayLength / g_frame.volumetricSteps;
    float density = g_frame.volumetricDensity;
    float sunPhase = Phase(dot(direction, g_frame.sunDirection));
    float3 light = 0.0;
    // Under water (fogParams.y, shaders/deferred_lighting.hlsl) the scattered light crosses the water to the eye and takes
    // its colour; without that the air's haze washed the water's fog out to grey.
    bool underwater = g_frame.fogEnabled != 0 && g_frame.fogParams.y > 0.0;
    float3 waterTint = g_frame.fogColor / max(max(g_frame.fogColor.r, max(g_frame.fogColor.g, g_frame.fogColor.b)), 1e-4);
    for (uint s = 0; s < g_frame.volumetricSteps; ++s) {
        float t = (s + Random(seed)) * step;
        float3 position = g_frame.eyePosition + direction * t;
        float3 scattered = ProjectorScatter(position, direction);
        if (g_frame.sunEnabled != 0.0)
            scattered += g_frame.sunColor * sunPhase * SunVisibility(position, g_frame.sunShadowMask);
        // Radiance scattered from irradiance E is density * phase * pi * E in the units the lighting uses.
        if (underwater)
            scattered *= waterTint * (1.0 - saturate(min(g_frame.fogParams.z, t * g_frame.fogParams.w - g_frame.fogParams.x)));
        light += scattered * (3.14159265 * density * step * exp(-density * t));
    }
    return float4(ClampSample(light), rayLength);
}

float4 March(int2 pixel)
{
    int2 full = FullPixel(pixel);
    float4 surface = SurfaceAt(g_frame.surfaceView, full);
    float3 target = WorldPositionFromDepth(full, DepthAt(full));
    float3 toTarget = target - g_frame.eyePosition;
    float targetDistance = max(sqrt(dot(toTarget, toTarget)), 1e-3);
    float3 direction = toTarget / targetDistance;
    uint seed = PixelSeed(full, 0xB1u);
    if (g_frame.eyeWater != kNoView)
        return MarchWater(direction, surface.w != 0.0 ? min(abs(surface.w), kMaxDistance) : kMaxDistance, seed);
    if (g_frame.volumetricFog != 0) {
        // Source leaves the sky unfogged.
        if (surface.w == 0.0)
            return 0.0;
        return MarchFog(direction, abs(surface.w), mul(g_frame.viewProj, float4(target, 1.0)).w / targetDistance, seed);
    }
    // Water pixels (negative distance, shaders/surface.hlsl) scatter as far as the surface.
    return MarchHaze(direction, surface.w != 0.0 ? min(abs(surface.w), kMaxDistance) : kSkyDistance, seed);
}

float3 RgbToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 YCoCgToRgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

float4 Accumulate(int2 pixel, uint2 size)
{
    Texture2D<float4> march = ResourceDescriptorHeap[g_pass.input0];
    float centreLength = march.Load(int3(pixel, 0)).a;
    float3 current = 0.0;
    float weights = 0.0;
    float3 moment1 = 0.0;
    float3 moment2 = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float4 sample = march.Load(int3(clamp(pixel + int2(x, y), 0, int2(size) - 1), 0));
            // Rays of very different length cross different air, as at silhouettes.
            float weight = saturate(1.0 - abs(sample.a - centreLength) / (0.1 * centreLength + 16.0));
            current += sample.rgb * weight;
            weights += weight;
            float3 c = RgbToYCoCg(sample.rgb);
            moment1 += c;
            moment2 += c * c;
        }
    }
    current /= weights;
    if (g_pass.input2 == 0)
        return float4(current, 1.0);
    float3 mean = moment1 / 9.0;
    float3 deviation = sqrt(max(moment2 / 9.0 - mean * mean, 0.0));
    Texture2D<float2> motion = ResourceDescriptorHeap[g_frame.motionView];
    Texture2D<float4> history = ResourceDescriptorHeap[g_pass.input1];
    float2 position = float2(pixel * 2) + 1.0 + motion.Load(int3(FullPixel(pixel), 0));
    if (any(position < 0.0) || any(position > float2(g_frame.size)))
        return float4(current, 1.0);
    // The grid fills only part of the texture when Ray Reconstruction renders below the output size.
    float2 historySize;
    history.GetDimensions(historySize.x, historySize.y);
    float4 previous = history.SampleLevel(g_linearClamp, min(position * 0.5, float2(size) - 0.5) / historySize, 0.0);
    float3 clipped = YCoCgToRgb(clamp(RgbToYCoCg(previous.rgb), mean - 2.0 * deviation, mean + 2.0 * deviation));
    float samples = min(previous.a + 1.0, kMaxSamples);
    return float4(lerp(clipped, current, 1.0 / samples), samples);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 size = (g_frame.size + 1) / 2;
    if (any(id.xy >= size))
        return;
    int2 pixel = int2(id.xy);
    g_inAir = true;
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    output[pixel] = g_pass.step == 0 ? March(pixel) : Accumulate(pixel, size);
}
