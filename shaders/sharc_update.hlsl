#define SHARC_UPDATE 1
#include "nvidia/sharc_common.hlsli"
#include "baked.hlsli"

// Fills the radiance cache (shaders/nvidia/sharc_common.hlsli) with paths from a pixel picked at random in each
// kBlock x kBlock block of the main view. Each vertex adds its albedo times its direct light (the sun and one local
// light picked by SampleLight, as off-screen hits are lit elsewhere) and the emission it meets, and its continuation
// is a cosine-weighted bounce, whose throughput (BRDF times cosine over the pdf) is the albedo. The cache spreads
// what later vertices find back to the earlier ones and may end a path early where it already knows the light. No
// Russian roulette: the cache must see whole paths. Each stretch is traced before its vertex is recorded: the water it
// crosses (TraceThroughWater) adds its own light to the vertex's and dims, bends or mirrors what lies beyond.

static const uint kBlock = 4;
static const uint kCacheBounces = 4;
static const float kMaxDistance = 16384.0;

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 blocks = (g_frame.size + kBlock - 1) / kBlock;
    if (any(id.xy >= blocks) || g_sampling.cacheEnabled == 0 || g_frame.traceMask == 0)
        return;
    uint seed = PixelSeed(id.xy, 0xC5u);
    uint pick = Hash(seed);
    int2 pixel = int2(min(id.xy * kBlock + uint2(pick % kBlock, (pick >> 8) % kBlock), g_frame.size - 1));
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    Texture2D<uint> pixelFlags = ResourceDescriptorHeap[g_frame.pixelFlagsView];
    uint flags = pixelFlags.Load(int3(pixel, 0));
    if (surface.w == 0.0 || (flags & kPixelUnlit) != 0)
        return;
    HitSurface current;
    float3 origin;
    float3 sampleNormal;
    float3 emission = 0.0;
    if (surface.w < 0.0) {
        // A water pixel seen from above (negative distance, shaders/surface.hlsl): the path starts on what the view
        // meets under the flat surface, so the cache holds the light under the water that refraction, indirect rays
        // and the viewmodel's rays find there.
        float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
        uint water = g_frame.eyeWater == kNoWater ? WaterBodyAt(position - float3(0.0, 0.0, 1.0)) : kNoWater;
        if (water == kNoWater)
            return;
        float3 surfacePoint = float3(position.xy, LoadWaterBody(water).surfaceZ);
        float3 view = normalize(surfacePoint - g_frame.eyePosition);
        float3 down = refract(view, float3(0.0, 0.0, 1.0), 1.0 / kWaterIor);
        HitInfo hitInfo;
        if (view.z >= 0.0 || !TraceClosest(surfacePoint - float3(0.0, 0.0, kWaterSurfaceOffset), down, kMaxDistance, g_frame.traceMask & ~kWaterMask, current,
                hitInfo))
            return;
        origin = current.position + current.normal;
        sampleNormal = current.normal;
        emission = BounceEmission(current, hitInfo, false);
    } else {
        Texture2D<float4> albedos = ResourceDescriptorHeap[g_frame.albedoView];
        // The visible surface is the path's first vertex, as a face (its geometric normal) of the G-buffer's albedo;
        // leaves take light from both sides (SunIrradiance).
        current.position = WorldPositionFromDepth(pixel, DepthAt(pixel));
        current.normal = GeometricNormalAt(pixel);
        current.albedo = saturate(albedos.Load(int3(pixel, 0)).rgb);
        current.flags = (flags & kPixelIgnoreNormals) != 0 ? kInstanceIgnoreNormals : 0u;
        origin = SurfaceRayOrigin(current.position, current.normal, surface.w);
        bool leaf;
        sampleNormal = LitNormal(pixel, surface.xyz, leaf);
    }

    SharcParameters parameters = CacheParameters();
    SharcState state;
    SharcInit(state);
    for (uint vertex = 0; vertex < kCacheBounces; ++vertex) {
        float3 demodulation = max(current.albedo, kMinDemodulation);
        float3 direct = current.albedo * HitDirectIrradiance(current, origin, g_frame.bounceMask, seed);
        float3 direction = AboveSurface(CosineSample(sampleNormal, Random(seed), Random(seed)), current.normal);
        HitSurface next;
        HitInfo nextHit;
        float3 water;
        float3 waterLight;
        bool found = TraceThroughWater(origin, direction, kMaxDistance, vertex == 0 ? g_frame.indirectMask : g_frame.bounceMask, g_frame.indirectRayFlags, seed,
            next, nextHit, water, waterLight);
        if (!SharcUpdateHit(parameters, state, CacheHit(current.position, current.normal, current.albedo, emission), direct + current.albedo * waterLight,
                Random(seed)))
            return;
        SharcSetThroughput(state, demodulation * water);
        if (!found) {
            SharcUpdateMiss(parameters, state, SkyLight(direction));
            return;
        }
        emission = BounceEmission(next, nextHit, false);
        current = next;
        origin = next.position + next.normal;
        sampleNormal = next.normal;
    }
}
