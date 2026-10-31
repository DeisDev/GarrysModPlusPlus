#include "phong.hlsli"

// Direct light of each G-buffer pixel. The sun and the g_frame.exactLights strongest local lights get a shadow ray
// each; the other lights share g_frame.sampledLightRays shadow rays, picked in proportion to their light in one pass
// (weighted reservoir sampling) and weighted to keep the sum unbiased. The exact part stays sharp; the noisy
// remainder is added to the indirect estimate (gi_trace.hlsl) and denoised with it. The SUN_ONLY variant lights with
// the sun alone, for local lights sampled by RTXDI (shaders/restir.hlsl), which writes the remainder.
// input0: the sun's visibility when traced softly beforehand (SunShadowAt), else it is traced here, to the sun's centre.
// output0: exact direct irradiance (rgb) and sun visibility (a); output1: sampled remainder; output2: highlights.

// The most g_frame.exactLights and g_frame.sampledLightRays can be. Larger sets take registers that slow the light
// loop, so only settings above the High preset's 4 and 2 use the wide variant (WIDE_LIGHT_SETS).
#if WIDE_LIGHT_SETS
static const uint kExactLights = 8;
static const uint kRemainderSamples = 4;
#else
static const uint kExactLights = 4;
static const uint kRemainderSamples = 2;
#endif

static const uint kNoList = 0xFFFFFFFFu;

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
    result.flags = WaveReadLaneAt(light.flags, lane);
    result.size = WaveReadLaneAt(light.size, lane);
    result.style = WaveReadLaneAt(light.style, lane);
    result.referenceScale = WaveReadLaneAt(light.referenceScale, lane);
    result.padding = 0;
    return result;
}

// Adds a light of a pixel's set to the remainder by weighted reservoir sampling: `luminance` is its unshadowed
// luminance, `selection` the weight it is picked by, luminance over the probability it was drawn with (1 for listed
// lights). The remainder sums the selection weights, which estimates the sum of the lights' luminance.
void AddToRemainder(uint i, float luminance, float selection, inout uint picked[kRemainderSamples], inout float pickedWeight[kRemainderSamples],
    inout float remainder, inout uint seed)
{
    remainder += selection;
    [unroll]
    for (uint s = 0; s < kRemainderSamples; ++s) {
        if (s < g_frame.sampledLightRays && Random(seed) * remainder < selection) {
            picked[s] = i;
            pickedWeight[s] = luminance;
        }
    }
}

// Adds the i-th light of a pixel's set, with its unshadowed light, to the strongest lights (unless `exact` is false);
// whatever drops out (this light or a displaced one) joins the remainder and may become one of its samples.
void ConsiderLight(uint i, float3 irradiance, bool exact, inout uint top[kExactLights], inout float topWeight[kExactLights],
    inout uint picked[kRemainderSamples], inout float pickedWeight[kRemainderSamples], inout float remainder, inout float weakest, inout uint seed)
{
    float weight = Luminance(irradiance);
    if (weight <= 0.0)
        return;
    uint candidate = i;
    float candidateWeight = weight;
    if (exact && weight > weakest) {
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
    if (candidateWeight > 0.0)
        AddToRemainder(candidate, candidateWeight, candidateWeight, picked, pickedWeight, remainder, seed);
}

// The viewmodel is lit without a denoiser or temporal resolve, and the world's shadows fall on it wide and soft (its
// surfaces are inches from the eye), so its shadow rays cover the sun's disc (g_frame.viewmodelShadowRays) and each
// exact light's sphere (a quarter as many) at once. A single ray aims at the centre: a random point would leave grain.

// The k-th of `count` points spread evenly over a disc (a golden-angle spiral), turned by `turn`, as the area share and
// angle that DiscPoint and SunDiscDirectionAt take.
float2 SpiralPoint(uint k, uint count, float2 turn)
{
    return float2((float(k) + turn.x) / float(count), frac(float(k) * 0.618034 + turn.y));
}

float3 ViewmodelSunVisibility(float3 from, uint rayFlags, inout uint seed)
{
    uint rays = max(g_frame.viewmodelShadowRays, 1u);
    if (rays == 1u)
        return SunVisibility(from, g_frame.sunShadowMask, rayFlags);
    float2 turn = float2(Random(seed), Random(seed));
    float3 sum = 0.0;
    for (uint k = 0; k < rays; ++k)
        sum += SunVisibilityAlong(from, SunDiscDirectionAt(SpiralPoint(k, rays, turn)), g_frame.sunShadowMask, rayFlags);
    return sum / float(rays);
}

// LightTransmittance to a random point of an exact light, or for the viewmodel to its centre or the mean over points
// spread over it. Attached lights are points.
float3 ExactLightTransmittance(LightSet set, uint i, float3 from, float3 lightPosition, uint rayFlags, inout uint seed)
{
    uint rays = g_frame.viewmodel != 0 && !IsAttachedLight(set, i) ? max(g_frame.viewmodelShadowRays / 4u, 1u) : 1u;
    float2 turn = float2(Random(seed), Random(seed));
    if (rays == 1u)
        return LightTransmittance(set, i, from, lightPosition, g_frame.viewmodel != 0 ? kLightCentre : turn, rayFlags);
    float3 sum = 0.0;
    for (uint k = 0; k < rays; ++k)
        sum += LightTransmittance(set, i, from, lightPosition, SpiralPoint(k, rays, turn), rayFlags);
    return sum / float(rays);
}

// The main view's soft sun shadows (shaders/sun_shadow.hlsl) in input0, NRD's translucency encoding: denoised by SIGMA
// when input1 is 1, which stores the square root (NRD.hlsli, SIGMA_BackEnd_UnpackShadow).
float3 SunShadowAt(int2 pixel)
{
    Texture2D<float4> shadow = ResourceDescriptorHeap[g_pass.input0];
    float3 visibility = shadow.Load(int3(pixel, 0)).yzw;
    return g_pass.input1 == 1 ? visibility * visibility : visibility;
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
    float3 faceNormal = lit ? GeometricNormalAt(pixel) : 0.0;
    // Leaves (props that ignore normals) take light from both sides with their normal towards the eye; shadow rays of
    // light that comes through them start behind them.
    g_twoSided = lit && (pixelFlags.Load(int3(pixel, 0)) & kPixelIgnoreNormals) != 0;
    g_toEye = lit ? normalize(g_frame.eyePosition - position) : 0.0;
    float3 lightNormal = g_twoSided && dot(normal, faceNormal) < 0.0 ? -normal : normal;
    float3 behind = g_twoSided ? position - faceNormal * SurfaceBias(surface.w) : origin;
    bool onModel = false;
    if (lit) {
        Texture2D<uint4> visibility = ResourceDescriptorHeap[g_frame.visibilityView];
        uint instance = visibility.Load(int3(pixel, 0)).x;
        onModel = instance != kNoView && (LoadInstance(instance).flags & kInstanceModel) != 0;
        Texture2D<float4> gbufferFlags = ResourceDescriptorHeap[g_frame.flagsView];
        float4 marks = gbufferFlags.Load(int3(pixel, 0));
        g_halfLambert = (MaterialBits(marks) & kMaterialHalfLambert) != 0;
        g_lightWarpView = WarpsTotalLight(MaterialBits(marks)) ? kNoView : LightWarpView(marks);
    }

    float3 exact = 0.0;
    float3 specular = 0.0;
    float3 rim = 0.0;
    Phong phong = PhongAt(pixel, lit && inside, position, normal);
    float3 sunVisibility = 0.0;
    float lambert = Lambert(lightNormal, g_frame.sunDirection);
    if (lit && g_frame.sunEnabled != 0.0 && lambert > 0.0) {
        float3 sunFrom = ShadowOrigin(origin, behind, faceNormal, g_frame.sunDirection);
        uint sunFlags = SelfShadowFlags(onModel, faceNormal, g_frame.sunDirection);
        if (g_pass.input0 != kNoView)
            sunVisibility = SunShadowAt(pixel);
        else if (g_frame.viewmodel != 0)
            sunVisibility = ViewmodelSunVisibility(sunFrom, sunFlags, seed);
        else
            sunVisibility = SunVisibility(sunFrom, g_frame.sunShadowMask, sunFlags);
        exact += g_frame.sunColor * lambert * sunVisibility * LightWarp(lightNormal, g_frame.sunDirection);
        specular += SunHighlight(phong, g_frame.sunColor * (lambert * sunVisibility));
        rim += ModelRim(phong, g_frame.sunColor * (lambert * sunVisibility), g_frame.sunDirection);
    }

    float3 sampled = 0.0;
#if !SUN_ONLY
    LightSet set = (LightSet)0;
    if (lit)
        set = LightSetAt(position);
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
    // Listed map lights in list order, as their per-lane loads would wait on each other: the lanes that share a grid
    // cell load a wave's width of its lights at once, a light each, and hand them round. Lights the PVS leaves out
    // (kHiddenLight) take no exact shadow ray. Then the engine's dynamic and projected lights, the same for every pixel.
    uint list = lit && set.mapLights != 0 ? set.ranges.first : kNoList;
    bool pending = list != kNoList;
    [loop]
    for (;;) {
        uint first = WaveActiveMin(pending ? list : kNoList);
        if (first == kNoList)
            break;
        bool mine = pending && list == first;
        uint count = WaveActiveMax(mine ? set.mapLights : 0u);
        LightRanges ranges = {first, count, 0, 0};
        for (uint base = 0; base < count; base += WaveGetLaneCount()) {
            Light loaded = (Light)0;
            bool hidden = false;
            if (base + WaveGetLaneIndex() < count) {
                uint entry = LightRangeEntry(ranges, base + WaveGetLaneIndex());
                loaded = LoadLight(entry & ~kHiddenLight);
                hidden = (entry & kHiddenLight) != 0;
            }
            uint batch = min(WaveGetLaneCount(), count - base);
            for (uint j = 0; j < batch; ++j) {
                Light light = WaveReadLight(loaded, j);
                bool exactLight = !WaveReadLaneAt(hidden, j);
                if (mine) {
                    float3 toLight;
                    float distance;
                    ConsiderLight(base + j, LightIrradiance(light, position, lightNormal, toLight, distance), exactLight, top, topWeight, picked,
                        pickedWeight, remainder, weakest, seed);
                }
            }
        }
        pending = pending && !mine;
    }
    for (uint i = set.mapLights; i < set.count; ++i) {
        float3 toLight;
        float distance;
        ConsiderLight(i, SetLightIrradiance(set, i, position, lightNormal, toLight, distance), true, top, topWeight, picked, pickedWeight, remainder,
            weakest, seed);
    }
    // One light of the cell's tail, the lights it does not list, by its weight there: their light still arrives, sampled.
    uint tailLight;
    float probability;
    if (lit && SampleTailLight(set.ranges, Random(seed), tailLight, probability)) {
        float3 toLight;
        float distance;
        float luminance = Luminance(SetLightIrradiance(set, set.count + tailLight, position, lightNormal, toLight, distance));
        if (luminance > 0.0)
            AddToRemainder(set.count + tailLight, luminance, luminance / probability, picked, pickedWeight, remainder, seed);
    }
    [unroll]
    for (uint k = 0; k < kExactLights; ++k) {
        if (k >= g_frame.exactLights || topWeight[k] <= 0.0)
            break;
        float3 toLight;
        float distance;
        float3 light = SetLightIrradiance(set, top[k], position, lightNormal, toLight, distance);
        float3 shadowed = light * ExactLightTransmittance(set, top[k], ShadowOrigin(origin, behind, faceNormal, toLight), position + toLight * distance,
            SelfShadowFlags(onModel, faceNormal, toLight), seed);
        exact += shadowed * LightWarp(lightNormal, toLight);
        specular += ModelHighlight(phong, shadowed, toLight);
        rim += ModelRim(phong, shadowed, toLight);
    }
    if (remainder > 0.0) {
        [unroll]
        for (uint s = 0; s < kRemainderSamples; ++s) {
            if (s >= g_frame.sampledLightRays)
                break;
            float3 toLight;
            float distance;
            float3 light = SetLightIrradiance(set, picked[s], position, lightNormal, toLight, distance);
            float3 transmittance = LightTransmittance(set, picked[s], ShadowOrigin(origin, behind, faceNormal, toLight), position + toLight * distance,
                float2(Random(seed), Random(seed)), SelfShadowFlags(onModel, faceNormal, toLight));
            sampled += ClampSample(light * LightWarp(lightNormal, toLight) * (transmittance * remainder / (pickedWeight[s] * g_frame.sampledLightRays)));
        }
    }
    // Glowing triangles as area lights (EmissiveLighting::Full): one point of one of them, with the sampled remainder.
    float3 target;
    float3 glow;
    if (lit && SampleEmitter(position, Random(seed), float2(Random(seed), Random(seed)), target, glow)) {        float3 toEmitter = target - position;
        float distanceSquared = max(dot(toEmitter, toEmitter), 1.0);
        float3 toLight = toEmitter * rsqrt(max(dot(toEmitter, toEmitter), 1e-8));
        float lambert = Lambert(lightNormal, toLight);
        if (lambert > 0.0) {
            float3 from = ShadowOrigin(origin, behind, faceNormal, toLight);
            float3 path = target - from;
            float distance = max(length(path), 1e-3);
            sampled += ClampSample(glow * (lambert / distanceSquared) *
                Transmittance(from, path / distance, ShadowRayLength(kLightClearance, distance), ShadowMask(false), SelfShadowFlags(onModel, faceNormal, toLight)));
        }
    }
#endif
    if (!inside)
        return;
    RWTexture2D<float4> exactOutput = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<float4> remainderOutput = ResourceDescriptorHeap[g_pass.output1];
    exactOutput[pixel] = float4(exact, Luminance(sunVisibility));
    remainderOutput[pixel] = float4(sampled, 1.0);
    if (g_pass.output2 != kNoView) {
        RWTexture2D<float4> specularOutput = ResourceDescriptorHeap[g_pass.output2];
        specularOutput[pixel] = float4(max(specular, rim), 0.0);
    }
    if (g_frame.probeView != kNoView && all(uint2(pixel) == uint2(g_frame.probePixel & 0xFFFF, g_frame.probePixel >> 16))) {
        RWByteAddressBuffer probe = ResourceDescriptorHeap[g_frame.probeView];
        HitInfo hit;
        bool blocked = TraceHit(origin, g_frame.sunDirection, 65536.0, g_frame.traceMask, hit);
        probe.Store4(0, uint4(blocked ? hit.instance : kNoView, hit.primitive, asuint(hit.t), asuint(Luminance(sunVisibility))));
        probe.Store4(16, uint4(asuint(origin), 0));
    }
}
