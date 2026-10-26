#include "scene.hlsli"

// One cosine-weighted diffuse bounce per pixel. The result is an irradiance estimate: a sky miss returns
// the sky irradiance, a hit returns its albedo times the light reaching it (sun, one random local light and
// indirect light: last frame's result where the hit is on screen, otherwise one more traced bounce).
// input0: last frame's filtered indirect irradiance, or none; output0: raw estimate; step: samples per pixel.

static const float kMaxDistance = 16384.0;

float3 PreviousIndirect(float3 position, float3 normal)
{
    if (g_frame.historyValid == 0 || g_pass.input0 == kNoView)
        return -1.0;
    float4 clip = mul(g_frame.prevViewProj, float4(position, 1.0));
    if (clip.w <= 0.0)
        return -1.0;
    float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    if (any(uv < 0.0) || any(uv >= 1.0))
        return -1.0;
    int2 pixel = int2(uv * float2(g_frame.size));
    float4 surface = SurfaceAt(g_frame.prevSurfaceView, pixel);
    float distance = length(position - g_frame.prevEyePosition);
    if (surface.w <= 0.0 || abs(surface.w - distance) > 4.0 + distance * 0.02 || dot(surface.xyz, normal) < 0.7)
        return -1.0;
    Texture2D<float4> indirect = ResourceDescriptorHeap[g_pass.input0];
    return indirect.Load(int3(pixel, 0)).rgb;
}

// Indirect light at an off-screen hit: one more diffuse bounce that sees the sky or a sunlit surface.
float3 SecondBounce(HitSurface hit, float3 origin, inout uint seed)
{
    float3 direction = CosineSample(hit.normal, Random(seed), Random(seed));
    HitSurface next;
    if (!TraceClosest(origin, direction, kMaxDistance, g_frame.traceMask, next))
        return g_frame.skyColor;
    return next.albedo * SunIrradiance(next, next.position + next.normal);
}

float3 HitIrradiance(HitSurface hit, inout uint seed)
{
    float3 origin = hit.position + hit.normal * 1.0;
    float3 irradiance = SunIrradiance(hit, origin);
    // One light picked uniformly among the map lights that can reach the hit, dynamic and projected lights.
    LightRanges ranges = LightsAt(hit.position, hit.normal);
    uint mapLights = LightRangeCount(ranges);
    uint lightCount = mapLights + g_frame.dynamicLightCount;
    uint candidates = lightCount + g_frame.projectorCount;
    if (candidates > 0) {
        uint index = min(uint(Random(seed) * candidates), candidates - 1);
        bool attached = index >= mapLights;
        float3 toLight;
        float distance;
        float3 light = index >= lightCount ? ProjectorIrradiance(LoadProjector(index - lightCount), hit.position, hit.normal, toLight, distance)
            : LightIrradiance(LoadLight(attached ? g_frame.lightCount + index - mapLights : LightRangeIndex(ranges, index)), hit.position, hit.normal,
                  toLight, distance);
        if (any(light > 0.0) && !Occluded(origin, toLight, ShadowRayLength(attached, distance), ShadowMask(attached)))
            irradiance += light * candidates;
    }
    float3 previous = PreviousIndirect(hit.position, hit.normal);
    irradiance += previous.x >= 0.0 ? previous : SecondBounce(hit, origin, seed);
    return irradiance;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    if (surface.w <= 0.0 || g_frame.traceMask == 0) {
        output[pixel] = 0.0;
        return;
    }
    float3 normal = surface.xyz;
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    uint seed = PixelSeed(id.xy, 0x51u);
    float3 origin = position + normal * (g_frame.normalBias * (1.0 + surface.w * 0.002));
    uint samples = max(g_pass.step, 1u);
    float3 estimate = 0.0;
    for (uint s = 0; s < samples; ++s) {
        float3 direction = CosineSample(normal, Random(seed), Random(seed));
        HitSurface hit;
        if (TraceClosest(origin, direction, kMaxDistance, g_frame.traceMask, hit))
            estimate += hit.albedo * HitIrradiance(hit, seed);
        else
            estimate += g_frame.skyColor;
    }
    output[pixel] = float4(estimate / samples, 1.0);
}
