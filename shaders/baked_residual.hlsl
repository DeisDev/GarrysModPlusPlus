#include "baked.hlsli"

// The bounce light and sky ambient of VRAD's style-0 lightmaps: VRAD summed direct, bounce and sky ambient light
// into each luxel (FinalLightFace in src/utils/vrad/lightmap.cpp adds the bounce to style 0 only), so the direct
// light of the style-0 lights and sun, recomputed here with VRAD's falloff (LightIrradiance) and occluders (the world
// and static props that cast shadows), is taken out; ray tracing adds it back exactly. A thread per luxel, run in
// parts over several frames; g_frame holds the map's lights and sun at their baked strength.
// Step 0 (bake): input0: BakeLuxels; input1: lightmap samples; input2: FaceLightmaps; input3: first luxel; input4:
// luxel count; output0: residual; output1: counters (0: luxels clamped at zero).
// Step 1 (filter): the residual is low-pass filtered within each face, which hides where VRAD's shadow edges and
// ours differ. input1: residual; output0: filtered residual; output1: counters (1: luxels changed by more than a
// tenth); the rest as step 0.

// VRAD's occluders: the world and static props, but not those it was told cast no shadows.
static const uint kVradOccluders = 0x01 | 0x02;
// A luxel counts as clamped when the direct light exceeds the baked light by more than this share of it.
static const float kClampTolerance = 0.05;

// The direct light of style-0 lights and the sun at a luxel, for its flat normal (direct[0]) and, when bumped,
// its three bump basis normals (direct[1-3]).
void DirectLight(BakeLuxel luxel, FaceLightmap face, out float3 direct[4])
{
    float3 normals[4];
    normals[0] = luxel.normal;
    for (uint b = 0; b < 3; ++b)
        normals[b + 1] = normalize(kBumpBasis[b].x * face.tangentS + kBumpBasis[b].y * face.tangentT + kBumpBasis[b].z * luxel.normal);
    uint count = face.bumped != 0 ? 4 : 1;
    for (uint n = 0; n < 4; ++n)
        direct[n] = 0.0;
    float3 origin = luxel.position + luxel.normal * 0.5;
    if (g_frame.sunEnabled != 0.0) {
        float visibility = -1.0;
        for (uint n = 0; n < count; ++n) {
            float lambert = saturate(dot(normals[n], g_frame.sunDirection));
            if (lambert <= 0.0)
                continue;
            if (visibility < 0.0)
                visibility = Transmittance(origin, g_frame.sunDirection, 65536.0, kVradOccluders);
            direct[n] += g_frame.sunColor * lambert * visibility;
        }
    }
    LightRanges ranges = LightsAt(luxel.position, luxel.normal);
    for (uint i = 0; i < LightRangeCount(ranges); ++i) {
        Light light = LoadLight(LightRangeIndex(ranges, i));
        // Other styles have their own lightmaps, which hold no bounce light.
        if (light.style != 0)
            continue;
        float visibility = -1.0;
        for (uint n = 0; n < count; ++n) {
            float3 toLight;
            float distance;
            float3 irradiance = LightIrradiance(light, luxel.position, normals[n], toLight, distance);
            if (all(irradiance <= 0.0))
                continue;
            if (visibility < 0.0)
                visibility = Transmittance(origin, toLight, ShadowRayLength(kLightClearance, distance), kVradOccluders);
            direct[n] += irradiance * visibility;
        }
    }
}

void Bake(uint index)
{
    ByteAddressBuffer luxels = ResourceDescriptorHeap[g_pass.input0];
    ByteAddressBuffer samples = ResourceDescriptorHeap[g_pass.input1];
    RWByteAddressBuffer residual = ResourceDescriptorHeap[g_pass.output0];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[g_pass.output1];
    BakeLuxel luxel = luxels.Load<BakeLuxel>(index * 32);
    FaceLightmap face = LoadFace(g_pass.input2, luxel.face);
    float3 direct[4];
    DirectLight(luxel, face, direct);
    uint count = face.bumped != 0 ? 4 : 1;
    uint luxelCount = face.width * face.height;
    bool clamped = false;
    for (uint n = 0; n < count; ++n) {
        uint sample = face.sample + n * luxelCount + luxel.luxel;
        float3 baked = DecodeSample(samples.Load(sample * 4));
        float3 remainder = baked - direct[n];
        clamped = clamped || any(remainder < -kClampTolerance * max(baked, 1e-3));
        StoreResidual(residual, sample, max(remainder, 0.0));
    }
    if (clamped)
        counters.InterlockedAdd(0, 1);
}

void Filter(uint index)
{
    ByteAddressBuffer luxels = ResourceDescriptorHeap[g_pass.input0];
    ByteAddressBuffer residual = ResourceDescriptorHeap[g_pass.input1];
    RWByteAddressBuffer filtered = ResourceDescriptorHeap[g_pass.output0];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[g_pass.output1];
    BakeLuxel luxel = luxels.Load<BakeLuxel>(index * 32);
    FaceLightmap face = LoadFace(g_pass.input2, luxel.face);
    int2 at = int2(luxel.luxel % face.width, luxel.luxel / face.width);
    uint count = face.bumped != 0 ? 4 : 1;
    uint luxelCount = face.width * face.height;
    bool changed = false;
    for (uint n = 0; n < count; ++n) {
        uint plane = face.sample + n * luxelCount;
        float3 centre = LoadResidual(residual, plane + luxel.luxel);
        // A 3x3 tent within the face.
        float3 total = 0.0;
        float weights = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                int2 neighbour = at + int2(x, y);
                if (any(neighbour < 0) || neighbour.x >= int(face.width) || neighbour.y >= int(face.height))
                    continue;
                float weight = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
                total += LoadResidual(residual, plane + uint(neighbour.y) * face.width + uint(neighbour.x)) * weight;
                weights += weight;
            }
        }
        float3 value = total / weights;
        changed = changed || abs(Luminance(value) - Luminance(centre)) > 0.1 * max(Luminance(centre), 1e-3);
        StoreResidual(filtered, plane + luxel.luxel, value);
    }
    if (changed)
        counters.InterlockedAdd(4, 1);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pass.input4)
        return;
    uint index = g_pass.input3 + id.x;
    if (g_pass.step == 0)
        Bake(index);
    else
        Filter(index);
}
