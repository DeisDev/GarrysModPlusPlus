#include "baked.hlsli"
#include "radiance_cache.hlsli"

// Cosine-weighted diffuse bounces per pixel. The result is an irradiance estimate: a sky miss returns the sky
// irradiance, a hit the light leaving it towards the pixel, its albedo times the light reaching it plus its glow. With
// the radiance cache that light comes from the cache where the hit is far enough away (CachedRadiance). Without it,
// where the hit was on screen last frame, it is last frame's whole irradiance there (exact direct plus denoised light).
// Elsewhere it is the sun, one local light picked by SampleLight and g_frame.indirectBounces - 1 more traced bounces lit
// the same way, which end in the cache where it has their light, or, for the viewmodel's many rays, the sun and one local
// light with the main view's indirect light volume.
// The sampled remainder of the direct light (direct.hlsl) is added, so the denoiser handles both.
// input0: last frame's denoised light, or none; input1: sampled direct remainder; input2: last frame's exact direct
// light; output0: raw estimate (rgb) and the rays' mean hit distance (a; NRD's, shaders/nrd.hlsl); step: samples per
// pixel.
// With indirect_lighting = 1 (g_frame.bakedTrianglesView) nothing is traced: the world takes the baked bounce light
// (shaders/baked.hlsli, shaders/baked_bounce.hlsl), other surfaces the leaf ambient light, and the denoiser still gets the remainder.

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

// Indirect light at an off-screen hit: more diffuse bounces, each seeing the sky, the light the radiance cache holds for
// the surface it meets, or that surface lit by the sun and one local light, through the water on the way
// (TraceThroughWater). Below foliage_shadows' "All light", rays after the first pass through shadowless props
// (g_frame.bounceMask): in grass they cross many alpha-tested blades for light the denoiser blurs anyway.
float3 FurtherBounces(HitSurface hit, float3 origin, inout uint seed)
{
    float3 irradiance = 0.0;
    float3 throughput = 1.0;
    for (uint bounce = 1; bounce < g_frame.indirectBounces; ++bounce) {
        float3 direction = CosineSample(hit.normal, Random(seed), Random(seed));
        HitSurface next;
        HitInfo nextHit;
        float3 water;
        float3 waterLight;
        bool found = TraceThroughWater(origin, direction, kMaxDistance, g_frame.bounceMask, g_frame.indirectRayFlags, seed, next, nextHit, water, waterLight);
        irradiance += throughput * waterLight;
        throughput *= water;
        if (!found)
            return irradiance + throughput * SkyLight(direction);
        float3 emission = BounceEmission(next, nextHit, false);
        float3 cached;
        if (CachedRadiance(next, emission, nextHit.t, nextHit.t, cached))
            return irradiance + throughput * cached;
        origin = next.position + next.normal;
        irradiance += throughput * (next.albedo * HitDirectIrradiance(next, origin, g_frame.bounceMask, seed) + emission);
        throughput *= next.albedo;
        hit = next;
    }
    return irradiance;
}


// The light leaving the first hit of a pixel's indirect ray towards it.
float3 HitRadiance(HitSurface hit, HitInfo hitInfo, inout uint seed)
{
    float3 emission = BounceEmission(hit, hitInfo, true);
    float3 cached;
    if (CachedRadiance(hit, emission, hitInfo.t, hitInfo.t, cached))
        return cached;
    if (!RadianceCacheEnabled()) {
        float3 previous = PreviousIrradiance(hit.position, hit.normal);
        if (previous.x >= 0.0)
            return hit.albedo * previous + emission;
    }
    float3 origin = hit.position + hit.normal * 1.0;
    // The viewmodel's many rays take the main view's indirect light volume in place of further bounces.
    float3 volume = g_frame.viewmodel != 0 ? VolumeIrradiance(hit.position, hit.normal) : -1.0;
    float3 further = volume.x >= 0.0 ? volume : FurtherBounces(hit, origin, seed);
    return hit.albedo * (HitDirectIrradiance(hit, origin, g_frame.bounceMask, seed) + further) + emission;
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
    // Leaves (props that ignore normals) turn their normal towards the eye and take kLeafTransmission of their light
    // through them from behind (LeafTerm, without its forward lobe); rays through them start on the far side.
    bool leaf;
    normal = LitNormal(pixel, normal, leaf);
    Texture2D<float4> remainder = ResourceDescriptorHeap[g_pass.input1];
    if (g_frame.bakedTrianglesView != kNoView) {
        // No ray was traced: NRD takes the hit distance as unoccluded.
        output[pixel] = float4(BakedIndirect(pixel, position, normal, leaf) + remainder.Load(int3(pixel, 0)).rgb, kMaxDistance);
        return;
    }
    uint samples = max(g_pass.step, 1u);
    float3 estimate = 0.0;
    float hitDistance = 0.0;
    for (uint s = 0; s < samples; ++s) {
        // A leaf's side is picked in proportion to the light it takes from it, which leaves each sample weighing 1.
        bool through = false;
        if (leaf)
            through = Random(seed) < kLeafTransmission;
        float3 side = through ? -geometricNormal : geometricNormal;
        float3 direction = AboveSurface(CosineSample(through ? -normal : normal, Random(seed), Random(seed)), side);
        HitSurface hit;
        HitInfo hitInfo;
        float3 water;
        float3 waterLight;
        if (TraceThroughWater(through ? position - geometricNormal * bias : origin, direction, kMaxDistance, g_frame.indirectMask, g_frame.indirectRayFlags, seed,
                hit, hitInfo, water, waterLight)) {
            estimate += ClampSample(HitRadiance(hit, hitInfo, seed) * water + waterLight);
            hitDistance += hitInfo.t;
        } else {
            estimate += SkyLight(direction) * water + waterLight;
            hitDistance += kMaxDistance;
        }
    }
    output[pixel] = float4(estimate / samples + remainder.Load(int3(pixel, 0)).rgb, hitDistance / samples);
}
