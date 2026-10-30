#ifndef GMODPP_BAKED_HLSLI
#define GMODPP_BAKED_HLSLI

#include "scene.hlsli"

// VRAD's baked light for indirect_lighting = 1 (src/bsp.h BakedLighting, src/scene.h BakedBuffers): style-0
// lightmaps, whose direct light shaders/baked_residual.hlsl takes out, and leaf ambient samples.

// Mirrors bsp::FaceLightmap in src/bsp.h.
struct FaceLightmap
{
    uint sample;
    uint width;
    uint height;
    uint bumped;
    float3 tangentS;
    float unused0;
    float3 tangentT;
    float unused1;
    float3 normal;
    float unused2;
};

// Mirrors bsp::BakeLuxel in src/bsp.h.
struct BakeLuxel
{
    float3 position;
    uint face;
    float3 normal;
    uint luxel;
    // bsp::BakeLuxel::weights, unorm16.
    uint2 weights;
};

// Mirrors gmodpp::BakedTriangle in src/scene.h.
struct BakedTriangle
{
    uint face;
    float2 luxel0;
    float2 luxel1;
    float2 luxel2;
    uint unused;
};

// Mirrors bsp::AmbientSample in src/bsp.h: +x, -x, +y, -y, +z, -z.
struct AmbientSample
{
    float4 position;
    float4 cube[6];
};

static const uint kNoFace = 0xFFFFFFFFu;
// Scene::RecordFrame puts the world model's instance first.
static const uint kWorldInstance = 0;
// g_localBumpBasis (mathlib/bumpvects.h).
static const float3 kBumpBasis[3] = {
    float3(0.81649661, 0.0, 0.57735026),
    float3(-0.40824822, 0.70710677, 0.57735026),
    float3(-0.40824822, -0.70710677, 0.57735026),
};

FaceLightmap LoadFace(uint view, uint face)
{
    ByteAddressBuffer faces = ResourceDescriptorHeap[view];
    return faces.Load<FaceLightmap>(face * 64);
}

// ColorRGBExp32: c * 2^exponent / 255 (TexLightToLinear in mathlib/mathlib.h).
float3 DecodeSample(uint packed)
{
    int exponent = asint(packed) >> 24;
    return float3(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF) * (exp2(float(exponent)) / 255.0);
}

float3 LoadResidual(ByteAddressBuffer residual, uint sample)
{
    uint2 halves = residual.Load2(sample * 8);
    return float3(f16tof32(halves.x), f16tof32(halves.x >> 16), f16tof32(halves.y));
}

float3 LoadResidual(RWByteAddressBuffer residual, uint sample)
{
    uint2 halves = residual.Load2(sample * 8);
    return float3(f16tof32(halves.x), f16tof32(halves.x >> 16), f16tof32(halves.y));
}

void StoreResidual(RWByteAddressBuffer residual, uint sample, float3 value)
{
    residual.Store2(sample * 8, uint2(f32tof16(value.x) | (f32tof16(value.y) << 16), f32tof16(value.z)));
}

// How Source mixes a bumped face's three lightmaps for a normal (lightmappedgeneric_ps2_3_x.h): the squared
// cosines to the bump basis, normalised.
float3 BumpWeights(FaceLightmap face, float3 normal)
{
    float3 tangentNormal = float3(dot(normal, face.tangentS), dot(normal, face.tangentT), dot(normal, face.normal));
    float3 weights = float3(saturate(dot(tangentNormal, kBumpBasis[0])), saturate(dot(tangentNormal, kBumpBasis[1])), saturate(dot(tangentNormal, kBumpBasis[2])));
    weights *= weights;
    return weights / max(dot(weights, 1.0), 1e-4);
}

// One luxel's residual for a normal: the flat lightmap's, or the bump lightmaps' mixed by `weights`.
float3 LuxelResidual(ByteAddressBuffer residual, FaceLightmap face, uint luxel, float3 weights)
{
    if (face.bumped == 0)
        return LoadResidual(residual, face.sample + luxel);
    uint luxels = face.width * face.height;
    return weights.x * LoadResidual(residual, face.sample + luxels + luxel) + weights.y * LoadResidual(residual, face.sample + 2 * luxels + luxel) +
        weights.z * LoadResidual(residual, face.sample + 3 * luxels + luxel);
}

// The baked indirect light at a G-buffer pixel that shows the world, read bilinearly from its face's residual as
// the engine reads lightmaps; -1 for other surfaces.
float3 BakedIrradiance(int2 pixel, float3 normal)
{
    HitInfo hit;
    if (g_frame.bakedTrianglesView == kNoView || !VisibleHit(pixel, hit) || hit.instance != kWorldInstance)
        return -1.0;
    ByteAddressBuffer triangles = ResourceDescriptorHeap[g_frame.bakedTrianglesView];
    BakedTriangle corners = triangles.Load<BakedTriangle>(hit.primitive * 32);
    if (corners.face == kNoFace)
        return -1.0;
    FaceLightmap face = LoadFace(g_frame.bakedFacesView, corners.face);
    float2 luxel = corners.luxel0 + hit.barycentrics.x * (corners.luxel1 - corners.luxel0) + hit.barycentrics.y * (corners.luxel2 - corners.luxel0);
    float2 last = float2(face.width - 1, face.height - 1);
    float2 coord = clamp(luxel, 0.0, last);
    uint2 low = uint2(min(floor(coord), max(last - 1.0, 0.0)));
    uint2 high = min(low + 1, uint2(last));
    float2 f = saturate(coord - float2(low));
    ByteAddressBuffer residual = ResourceDescriptorHeap[g_frame.bakedResidualView];
    float3 weights = face.bumped != 0 ? BumpWeights(face, normal) : 0.0;
    float3 a = LuxelResidual(residual, face, low.y * face.width + low.x, weights);
    float3 b = LuxelResidual(residual, face, low.y * face.width + high.x, weights);
    float3 c = LuxelResidual(residual, face, high.y * face.width + low.x, weights);
    float3 d = LuxelResidual(residual, face, high.y * face.width + high.x, weights);
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// The BSP leaf holding a point (the engine's point-in-leaf walk), or -1 without visibility data or in solid space.
int LeafAt(float3 position)
{
    if (g_frame.clusterCount == 0)
        return -1;
    ByteAddressBuffer nodes = ResourceDescriptorHeap[g_frame.bspNodesView];
    int node = 0;
    for (uint steps = 0; node >= 0 && steps < 512; ++steps) {
        float4 plane = asfloat(nodes.Load4(node * 24));
        int2 children = asint(nodes.Load2(node * 24 + 16));
        node = dot(plane.xyz, position) - plane.w >= 0.0 ? children.x : children.y;
    }
    return node < 0 ? -node - 1 : -1;
}

// The leaf ambient cube at a point: its leaf's samples, weighted by inverse squared distance as the engine weights
// them. False where the leaf has no samples.
bool LeafAmbientCube(float3 position, out float3 cube[6])
{
    for (uint side = 0; side < 6; ++side)
        cube[side] = 0.0;
    int leaf = LeafAt(position);
    if (leaf < 0 || g_frame.leafAmbientView == kNoView)
        return false;
    ByteAddressBuffer leaves = ResourceDescriptorHeap[g_frame.leafAmbientView];
    uint2 range = leaves.Load2(leaf * 8);
    if (range.y == 0)
        return false;
    ByteAddressBuffer samples = ResourceDescriptorHeap[g_frame.ambientSamplesView];
    float weights = 0.0;
    for (uint i = 0; i < range.y; ++i) {
        AmbientSample sample = samples.Load<AmbientSample>((range.x + i) * 112);
        float3 offset = sample.position.xyz - position;
        float weight = 1.0 / max(dot(offset, offset), 1.0);
        for (uint s = 0; s < 6; ++s)
            cube[s] += sample.cube[s].rgb * weight;
        weights += weight;
    }
    for (uint s = 0; s < 6; ++s)
        cube[s] /= weights;
    return true;
}

// The leaf ambient light at a point for a normal: the squared normal's components weight the cube's faces it points
// to. Props that ignore normals (a zero normal) take the average of all six. -1 where the leaf has no samples.
float3 LeafAmbient(float3 position, float3 normal)
{
    float3 cube[6];
    if (!LeafAmbientCube(position, cube))
        return -1.0;
    if (all(normal == 0.0))
        return (cube[0] + cube[1] + cube[2] + cube[3] + cube[4] + cube[5]) / 6.0;
    float3 squared = normal * normal;
    return squared.x * cube[normal.x < 0.0 ? 1 : 0] + squared.y * cube[normal.y < 0.0 ? 3 : 2] + squared.z * cube[normal.z < 0.0 ? 5 : 4];
}

#endif
