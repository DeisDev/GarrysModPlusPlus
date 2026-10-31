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

// A pixel's Phong highlight from its twin (shaders/twins): the exponent in the emissive target's alpha (negative for
// Source's model specular, positive for GMod's brush Phong, 0 for none) and the strength in the flags' blue.
struct Phong
{
    float exponent;
    float strength;
    float3 normal;
    float3 toEye;
    // Rim light (Source's SpecularAndRimTerms), or exponent 0.
    float rimExponent;
    float rimScale;
};

Phong PhongAt(int2 pixel, bool lit, float3 position, float3 normal)
{
    Phong phong = (Phong)0;
    if (!lit)
        return phong;
    Texture2D<float4> emissive = ResourceDescriptorHeap[g_frame.emissiveView];
    phong.exponent = emissive.Load(int3(pixel, 0)).a;
    if (phong.exponent == 0.0)
        return phong;
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    float4 marks = flags.Load(int3(pixel, 0));
    phong.strength = marks.b;
    phong.normal = normal;
    phong.toEye = normalize(g_frame.eyePosition - position);
    if (phong.exponent < 0.0 && (MaterialBits(marks) & kMaterialRimLight) != 0) {
        Texture2D<float4> normals = ResourceDescriptorHeap[g_frame.gbufferNormalView];
        phong.rimExponent = normals.Load(int3(pixel, 0)).a;
        phong.rimScale = RimScale(marks);
    }
    return phong;
}

// Rim light from one light: pow(R.L, rim exponent) times the light reaching the surface, scaled by the rim's mask,
// Fresnel and tint. The skin shader takes the larger of the summed highlights and the summed rim.
float3 ModelRim(Phong phong, float3 light, float3 toLight)
{
    if (phong.rimExponent <= 0.0)
        return 0.0;
    float3 reflected = 2.0 * phong.normal * dot(phong.normal, phong.toEye) - phong.toEye;
    return light * (phong.rimScale * pow(saturate(dot(reflected, toLight)), phong.rimExponent));
}

// Models: Source's SpecularAndRimTerms (common_vertexlitgeneric_dx9.h), pow(R.L, exponent) times the light reaching
// the surface (with its N.L), as the skin shader sums it over its lights.
float3 ModelHighlight(Phong phong, float3 light, float3 toLight)
{
    if (phong.exponent >= 0.0)
        return 0.0;
    float3 reflected = 2.0 * phong.normal * dot(phong.normal, phong.toEye) - phong.toEye;
    return light * (phong.strength * pow(saturate(dot(reflected, toLight)), -phong.exponent));
}

// The sun's highlight. Brushes: GMod's lightmappedgeneric_ps30 PHONG combos compute pow(|N.H|, exponent) * sqrt(N.L) *
// strength * L * saturate(L) toward the sun (g_LightDir), with L the lightmap; here L is the traced direct sunlight.
float3 SunHighlight(Phong phong, float3 sunlight)
{
    if (phong.exponent < 0.0)
        return ModelHighlight(phong, sunlight, g_frame.sunDirection);
    if (phong.exponent == 0.0)
        return 0.0;
    float lambert = saturate(dot(phong.normal, g_frame.sunDirection));
    float3 halfway = normalize(phong.toEye + g_frame.sunDirection);
    return sunlight * saturate(sunlight) * (phong.strength * sqrt(lambert) * pow(saturate(abs(dot(phong.normal, halfway))), phong.exponent));
}

// On a dynamic model, a shadow ray towards a light its triangle faces away from (where the smoothed normal still faces
// it) starts into the model and meets the model's far side: shards along the terminator. Those rays skip the model's
// back faces. DXR's front faces appear clockwise from the ray origin in a left-handed system, so cross(p1 - p0, p2 - p0)
// points at the origin; Source's point out of the back, so Source's back faces are DXR's front faces.
uint SelfShadowFlags(bool onModel, float3 faceNormal, float3 toLight)
{
    return onModel && dot(faceNormal, toLight) < 0.0 ? RAY_FLAG_CULL_FRONT_FACING_TRIANGLES : RAY_FLAG_NONE;
}

// Where a pixel's shadow ray towards a light starts: in front of its face, or behind it for light that comes through a
// leaf from behind (`behind` is `origin` for other surfaces).
float3 ShadowOrigin(float3 origin, float3 behind, float3 faceNormal, float3 toLight)
{
    return dot(faceNormal, toLight) < 0.0 ? behind : origin;
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
        sunVisibility = SunVisibility(ShadowOrigin(origin, behind, faceNormal, g_frame.sunDirection), g_frame.sunShadowMask,
            SelfShadowFlags(onModel, faceNormal, g_frame.sunDirection));
        exact += g_frame.sunColor * lambert * sunVisibility * LightWarp(lightNormal, g_frame.sunDirection);
        specular += SunHighlight(phong, g_frame.sunColor * (lambert * sunVisibility));
        rim += ModelRim(phong, g_frame.sunColor * (lambert * sunVisibility), g_frame.sunDirection);
    }

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
        float3 shadowed = light * LightTransmittance(set, top[k], ShadowOrigin(origin, behind, faceNormal, toLight), position + toLight * distance,
            float2(Random(seed), Random(seed)), SelfShadowFlags(onModel, faceNormal, toLight));
        exact += shadowed * LightWarp(lightNormal, toLight);
        specular += ModelHighlight(phong, shadowed, toLight);
        rim += ModelRim(phong, shadowed, toLight);
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
            float3 transmittance = LightTransmittance(set, picked[s], ShadowOrigin(origin, behind, faceNormal, toLight), position + toLight * distance,
                float2(Random(seed), Random(seed)), SelfShadowFlags(onModel, faceNormal, toLight));
            sampled += ClampSample(light * LightWarp(lightNormal, toLight) * (transmittance * remainder / (pickedWeight[s] * g_frame.sampledLightRays)));
        }
    }
    // Glowing triangles as area lights (EmissiveLighting::Full): one point of one of them, with the sampled remainder.
    float3 target;
    float3 glow;
    if (lit && SampleEmitter(position, Random(seed), float2(Random(seed), Random(seed)), target, glow)) {
        float3 toEmitter = target - position;
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
