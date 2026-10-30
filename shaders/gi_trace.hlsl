#include "baked.hlsli"

// Cosine-weighted diffuse bounces per pixel. The result is an irradiance estimate: a sky miss returns
// the sky irradiance, a hit returns its albedo times the light reaching it. Where the hit was on screen last
// frame that is last frame's whole irradiance there (exact direct plus denoised light); elsewhere it is the sun,
// one local light picked by SampleLight and g_frame.indirectBounces - 1 more traced bounces, or, for the viewmodel's
// many rays, the main view's irradiance volume and the projected lights.
// The sampled remainder of the direct light (direct.hlsl) is added, so the denoiser handles both.
// input0: last frame's denoised light, or none; input1: sampled direct remainder; input2: last frame's exact direct
// light; output0: raw estimate; step: samples per pixel.
// With indirect_lighting = 1 (g_frame.bakedTrianglesView) nothing is traced: the world takes VRAD's baked bounce
// light (shaders/baked.hlsli), other surfaces the leaf ambient light, and the denoiser still gets the remainder.

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

// Indirect light at an off-screen hit: more diffuse bounces, each seeing the sky or a sunlit surface. Rays after the
// first pass through shadowless props (g_frame.bounceMask): in grass they would cross many alpha-tested blades for
// light the denoiser blurs anyway.
float3 FurtherBounces(HitSurface hit, float3 origin, inout uint seed)
{
    float3 irradiance = 0.0;
    float3 throughput = 1.0;
    for (uint bounce = 1; bounce < g_frame.indirectBounces; ++bounce) {
        float3 direction = CosineSample(hit.normal, Random(seed), Random(seed));
        HitSurface next;
        if (!TraceClosest(origin, direction, kMaxDistance, g_frame.bounceMask, next, g_frame.indirectRayFlags))
            return irradiance + throughput * g_frame.skyColor;
        origin = next.position + next.normal;
        irradiance += throughput * next.albedo * SunIrradiance(next, origin, g_frame.bounceMask);
        throughput *= next.albedo;
        hit = next;
    }
    return irradiance;
}

// The main view's filled irradiance volume at a point, or -1 where there is none.
float3 VolumeIrradiance(float3 position, float3 normal)
{
    if (g_frame.probeRedView == kNoView)
        return -1.0;
    static const float3 kGrid = float3(32.0, 32.0, 16.0);
    float3 window = (position / g_frame.probeSpacing - 0.5) - float3(g_frame.probeOrigin);
    if (any(window < 0.0) || any(window > kGrid - 1.0))
        return -1.0;
    float3 uvw = position / (g_frame.probeSpacing * kGrid);
    Texture3D<float4> red = ResourceDescriptorHeap[g_frame.probeRedView];
    Texture3D<float4> green = ResourceDescriptorHeap[g_frame.probeGreenView];
    Texture3D<float4> blue = ResourceDescriptorHeap[g_frame.probeBlueView];
    float4 r = red.SampleLevel(g_linearWrap, uvw, 0.0);
    float4 g = green.SampleLevel(g_linearWrap, uvw, 0.0);
    float4 b = blue.SampleLevel(g_linearWrap, uvw, 0.0);
    return max(float3(r.x + dot(r.yzw, normal), g.x + dot(g.yzw, normal), b.x + dot(b.yzw, normal)), 0.0);
}

// The projected lights (flashlights) at a hit, which the irradiance volume leaves out.
float3 ProjectorLight(HitSurface hit, float3 origin)
{
    float3 irradiance = 0.0;
    for (uint p = 0; p < g_frame.projectorCount; ++p) {
        Projector projector = LoadProjector(p);
        float3 toLight;
        float distance;
        float3 light = ProjectorIrradiance(projector, hit.position, hit.normal, toLight, distance);
        if (Luminance(light) > 0.0)
            irradiance += light * Transmittance(origin, toLight, ShadowRayLength(ProjectorClearance(projector), distance), ShadowMask(true));
    }
    return irradiance;
}

float3 HitIrradiance(HitSurface hit, inout uint seed)
{
    float3 previous = PreviousIrradiance(hit.position, hit.normal);
    if (previous.x >= 0.0)
        return previous;
    float3 origin = hit.position + hit.normal * 1.0;
    float3 volume = VolumeIrradiance(hit.position, hit.normal);
    if (volume.x >= 0.0)
        return volume + ProjectorLight(hit, origin);
    float3 irradiance = SunIrradiance(hit, origin, g_frame.bounceMask);
    float3 light;
    float3 toLight;
    float distance;
    uint picked;
    LightSet set = LightSetAt(hit.position, hit.normal);
    if (SampleLight(set, hit.position, hit.normal, seed, light, toLight, distance, picked))
        irradiance += light * LightTransmittance(set, picked, origin, toLight, distance);
    return irradiance + FurtherBounces(hit, origin, seed);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    id.xy = DispatchPixel(id.xy);
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
    float bias = SurfaceBias(surface.w);
    float3 geometricNormal = GeometricNormalAt(pixel);
    float3 origin = SurfaceRayOrigin(position, geometricNormal, surface.w);
    // Props that ignore normals gather light from every direction, as VRAD lit them; rays through their thin
    // surfaces start on the far side.
    Texture2D<uint> pixelFlags = ResourceDescriptorHeap[g_frame.pixelFlagsView];
    bool ignoreNormals = (pixelFlags.Load(int3(pixel, 0)) & kPixelIgnoreNormals) != 0;
    Texture2D<float4> remainder = ResourceDescriptorHeap[g_pass.input1];
    if (g_frame.bakedTrianglesView != kNoView) {
        float3 baked = BakedIrradiance(pixel, normal);
        if (baked.x < 0.0)
            baked = LeafAmbient(position, ignoreNormals ? 0.0 : normal);
        if (baked.x < 0.0)
            baked = g_frame.skyColor;
        output[pixel] = float4(baked + remainder.Load(int3(pixel, 0)).rgb, 1.0);
        return;
    }
    uint samples = max(g_pass.step, 1u);
    float3 estimate = 0.0;
    for (uint s = 0; s < samples; ++s) {
        float3 direction = ignoreNormals ? UniformSphere(Random(seed), Random(seed))
                                           : AboveSurface(CosineSample(normal, Random(seed), Random(seed)), geometricNormal);
        HitSurface hit;
        if (TraceClosest(ignoreNormals ? position + direction * bias : origin, direction, kMaxDistance, g_frame.indirectMask, hit, g_frame.indirectRayFlags))
            estimate += ClampSample(hit.albedo * HitIrradiance(hit, seed));
        else
            estimate += g_frame.skyColor;
    }
    output[pixel] = float4(estimate / samples + remainder.Load(int3(pixel, 0)).rgb, 1.0);
}
