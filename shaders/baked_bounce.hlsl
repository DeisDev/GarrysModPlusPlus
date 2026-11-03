#include "baked.hlsli"

// The bounce light of indirect_lighting = 1, baked into the map's lightmap layout and leaf ambient samples with the
// light model of the traced indirect light (gi_trace.hlsl): cosine-weighted rays that see the sky or a hit lit by the
// sun, one sampled light and its glow, with further bounces. A thread per luxel or sample, run in parts.
// g_frame holds the map's lights and sun at their baked strength: style 0 alone, the others off. Every output holds
// kBakedParts parts one after another (shaders/baked.hlsli): the light from the sun, from the sky and from the lamps.
// Step 0 (trace): the irradiance at each luxel, for its flat normal or, when bumped, its three bump basis normals
// (the flat plane then takes their mean). input0: BakeLuxels; input2: FaceLightmaps; input3: first luxel; input4:
// luxel count; output0: raw bounce light, laid out as the lightmap samples.
// Step 1 (filter): the raw light is low-pass filtered within each face, which hides the bake's noise. input0, input2,
// input3 and input4 as step 0; input1: raw bounce light; output0: filtered bounce light.
// Step 2 (leaf ambient): each ambient sample's six cube sides. The map keeps a sample's position in 8-bit steps across
// its leaf's bounds, which can put it in the wall or floor bounding the leaf; such a sample moves past the nearest back
// face its rays meet. input3: first sample; input4: sample count; output0: ambient samples, changed in place.

static const float kMaxDistance = 16384.0;
// Rays per lightmap plane and per ambient cube side.
static const uint kLuxelRays = 64;
static const uint kCubeRays = 32;
// Rays that look for the walls around an ambient sample.
static const uint kInsideRays = 32;
// Diffuse bounces after the first hit: the radiance cache gives traced light many more, which a few match closely.
static const uint kFurtherBounces = 2;

// Baked light in its parts: from the sun, the sky, and the lamps and glowing surfaces.
struct BakedParts
{
    float3 sun;
    float3 sky;
    float3 lamps;
};

// One ray's irradiance estimate around `normal`, as gi_trace.hlsl's HitRadiance and FurtherBounces light it without
// the radiance cache or last frame's light. `side` keeps rays above the surface the point lies on.
BakedParts RayEstimate(float3 origin, float3 normal, float3 side, inout uint seed)
{
    BakedParts light = (BakedParts)0;
    float3 direction = AboveSurface(CosineSample(normal, Random(seed), Random(seed)), side);
    HitSurface hit;
    HitInfo hitInfo;
    if (!TraceClosest(origin, direction, kMaxDistance, g_frame.indirectMask, hit, hitInfo, g_frame.indirectRayFlags)) {
        light.sky = SkyLight(direction);
        return light;
    }
    light.lamps = BounceEmission(hit, hitInfo, true);
    float3 throughput = 1.0;
    for (uint bounce = 0;; ++bounce) {
        float3 hitOrigin = hit.position + hit.normal;
        light.sun += throughput * hit.albedo * SunIrradiance(hit, hitOrigin, g_frame.bounceMask, false);
        light.lamps += throughput * hit.albedo * HitLampIrradiance(hit, hitOrigin, seed);
        throughput *= hit.albedo;
        if (bounce == kFurtherBounces)
            return light;
        direction = CosineSample(hit.normal, Random(seed), Random(seed));
        HitSurface next;
        if (!TraceClosest(hitOrigin, direction, kMaxDistance, g_frame.bounceMask, next, hitInfo, g_frame.indirectRayFlags)) {
            light.sky += throughput * SkyLight(direction);
            return light;
        }
        light.lamps += throughput * BounceEmission(next, hitInfo, false);
        hit = next;
    }
}

// Each ray's parts are clamped together, as their sum is by ClampSample.
BakedParts Irradiance(float3 origin, float3 normal, float3 side, uint rays, inout uint seed)
{
    BakedParts total = (BakedParts)0;
    for (uint i = 0; i < rays; ++i) {
        BakedParts ray = RayEstimate(origin, normal, side, seed);
        float3 sum = ray.sun + ray.sky + ray.lamps;
        float scale = Luminance(sum) > 0.0 ? Luminance(ClampSample(sum)) / Luminance(sum) : 1.0;
        total.sun += ray.sun * (scale / rays);
        total.sky += ray.sky * (scale / rays);
        total.lamps += ray.lamps * (scale / rays);
    }
    return total;
}

// The lightmap samples in each part of the raw and filtered light.
uint PartSamples(RWByteAddressBuffer light)
{
    uint bytes;
    light.GetDimensions(bytes);
    return bytes / (8 * kBakedParts);
}

void StoreParts(RWByteAddressBuffer light, uint sample, BakedParts parts)
{
    uint part = PartSamples(light);
    StoreLuxelLight(light, sample, parts.sun);
    StoreLuxelLight(light, part + sample, parts.sky);
    StoreLuxelLight(light, 2 * part + sample, parts.lamps);
}

void Trace(uint index)
{
    ByteAddressBuffer luxels = ResourceDescriptorHeap[g_pass.input0];
    RWByteAddressBuffer output = ResourceDescriptorHeap[g_pass.output0];
    BakeLuxel luxel = luxels.Load<BakeLuxel>(index * 32);
    FaceLightmap face = LoadFace(g_pass.input2, luxel.face);
    uint seed = Hash(index * 0x9E3779B9u + 0x2545F491u);
    // A unit off the face, as VRAD lit it (ComputeIlluminationPointAndNormalsSSE).
    float3 origin = luxel.position + face.normal;
    uint luxelCount = face.width * face.height;
    if (face.bumped == 0) {
        StoreParts(output, face.sample + luxel.luxel, Irradiance(origin, luxel.normal, face.normal, kLuxelRays, seed));
        return;
    }
    // The bump basis around the luxel's Phong normal (GetBumpNormals).
    float3 tangentS = normalize(face.tangentS - luxel.normal * dot(face.tangentS, luxel.normal));
    float3 tangentT = cross(luxel.normal, tangentS);
    if (dot(tangentT, face.tangentT) < 0.0)
        tangentT = -tangentT;
    BakedParts mean = (BakedParts)0;
    for (uint b = 0; b < 3; ++b) {
        float3 normal = normalize(kBumpBasis[b].x * tangentS + kBumpBasis[b].y * tangentT + kBumpBasis[b].z * luxel.normal);
        BakedParts light = Irradiance(origin, normal, face.normal, kLuxelRays, seed);
        StoreParts(output, face.sample + (b + 1) * luxelCount + luxel.luxel, light);
        mean.sun += light.sun / 3.0;
        mean.sky += light.sky / 3.0;
        mean.lamps += light.lamps / 3.0;
    }
    StoreParts(output, face.sample + luxel.luxel, mean);
}

void Filter(uint index)
{
    ByteAddressBuffer luxels = ResourceDescriptorHeap[g_pass.input0];
    ByteAddressBuffer raw = ResourceDescriptorHeap[g_pass.input1];
    RWByteAddressBuffer filtered = ResourceDescriptorHeap[g_pass.output0];
    BakeLuxel luxel = luxels.Load<BakeLuxel>(index * 32);
    FaceLightmap face = LoadFace(g_pass.input2, luxel.face);
    int2 at = int2(luxel.luxel % face.width, luxel.luxel / face.width);
    uint count = face.bumped != 0 ? 4 : 1;
    uint luxelCount = face.width * face.height;
    uint part = PartSamples(filtered);
    for (uint n = 0; n < count * kBakedParts; ++n) {
        uint plane = (n / count) * part + face.sample + (n % count) * luxelCount;
        // A 3x3 tent within the face.
        float3 total = 0.0;
        float weights = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                int2 neighbour = at + int2(x, y);
                if (any(neighbour < 0) || neighbour.x >= int(face.width) || neighbour.y >= int(face.height))
                    continue;
                float weight = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
                total += LoadLuxelLight(raw, plane + uint(neighbour.y) * face.width + uint(neighbour.x)) * weight;
                weights += weight;
            }
        }
        StoreLuxelLight(filtered, plane + luxel.luxel, total / weights);
    }
}

static const float3 kBoxDirections[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};

void LeafAmbientSample(uint index)
{
    RWByteAddressBuffer samples = ResourceDescriptorHeap[g_pass.output0];
    uint bytes;
    samples.GetDimensions(bytes);
    uint part = bytes / (112 * kBakedParts);
    AmbientSample sample = samples.Load<AmbientSample>(index * 112);
    uint seed = Hash(index * 0x9E3779B9u + 0x68E31DA4u);
    float3 position = sample.position.xyz;
    uint backFaces = 0;
    float nearest = kMaxDistance;
    float3 escape = 0.0;
    for (uint r = 0; r < kInsideRays; ++r) {
        float3 direction = UniformSphere((r + Random(seed)) / kInsideRays, Random(seed));
        HitInfo hit;
        if (TraceHit(position, direction, kMaxDistance, g_frame.indirectMask, hit) && HitBackFace(hit, direction)) {
            ++backFaces;
            if (hit.t < nearest) {
                nearest = hit.t;
                escape = direction;
            }
        }
    }
    if (backFaces > kInsideRays * kInsideFraction)
        position += escape * (nearest + 1.0);
    AmbientSample sky = sample;
    AmbientSample lamps = sample;
    for (uint s = 0; s < 6; ++s) {
        BakedParts light = Irradiance(position, kBoxDirections[s], kBoxDirections[s], kCubeRays, seed);
        sample.cube[s].rgb = light.sun;
        sky.cube[s].rgb = light.sky;
        lamps.cube[s].rgb = light.lamps;
    }
    samples.Store<AmbientSample>(index * 112, sample);
    samples.Store<AmbientSample>((part + index) * 112, sky);
    samples.Store<AmbientSample>((2 * part + index) * 112, lamps);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pass.input4)
        return;
    uint index = g_pass.input3 + id.x;
    if (g_pass.step == 0)
        Trace(index);
    else if (g_pass.step == 1)
        Filter(index);
    else
        LeafAmbientSample(index);
}
