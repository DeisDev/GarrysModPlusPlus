#include "scene.hlsli"

// Direct light of each G-buffer pixel. The sun and the g_frame.exactLights strongest local lights get a shadow ray
// each; the other lights share g_frame.sampledLightRays shadow rays, picked in proportion to their light in one pass
// (weighted reservoir sampling) and weighted to keep the sum unbiased. The exact part stays sharp; the noisy
// remainder is added to the indirect estimate (gi_trace.hlsl) and denoised with it.
// output0: exact direct irradiance (rgb) and sun visibility (a); output1: sampled remainder.

// The most g_frame.exactLights and g_frame.sampledLightRays can be. Larger sets take registers that slow the light
// loop, so only settings above the High preset's 4 and 2 use the wide variant (WIDE_LIGHT_SETS).
#if WIDE_LIGHT_SETS
static const uint kExactLights = 8;
static const uint kRemainderSamples = 4;
#else
static const uint kExactLights = 4;
static const uint kRemainderSamples = 2;
#endif

static const uint kNoCluster = 0xFFFFFFFFu;

Light WaveReadLight(Light light, uint lane)
{
    Light result;
    result.origin = WaveReadLaneAt(light.origin, lane);
    result.type = WaveReadLaneAt(light.type, lane);
    result.intensity = WaveReadLaneAt(light.intensity, lane);
    result.radius = WaveReadLaneAt(light.radius, lane);
    result.normal = WaveReadLaneAt(light.normal, lane);
    result.stopdot = WaveReadLaneAt(light.stopdot, lane);
    result.stopdot2 = WaveReadLaneAt(light.stopdot2, lane);
    result.exponent = WaveReadLaneAt(light.exponent, lane);
    result.constantAttn = WaveReadLaneAt(light.constantAttn, lane);
    result.linearAttn = WaveReadLaneAt(light.linearAttn, lane);
    result.quadraticAttn = WaveReadLaneAt(light.quadraticAttn, lane);
    result.hullOffset = WaveReadLaneAt(light.hullOffset, lane);
    result.hullCount = WaveReadLaneAt(light.hullCount, lane);
    result.padding = 0.0;
    return result;
}

// Adds the i-th light of a pixel's set, with its unshadowed light, to the strongest lights; whatever drops out (this
// light or a displaced one) joins the remainder and may become one of its samples.
void ConsiderLight(uint i, float3 irradiance, inout uint top[kExactLights], inout float topWeight[kExactLights], inout uint picked[kRemainderSamples],
    inout float pickedWeight[kRemainderSamples], inout float remainder, inout float weakest, inout uint seed)
{
    float weight = Luminance(irradiance);
    if (weight <= VisibleIrradiance())
        return;
    uint candidate = i;
    float candidateWeight = weight;
    if (weight > weakest) {
        [unroll]
        for (uint k = 0; k < kExactLights; ++k) {
            if (k < g_frame.exactLights && candidateWeight > topWeight[k]) {
                uint index = top[k];
                float w = topWeight[k];
                top[k] = candidate;
                topWeight[k] = candidateWeight;
                candidate = index;
                candidateWeight = w;
            }
            if (k + 1 == g_frame.exactLights)
                weakest = topWeight[k];
        }
    }
    if (candidateWeight <= 0.0)
        return;
    remainder += candidateWeight;
    [unroll]
    for (uint s = 0; s < kRemainderSamples; ++s) {
        if (s < g_frame.sampledLightRays && Random(seed) * remainder < candidateWeight) {
            picked[s] = candidate;
            pickedWeight[s] = candidateWeight;
        }
    }
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    id.xy = DispatchPixel(id.xy);
    // Every lane runs to the end: the lanes of a wave load their clusters' lights together.
    int2 pixel = int2(id.xy);
    bool inside = all(id.xy < g_frame.size);
    float4 surface = inside ? SurfaceAt(g_frame.surfaceView, pixel) : 0.0;
    bool lit = surface.w > 0.0;
    float3 normal = lit ? surface.xyz : float3(0.0, 0.0, 1.0);
    float3 position = lit ? LitPosition(WorldPositionFromDepth(pixel, DepthAt(pixel))) : 0.0;
    float3 origin = lit ? SurfaceRayOrigin(position, GeometricNormalAt(pixel), surface.w) : 0.0;
    uint seed = PixelSeed(pixel, 0x2Du);
    Texture2D<uint> pixelFlags = ResourceDescriptorHeap[g_frame.pixelFlagsView];
    float3 lightNormal = lit && (pixelFlags.Load(int3(pixel, 0)) & kPixelIgnoreNormals) != 0 ? 0.0 : normal;

    float3 exact = 0.0;
    float sunVisibility = 0.0;
    float lambert = Lambert(lightNormal, g_frame.sunDirection);
    if (lit && g_frame.sunEnabled != 0.0 && lambert > 0.0) {
        sunVisibility = SunVisibility(origin, g_frame.sunShadowMask);
        exact += g_frame.sunColor * lambert * sunVisibility;
    }

    LightSet set = (LightSet)0;
    if (lit)
        set = LightSetAt(position, normal);
    uint top[kExactLights];
    float topWeight[kExactLights];
    uint picked[kRemainderSamples];
    float pickedWeight[kRemainderSamples];
    [unroll]
    for (uint k = 0; k < kExactLights; ++k) {
        top[k] = 0;
        topWeight[k] = 0.0;
    }
    [unroll]
    for (uint s = 0; s < kRemainderSamples; ++s) {
        picked[s] = 0;
        pickedWeight[s] = 1.0;
    }
    float remainder = 0.0;
    // The weakest of the strongest lights once they are all found; most lights are weaker and skip the insertion.
    float weakest = 0.0;
    // Map lights in index order, as their per-lane loads would wait on each other: the lanes that share a cluster
    // load a wave's width of its lights at once, a light each, and hand them round. Then the engine's dynamic and
    // projected lights, the same for every pixel.
    uint cluster = lit && set.mapLights != 0 ? set.ranges.first : kNoCluster;
    bool pending = cluster != kNoCluster;
    [loop]
    for (;;) {
        uint first = WaveActiveMin(pending ? cluster : kNoCluster);
        if (first == kNoCluster)
            break;
        bool mine = pending && cluster == first;
        uint count = WaveActiveMax(mine ? set.mapLights : 0u);
        LightRanges ranges = {first, count};
        for (uint base = 0; base < count; base += WaveGetLaneCount()) {
            Light loaded = (Light)0;
            if (base + WaveGetLaneIndex() < count)
                loaded = LoadLight(LightRangeIndex(ranges, base + WaveGetLaneIndex()));
            uint batch = min(WaveGetLaneCount(), count - base);
            for (uint j = 0; j < batch; ++j) {
                Light light = WaveReadLight(loaded, j);
                if (mine) {
                    float3 toLight;
                    float distance;
                    ConsiderLight(base + j, LightIrradiance(light, position, lightNormal, toLight, distance), top, topWeight, picked, pickedWeight,
                        remainder, weakest, seed);
                }
            }
        }
        pending = pending && !mine;
    }
    for (uint i = set.mapLights; i < set.count; ++i) {
        float3 toLight;
        float distance;
        ConsiderLight(i, SetLightIrradiance(set, i, position, lightNormal, toLight, distance), top, topWeight, picked, pickedWeight, remainder,
            weakest, seed);
    }
    [unroll]
    for (uint k = 0; k < kExactLights; ++k) {
        if (k >= g_frame.exactLights || topWeight[k] <= 0.0)
            break;
        float3 toLight;
        float distance;
        float3 light = SetLightIrradiance(set, top[k], position, lightNormal, toLight, distance);
        exact += light * LightTransmittance(set, top[k], origin, toLight, distance);
    }
    float3 sampled = 0.0;
    if (remainder > 0.0) {
        [unroll]
        for (uint s = 0; s < kRemainderSamples; ++s) {
            if (s >= g_frame.sampledLightRays)
                break;
            float3 toLight;
            float distance;
            float3 light = SetLightIrradiance(set, picked[s], position, lightNormal, toLight, distance);
            float transmittance = LightTransmittance(set, picked[s], origin, toLight, distance);
            sampled += ClampSample(light * (transmittance * remainder / (pickedWeight[s] * g_frame.sampledLightRays)));
        }
    }
    if (!inside)
        return;
    RWTexture2D<float4> exactOutput = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<float4> remainderOutput = ResourceDescriptorHeap[g_pass.output1];
    exactOutput[pixel] = float4(exact, sunVisibility);
    remainderOutput[pixel] = float4(sampled, 1.0);
    if (g_frame.probeView != kNoView && all(uint2(pixel) == uint2(g_frame.probePixel & 0xFFFF, g_frame.probePixel >> 16))) {
        RWByteAddressBuffer probe = ResourceDescriptorHeap[g_frame.probeView];
        HitInfo hit;
        bool blocked = TraceHit(origin, g_frame.sunDirection, 65536.0, g_frame.traceMask, hit);
        probe.Store4(0, uint4(blocked ? hit.instance : kNoView, hit.primitive, asuint(hit.t), asuint(sunVisibility)));
        probe.Store4(16, uint4(asuint(origin), 0));
    }
}
