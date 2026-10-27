#include "scene.hlsli"

// One cosine-weighted diffuse bounce per pixel. The result is an irradiance estimate: a sky miss returns
// the sky irradiance, a hit returns its albedo times the light reaching it. Where the hit was on screen last
// frame that is last frame's whole irradiance there (exact direct plus denoised light); elsewhere it is the sun,
// one local light picked by SampleLight and one more traced bounce.
// The sampled remainder of the direct light (direct.hlsl) is added, so the denoiser handles both.
// input0: last frame's denoised light, or none; input1: sampled direct remainder; input2: last frame's exact direct
// light; output0: raw estimate; step: samples per pixel.

static const float kMaxDistance = 16384.0;

float3 PreviousIrradiance(float3 position, float3 normal)
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
    Texture2D<float4> denoised = ResourceDescriptorHeap[g_pass.input0];
    Texture2D<float4> direct = ResourceDescriptorHeap[g_pass.input2];
    return denoised.Load(int3(pixel, 0)).rgb + direct.Load(int3(pixel, 0)).rgb;
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
    float3 previous = PreviousIrradiance(hit.position, hit.normal);
    if (previous.x >= 0.0)
        return previous;
    float3 origin = hit.position + hit.normal * 1.0;
    float3 irradiance = SunIrradiance(hit, origin);
    float3 light;
    float3 toLight;
    float distance;
    bool attached;
    if (SampleLight(LightSetAt(hit.position, hit.normal), hit.position, hit.normal, seed, light, toLight, distance, attached))
        irradiance += light * Transmittance(origin, toLight, ShadowRayLength(attached, distance), ShadowMask(attached));
    return irradiance + SecondBounce(hit, origin, seed);
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
    float3 position = LitPosition(WorldPositionFromDepth(pixel, DepthAt(pixel)));
    uint seed = PixelSeed(id.xy, 0x51u);
    float3 origin = position + normal * (g_frame.normalBias * (1.0 + surface.w * 0.002));
    uint samples = max(g_pass.step, 1u);
    float3 estimate = 0.0;
    for (uint s = 0; s < samples; ++s) {
        float3 direction = CosineSample(normal, Random(seed), Random(seed));
        HitSurface hit;
        if (TraceClosest(origin, direction, kMaxDistance, g_frame.traceMask, hit))
            estimate += ClampSample(hit.albedo * HitIrradiance(hit, seed));
        else
            estimate += g_frame.skyColor;
    }
    Texture2D<float4> remainder = ResourceDescriptorHeap[g_pass.input1];
    output[pixel] = float4(estimate / samples + remainder.Load(int3(pixel, 0)).rgb, 1.0);
}
