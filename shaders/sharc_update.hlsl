#define SHARC_UPDATE 1
#include "nvidia/sharc_common.hlsli"
#include "baked.hlsli"

// Fills the radiance cache (shaders/nvidia/sharc_common.hlsli) with paths from a pixel picked at random in each
// kBlock x kBlock block of the main view. Each vertex adds its albedo times its direct light (the sun and one local
// light picked by SampleLight, as off-screen hits are lit elsewhere) and the emission it meets, and its continuation
// is a cosine-weighted bounce, whose throughput (BRDF times cosine over the pdf) is the albedo. The cache spreads
// what later vertices find back to the earlier ones and may end a path early where it already knows the light. No
// Russian roulette: the cache must see whole paths.

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
    if (surface.w <= 0.0 || (flags & kPixelUnlit) != 0)
        return;
    Texture2D<float4> albedos = ResourceDescriptorHeap[g_frame.albedoView];
    // The visible surface is the path's first vertex, as a face (its geometric normal) of the G-buffer's albedo; leaves
    // take light from both sides (SunIrradiance).
    HitSurface current;
    current.position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    current.normal = GeometricNormalAt(pixel);
    current.albedo = saturate(albedos.Load(int3(pixel, 0)).rgb);
    current.flags = (flags & kPixelIgnoreNormals) != 0 ? kInstanceIgnoreNormals : 0u;
    float3 origin = SurfaceRayOrigin(current.position, current.normal, surface.w);
    bool leaf;
    float3 sampleNormal = LitNormal(pixel, surface.xyz, leaf);
    float3 emission = 0.0;

    SharcParameters parameters = CacheParameters();
    SharcState state;
    SharcInit(state);
    for (uint vertex = 0; vertex < kCacheBounces; ++vertex) {
        float3 demodulation = max(current.albedo, kMinDemodulation);
        float3 direct = current.albedo * HitDirectIrradiance(current, origin, g_frame.bounceMask, seed);
        if (!SharcUpdateHit(parameters, state, CacheHit(current.position, current.normal, current.albedo, emission), direct, Random(seed)))
            return;
        SharcSetThroughput(state, demodulation);
        float3 direction = AboveSurface(CosineSample(sampleNormal, Random(seed), Random(seed)), current.normal);
        HitSurface next;
        HitInfo nextHit;
        if (!TraceClosest(origin, direction, kMaxDistance, vertex == 0 ? g_frame.indirectMask : g_frame.bounceMask, next, nextHit, g_frame.indirectRayFlags)) {
            SharcUpdateMiss(parameters, state, SkyLight(direction));
            return;
        }
        emission = BounceEmission(next, nextHit, false);
        current = next;
        origin = next.position + next.normal;
        sampleNormal = next.normal;
    }
}
