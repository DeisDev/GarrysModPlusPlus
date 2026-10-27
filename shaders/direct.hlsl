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

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float4> exactOutput = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<float4> remainderOutput = ResourceDescriptorHeap[g_pass.output1];
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    if (surface.w <= 0.0) {
        exactOutput[pixel] = 0.0;
        remainderOutput[pixel] = 0.0;
        return;
    }
    float3 normal = surface.xyz;
    float3 position = LitPosition(WorldPositionFromDepth(pixel, DepthAt(pixel)));
    float3 origin = SurfaceRayOrigin(position, GeometricNormalAt(pixel), surface.w);
    uint seed = PixelSeed(pixel, 0x2Du);
    Texture2D<uint> pixelFlags = ResourceDescriptorHeap[g_frame.pixelFlagsView];
    float3 lightNormal = (pixelFlags.Load(int3(pixel, 0)) & kPixelIgnoreNormals) != 0 ? 0.0 : normal;

    float3 exact = 0.0;
    float sunVisibility = 0.0;
    float lambert = Lambert(lightNormal, g_frame.sunDirection);
    if (g_frame.sunEnabled != 0.0 && lambert > 0.0) {
        sunVisibility = SunVisibility(origin, g_frame.sunShadowMask);
        exact += g_frame.sunColor * lambert * sunVisibility;
    }

    LightSet set = LightSetAt(position, normal);
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
    for (uint i = 0; i < set.count; ++i) {
        float3 toLight;
        float distance;
        float weight = Luminance(SetLightIrradiance(set, i, position, lightNormal, toLight, distance));
        if (weight <= VisibleIrradiance())
            continue;
        // Insert into the strongest lights; whatever drops out (this light or a displaced one) joins the remainder.
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
            continue;
        remainder += candidateWeight;
        [unroll]
        for (uint s = 0; s < kRemainderSamples; ++s) {
            if (s < g_frame.sampledLightRays && Random(seed) * remainder < candidateWeight) {
                picked[s] = candidate;
                pickedWeight[s] = candidateWeight;
            }
        }
    }
    [unroll]
    for (uint k = 0; k < kExactLights; ++k) {
        if (k >= g_frame.exactLights || topWeight[k] <= 0.0)
            break;
        float3 toLight;
        float distance;
        float3 light = SetLightIrradiance(set, top[k], position, lightNormal, toLight, distance);
        bool attached = IsAttachedLight(set, top[k]);
        exact += light * Transmittance(origin, toLight, ShadowRayLength(attached, distance), ShadowMask(attached));
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
            bool attached = IsAttachedLight(set, picked[s]);
            float transmittance = Transmittance(origin, toLight, ShadowRayLength(attached, distance), ShadowMask(attached));
            sampled += ClampSample(light * (transmittance * remainder / (pickedWeight[s] * g_frame.sampledLightRays)));
        }
    }
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
