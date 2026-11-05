#ifndef GMODPP_BAKED_HLSLI
#define GMODPP_BAKED_HLSLI

#include "scene.hlsli"

// The baked bounce light of indirect_lighting = 1 (src/bsp.h BakedLighting, src/scene.h BakedBuffers), which
// shaders/baked_bounce.hlsl traces into the map's style-0 lightmap layout and leaf ambient samples, in kBakedParts parts
// one after another: the light that came from the sun, from the sky and from the map's lamps and glowing surfaces. The
// first two follow the sun and sky as they are now (FrameConstants::bakedSunScale and bakedSkyScale), so day and night
// and the strength settings change them; the lamps' stays as baked.
static const uint kBakedParts = 3;

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
    float3 luxelS;
    float unused3;
    float3 luxelT;
    float unused4;
};

// Mirrors bsp::BakeLuxel in src/bsp.h.
struct BakeLuxel
{
    float3 position;
    uint face;
    float3 normal;
    uint luxel;
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
    return faces.Load<FaceLightmap>(face * 96);
}

// The parts of baked light added up as the sun and sky are now, the sun's times `clouds`, the share of it the clouds let
// through there (their shadows are far larger than the reach of bounce light).
float3 DaylitLight(float3 sun, float3 sky, float3 lamps, float clouds)
{
    return sun * (g_frame.bakedSunScale * clouds) + sky * (g_frame.bakedSkyScale * g_frame.bakedSkyTint) + lamps;
}

// The lightmap samples in each part of a baked light buffer.
uint BakedPartSamples(ByteAddressBuffer light)
{
    uint bytes;
    light.GetDimensions(bytes);
    return bytes / (8 * kBakedParts);
}

// ColorRGBExp32: c * 2^exponent / 255 (TexLightToLinear in mathlib/mathlib.h).
float3 DecodeSample(uint packed)
{
    int exponent = asint(packed) >> 24;
    return float3(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF) * (exp2(float(exponent)) / 255.0);
}

float3 LoadLuxelLight(ByteAddressBuffer light, uint sample)
{
    uint2 halves = light.Load2(sample * 8);
    return float3(f16tof32(halves.x), f16tof32(halves.x >> 16), f16tof32(halves.y));
}

float3 LoadLuxelLight(RWByteAddressBuffer light, uint sample)
{
    uint2 halves = light.Load2(sample * 8);
    return float3(f16tof32(halves.x), f16tof32(halves.x >> 16), f16tof32(halves.y));
}

void StoreLuxelLight(RWByteAddressBuffer light, uint sample, float3 value)
{
    light.Store2(sample * 8, uint2(f32tof16(value.x) | (f32tof16(value.y) << 16), f32tof16(value.z)));
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

// A lightmap sample's baked light, its parts added up as the sun and sky are now; `part` samples to a part.
float3 DaylitSample(ByteAddressBuffer light, uint sample, uint part, float clouds)
{
    return DaylitLight(LoadLuxelLight(light, sample), LoadLuxelLight(light, part + sample), LoadLuxelLight(light, 2 * part + sample), clouds);
}

// One luxel's baked light for a normal: the flat lightmap's, or the bump lightmaps' mixed by `weights`.
float3 LuxelLight(ByteAddressBuffer light, uint part, FaceLightmap face, uint luxel, float3 weights, float clouds)
{
    if (face.bumped == 0)
        return DaylitSample(light, face.sample + luxel, part, clouds);
    uint luxels = face.width * face.height;
    return weights.x * DaylitSample(light, face.sample + luxels + luxel, part, clouds) + weights.y * DaylitSample(light, face.sample + 2 * luxels + luxel, part, clouds) +
        weights.z * DaylitSample(light, face.sample + 3 * luxels + luxel, part, clouds);
}

// The baked indirect light at a G-buffer pixel that shows the world at `position`, read bilinearly from its face's luxels
// as the engine reads lightmaps; -1 for other surfaces.
float3 BakedIrradiance(int2 pixel, float3 position, float3 normal)
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
    ByteAddressBuffer light = ResourceDescriptorHeap[g_frame.bakedLightView];
    uint part = BakedPartSamples(light);
    float3 weights = face.bumped != 0 ? BumpWeights(face, normal) : 0.0;
    float clouds = SunOcclusionAt(position, g_frame.traceMask);
    float3 a = LuxelLight(light, part, face, low.y * face.width + low.x, weights, clouds);
    float3 b = LuxelLight(light, part, face, low.y * face.width + high.x, weights, clouds);
    float3 c = LuxelLight(light, part, face, high.y * face.width + low.x, weights, clouds);
    float3 d = LuxelLight(light, part, face, high.y * face.width + high.x, weights, clouds);
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// The leaf ambient cube at a point: its leaf's samples, weighted by inverse squared distance as the engine weights
// them, their parts added up as the sun and sky are now. False where the leaf has no samples.
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
    uint bytes;
    samples.GetDimensions(bytes);
    uint part = bytes / (112 * kBakedParts);
    float clouds = SunOcclusionAt(position, g_frame.traceMask);
    float weights = 0.0;
    for (uint i = 0; i < range.y; ++i) {
        AmbientSample sun = samples.Load<AmbientSample>((range.x + i) * 112);
        AmbientSample sky = samples.Load<AmbientSample>((part + range.x + i) * 112);
        AmbientSample lamps = samples.Load<AmbientSample>((2 * part + range.x + i) * 112);
        float3 offset = sun.position.xyz - position;
        float weight = 1.0 / max(dot(offset, offset), 1.0);
        for (uint s = 0; s < 6; ++s)
            cube[s] += DaylitLight(sun.cube[s].rgb, sky.cube[s].rgb, lamps.cube[s].rgb, clouds) * weight;
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
    float3 squared = normal * normal;
    return squared.x * cube[normal.x < 0.0 ? 1 : 0] + squared.y * cube[normal.y < 0.0 ? 3 : 2] + squared.z * cube[normal.z < 0.0 ? 5 : 4];
}

// A pixel's untraced indirect light with indirect_lighting = 1: the baked bounce light on the world, the leaf ambient light
// elsewhere, or the sky's light. Leaves (`leaf`, `normal` towards the eye) take kLeafTransmission of it from behind.
float3 BakedIndirect(int2 pixel, float3 position, float3 normal, bool leaf)
{
    float3 baked = BakedIrradiance(pixel, position, normal);
    if (baked.x < 0.0)
        baked = leaf ? lerp(LeafAmbient(position, normal), LeafAmbient(position, -normal), kLeafTransmission) : LeafAmbient(position, normal);
    if (baked.x < 0.0)
        baked = leaf ? lerp(SkyLight(normal), SkyLight(-normal), kLeafTransmission) : SkyLight(normal);
    return baked;
}

// A pixel's G-buffer normal and whether it is a leaf (a prop that ignores normals), whose normal faces the eye.
float3 LitNormal(int2 pixel, float3 normal, out bool leaf)
{
    Texture2D<uint> pixelFlags = ResourceDescriptorHeap[g_frame.pixelFlagsView];
    leaf = (pixelFlags.Load(int3(pixel, 0)) & kPixelIgnoreNormals) != 0;
    return leaf && dot(normal, GeometricNormalAt(pixel)) < 0.0 ? -normal : normal;
}

#endif
