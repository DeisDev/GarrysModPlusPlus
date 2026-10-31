#ifndef GMODPP_SCENE_HLSLI
#define GMODPP_SCENE_HLSLI

#include "common.hlsli"

// Ray queries of the Shader Model 6.9 variants (OPACITY_MICROMAPS) let traversal use opacity micromaps, which
// settle alpha-tested candidates in their known opaque and transparent regions without the shader.
#if OPACITY_MICROMAPS
#define SCENE_RAY_QUERY(flags) RayQuery<flags, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>
#else
#define SCENE_RAY_QUERY(flags) RayQuery<flags>
#endif

// Mirrors Scene::InstanceRecord in src/scene.cpp: descriptor indices of raw buffers. A model's alpha-tested and
// translucent triangles follow its opaque ones in a second BLAS geometry; `alpha` holds one Scene::PackAlpha or
// Scene::PackTranslucent entry for each, `materials` a material table index per triangle.
struct InstanceRecord
{
    uint positions;
    uint indices;
    uint albedo;
    uint uvs;
    uint alpha;
    uint opaqueTriangles;
    uint materials;
    // Last frame's vertex positions of a skinned model, or kNoView when they are `positions`.
    uint previousPositions;
    // Last frame's object-to-world transform.
    float4 previousTransform[3];
    // kInstanceIgnoreNormals, kInstanceSelfIllum, kInstanceModel, kInstanceVradSky.
    uint flags;
    // For instances with emitters: each triangle's emitter index (Scene::EmittersView) plus one, 0 for none; kNoView
    // for instances without.
    uint emitterLights;
    uint2 padding;
};

// kInstanceIgnoreNormals, kInstanceSelfIllum, kInstanceModel and kInstanceVradSky in src/scene.h.
static const uint kInstanceIgnoreNormals = 0x01;
static const uint kInstanceSelfIllum = 0x02;
static const uint kInstanceModel = 0x04;
static const uint kInstanceVradSky = 0x08;
static const uint kInstanceEmitter = 0x10;
static const uint kInstanceCharacter = 0x20;

// EmissiveLighting in src/settings.h.
static const uint kEmissiveOff = 0;
static const uint kEmissiveFull = 2;

// Props that ignore normals (kInstanceIgnoreNormals, mostly foliage) are lit as thin two-sided leaves: this share of
// the light reaching one side passes through to the other.
static const float kLeafTransmission = 0.3;
// Light through a leaf scatters under its surface: this share of it scatters forward, in a lobe of this power around
// the light's own direction, and the rest evenly. Leaves glow most where the eye looks through them towards the light.
static const float kLeafForwardScatter = 0.5;
static const float kLeafScatterPower = 4.0;

// Mirrors GpuLight in src/scene.cpp (bsp::Light, VRAD units and falloff).
struct Light
{
    float3 origin;
    uint type;
    float3 intensity;
    // Where an engine dynamic light's light ends; 0 for map lights.
    float radius;
    float3 normal;
    float stopdot;
    float stopdot2;
    float exponent;
    float constantAttn;
    float linearAttn;
    float quadraticAttn;
    // kLightSealed.
    uint flags;
    // The radius of the sphere a map light is lit as: kBulbRadius in src/scene.h, or its lamp shell's.
    float size;
    // Light style (switchable and animated lights), or kNoLightStyle.
    uint style;
    // A map light's inverse-square intensity relative to `intensity` (Scene::GpuLight::referenceScale).
    float referenceScale;
    uint3 padding;
};

// Bytes per Light in the light buffers.
static const uint kLightStride = 96;
// For LightTransmittance: aim at the light's centre.
static const float2 kLightCentre = -1.0;

static const uint kNoLightStyle = 0xFFFFFFFFu;
// kLightSealed in src/scene.h: the light sits inside lamp shells (kLampShellMask), which its shadow rays pass.
static const uint kLightSealed = 0x01;
// engine::kLightStyles; lights of higher styles always shine fully, as in the engine.
static const uint kLightStyles = 128;

static const uint kLightSurface = 0;
static const uint kLightPoint = 1;
static const uint kLightSpot = 2;
// An engine dynamic light: stopdot/stopdot2 are the cosines of its inner/outer cone, exponent 1 marks a spot.
static const uint kLightDynamic = 3;

// Mirrors GpuProjector in src/lighting.cpp: a flashlight or projected texture in Source's DoFlashlight terms.
struct Projector
{
    float4 worldToTexture[4];
    float3 origin;
    float farZ;
    float3 color;
    uint cookieView;
    float3 attenuation;
    // Where the attenuation first reaches full strength, or 0 to keep Source's curve everywhere.
    float fullDistance;
    // kProjectorLocalFlashlight.
    uint flags;
    uint3 padding;
};

static const uint kNoCookie = 0xFFFFFFFFu;
// The local player's own flashlight (Lighting::WriteProjectors), whose holder the shadow mask already leaves out.
static const uint kProjectorLocalFlashlight = 0x01;

struct HitSurface
{
    float3 position;
    float3 normal;
    float3 albedo;
    // InstanceRecord::flags.
    uint flags;
};

InstanceRecord LoadInstance(uint instance)
{
    ByteAddressBuffer records = ResourceDescriptorHeap[g_frame.instancesView];
    return records.Load<InstanceRecord>(instance * 96);
}

float3 UnpackAlbedo(uint packed)
{
    return float3(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF) * (1.0 / 255.0);
}

// Scene::PackTranslucent's flag, and its texture view when the opacity is constant.
static const uint kTranslucentFlag = 0x80000000u;
static const uint kNoAlphaTexture = 0xFFFFFFu;

// Mirrors AlphaTriangle in src/scene.cpp.
struct AlphaTriangle
{
    uint packed;
    float2 uv0;
    float2 uv1;
    float2 uv2;
    // Scene::PackTint: how a translucent triangle colours the light through it (linear RGB8, mode in the top byte).
    uint tint;
};

// Scene::kTintFilter and Scene::kTintRefract.
static const uint kTintFilter = 1;
static const uint kTintRefract = 2;

// The light that gets through where a ray crosses a candidate in a model's non-opaque geometry: 0 or 1 for alpha-tested
// triangles; for translucent ones (`translucent`), with opacity the texture's alpha times $alpha, (1 - opacity) as it
// is and the covered part filtered by the texture's colour; for Refract glass its tint times its tint texture's colour.
// The alpha is filtered as the game's alpha tests filter it; unfiltered, the low-resolution alpha's texels showed as
// square blocks in leaf shadows. Textures are read as sRGB, as the game reads base and tint textures.
float3 CandidateTransmission(uint instance, uint alphaPrimitive, float2 barycentrics, out bool translucent)
{
    ByteAddressBuffer alpha = ResourceDescriptorHeap[NonUniformResourceIndex(LoadInstance(instance).alpha)];
    AlphaTriangle entry = alpha.Load<AlphaTriangle>(alphaPrimitive * 32);
    uint packed = entry.packed;
    translucent = (packed & kTranslucentFlag) != 0;
    float value = ((packed >> 24) & 0x7F) * (1.0 / 127.0);
    float4 texel = 1.0;
    if ((packed & 0xFFFFFF) != kNoAlphaTexture) {
        float2 uv = entry.uv0 + barycentrics.x * (entry.uv1 - entry.uv0) + barycentrics.y * (entry.uv2 - entry.uv0);
        Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(packed & 0xFFFFFF)];
        texel = texture.SampleLevel(g_linearWrap, uv, 0.0);
    }
    if (!translucent)
        return texel.a >= value ? 0.0 : 1.0;
    float3 colour = UnpackAlbedo(entry.tint) * SrgbToLinear(saturate(texel.rgb));
    if ((entry.tint >> 24) == kTintRefract)
        return colour;
    float opacity = saturate(texel.a * value);
    return (1.0 - opacity) + opacity * colour;
}

// Rays other than shadow rays stop at candidates at least this opaque (1 - the luminance of what gets through).
static const float kSolidOpacity = 0.5;

// With `gbufferSurface`, translucent candidates never stop the ray: the game draws them after the G-buffer.
bool CandidateIsSolid(uint instance, uint alphaPrimitive, float2 barycentrics, bool gbufferSurface = false)
{
    bool translucent;
    float3 through = CandidateTransmission(instance, alphaPrimitive, barycentrics, translucent);
    return 1.0 - Luminance(through) >= kSolidOpacity && !(gbufferSurface && translucent);
}

// A committed triangle hit: the instance, the triangle's index in its model, and where on it.
struct HitInfo
{
    uint instance;
    uint primitive;
    float2 barycentrics;
    float t;
    float3x4 objectToWorld;
};

// The closest triangle along a ray, or false when it reaches tMax. `rayFlags` is RAY_FLAG_NONE or
// g_frame.indirectRayFlags; `gbufferSurface` looks for the surface the G-buffer shows (CandidateIsSolid).
bool TraceHit(float3 origin, float3 direction, float tMax, uint mask, out HitInfo hit, uint rayFlags = RAY_FLAG_NONE, bool gbufferSurface = false)
{
    hit = (HitInfo)0;
    SCENE_RAY_QUERY(RAY_FLAG_NONE) query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.01;
    ray.TMax = tMax;
    query.TraceRayInline(g_scene, rayFlags, mask, ray);
    while (query.Proceed()) {
        if (CandidateIsSolid(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), gbufferSurface))
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;
    hit.instance = query.CommittedInstanceID();
    // Geometry 1 is the non-opaque one; with no opaque triangles the non-opaque geometry is 0 and starts at 0.
    hit.primitive = query.CommittedPrimitiveIndex() + (query.CommittedGeometryIndex() != 0 ? LoadInstance(hit.instance).opaqueTriangles : 0);
    hit.barycentrics = query.CommittedTriangleBarycentrics();
    hit.t = query.CommittedRayT();
    hit.objectToWorld = query.CommittedObjectToWorld3x4();
    return true;
}

// Where lighting of a G-buffer position is evaluated. The viewmodel is drawn with its own projection and depth
// range, so next to a wall its surface lies inside or behind world geometry; it is lit from the last point
// the eye can see on the way to it instead.
float3 LitPosition(float3 position)
{
    if (g_frame.viewmodel == 0)
        return position;
    float3 toPosition = position - g_frame.eyePosition;
    float distance = length(toPosition);
    float3 direction = toPosition / max(distance, 1e-4);
    HitInfo hit;
    if (!TraceHit(g_frame.eyePosition, direction, distance, g_frame.traceMask, hit))
        return position;
    return g_frame.eyePosition + direction * max(hit.t - 2.0, 0.0);
}

// How far rays from a G-buffer point `distance` from the eye start off its surface.
float SurfaceBias(float distance) { return g_frame.normalBias * (1.0 + distance * 0.002); }

// The face normal under a G-buffer pixel (shaders/primary.hlsl). Normal maps tilt the G-buffer normal far from it at
// grooves and bevels, and rays that leave along the tilted normal start under the surface.
float3 GeometricNormalAt(int2 pixel)
{
    Texture2D<float4> normals = ResourceDescriptorHeap[g_frame.geometricNormalView];
    return normals.Load(int3(clamp(pixel, int2(0, 0), int2(g_frame.size) - 1), 0)).xyz;
}

// Where rays from a G-buffer point start: off the surface along its face normal and back towards the eye. At a crease
// the normal alone can leave the origin a hair from the other surface, and rays that meet it closer than TMin slip
// through to the sky behind (bright lines along wall edges). The way back to the eye is open, since the eye saw it.
float3 SurfaceRayOrigin(float3 position, float3 geometricNormal, float distance)
{
    return position + (geometricNormal + normalize(g_frame.eyePosition - position) * 0.5) * SurfaceBias(distance);
}

// A direction sampled around a normal-mapped normal, mirrored back above the surface where it points into it.
float3 AboveSurface(float3 direction, float3 geometricNormal)
{
    float below = dot(direction, geometricNormal);
    return below < 0.0 ? direction - 2.0 * below * geometricNormal : direction;
}

// Face normal (facing the ray origin) and albedo of a hit triangle.
HitSurface SurfaceOf(HitInfo hit, float3 origin, float3 direction)
{
    InstanceRecord record = LoadInstance(hit.instance);
    ByteAddressBuffer positions = ResourceDescriptorHeap[NonUniformResourceIndex(record.positions)];
    ByteAddressBuffer indices = ResourceDescriptorHeap[NonUniformResourceIndex(record.indices)];
    ByteAddressBuffer albedo = ResourceDescriptorHeap[NonUniformResourceIndex(record.albedo)];
    uint3 index = indices.Load3(hit.primitive * 12);
    float3 p0 = positions.Load<float3>(index.x * 12);
    float3 p1 = positions.Load<float3>(index.y * 12);
    float3 p2 = positions.Load<float3>(index.z * 12);
    float3 normal = normalize(mul((float3x3)hit.objectToWorld, cross(p1 - p0, p2 - p0)));
    if (dot(normal, direction) > 0.0)
        normal = -normal;

    HitSurface surface;
    surface.position = origin + direction * hit.t;
    surface.normal = normal;
    surface.albedo = UnpackAlbedo(albedo.Load(hit.primitive * 4));
    surface.flags = record.flags;
    return surface;
}

// The closest surface along a ray, and the triangle it is on, or false when it reaches tMax.
bool TraceClosest(float3 origin, float3 direction, float tMax, uint mask, out HitSurface surface, out HitInfo hit, uint rayFlags = RAY_FLAG_NONE)
{
    surface = (HitSurface)0;
    if (!TraceHit(origin, direction, tMax, mask, hit, rayFlags))
        return false;
    surface = SurfaceOf(hit, origin, direction);
    return true;
}

bool TraceClosest(float3 origin, float3 direction, float tMax, uint mask, out HitSurface surface, uint rayFlags = RAY_FLAG_NONE)
{
    HitInfo hit;
    return TraceClosest(origin, direction, tMax, mask, surface, hit, rayFlags);
}

// Mirrors GpuMaterial in src/scene.h.
struct Material
{
    float3 envmapTint;
    uint envmapMask;
    float3 selfIllumTint;
    uint selfIllumMask;
    float envmapContrast;
    float envmapSaturation;
    uint flags;
    float envmapLightScale;
    float envmapLightMin;
    float envmapLightRange;
    float2 padding;
};

static const uint kNoMaterial = 0xFFFFFFFFu;
// Material flags (Scene::kMaterialUnlit): the surface shows its albedo as it is. Scene::kMaterialLightTexture: a face
// whose texture VRAD made emit_surface lights of, so its glow lights nothing again.
static const uint kMaterialUnlit = 1;
static const uint kMaterialLightTexture = 2;

// The material flags of a hit triangle, 0 without a material.
uint MaterialFlags(HitInfo hit)
{
    InstanceRecord record = LoadInstance(hit.instance);
    if (record.materials == kNoView)
        return 0;
    ByteAddressBuffer materials = ResourceDescriptorHeap[NonUniformResourceIndex(record.materials)];
    uint index = materials.Load(hit.primitive * 4);
    if (index == kNoMaterial)
        return 0;
    ByteAddressBuffer table = ResourceDescriptorHeap[g_frame.materialsView];
    return table.Load(index * 64 + 40);
}

// The triangle a G-buffer pixel shows, from shaders/primary.hlsl; false where none matches.
bool VisibleHit(int2 pixel, out HitInfo hit)
{
    hit = (HitInfo)0;
    Texture2D<uint4> visibility = ResourceDescriptorHeap[g_frame.visibilityView];
    uint4 visible = visibility.Load(int3(pixel, 0));
    if (visible.x == kNoView)
        return false;
    hit.instance = visible.x;
    hit.primitive = visible.y;
    hit.barycentrics = asfloat(visible.zw);
    return true;
}
static const uint kMaskOff = 0xFFFFFFFFu;

// A hit triangle's material, or false when its model has none. `uvPerUnit` is how far its texture coordinates
// move per world unit, for choosing mip levels.
bool HitMaterial(HitInfo hit, out Material material, out float2 uv, out float uvPerUnit)
{
    material = (Material)0;
    uv = 0.0;
    uvPerUnit = 0.0;
    InstanceRecord record = LoadInstance(hit.instance);
    if (record.materials == kNoView)
        return false;
    ByteAddressBuffer materials = ResourceDescriptorHeap[NonUniformResourceIndex(record.materials)];
    uint index = materials.Load(hit.primitive * 4);
    if (index == kNoMaterial)
        return false;
    ByteAddressBuffer table = ResourceDescriptorHeap[g_frame.materialsView];
    material = table.Load<Material>(index * 64);
    if (record.uvs != kNoView) {
        ByteAddressBuffer indices = ResourceDescriptorHeap[NonUniformResourceIndex(record.indices)];
        ByteAddressBuffer uvs = ResourceDescriptorHeap[NonUniformResourceIndex(record.uvs)];
        ByteAddressBuffer positions = ResourceDescriptorHeap[NonUniformResourceIndex(record.positions)];
        uint3 vertex = indices.Load3(hit.primitive * 12);
        float2 uv0 = uvs.Load<float2>(vertex.x * 8);
        float2 uv1 = uvs.Load<float2>(vertex.y * 8) - uv0;
        float2 uv2 = uvs.Load<float2>(vertex.z * 8) - uv0;
        uv = uv0 + hit.barycentrics.x * uv1 + hit.barycentrics.y * uv2;
        float3 p0 = positions.Load<float3>(vertex.x * 12);
        float worldArea = length(cross(positions.Load<float3>(vertex.y * 12) - p0, positions.Load<float3>(vertex.z * 12) - p0));
        uvPerUnit = sqrt(abs(uv1.x * uv2.y - uv1.y * uv2.x) / max(worldArea, 1e-6));
    }
    return true;
}

// A Scene::PackMask value at a texture coordinate; `footprint` is how many texture coordinates a pixel spans.
float MaskValue(uint mask, float2 uv, float footprint)
{
    if (mask == kMaskOff)
        return 0.0;
    uint view = mask & 0xFFFFFF;
    if (view == 0xFFFFFF)
        return 1.0;
    Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(view)];
    uint width, height, levels;
    texture.GetDimensions(0, width, height, levels);
    float lod = log2(max(footprint * max(width, height), 1e-6));
    float value = texture.SampleLevel(g_linearWrap, uv, lod)[(mask >> 24) & 3];
    return (mask & (1u << 26)) != 0 ? 1.0 - value : value;
}

// Texture coordinates a hit's mask sample spans per unit of distance: rays that gather light need no fine detail.
static const float kHitMaskFootprint = 0.01;

// The light a hit surface gives off itself: Source's self-illumination, its albedo times the material's tint and mask
// (the G-buffer twins write the same as emissive). `distance` is the ray's length. Only models with such a material
// (kInstanceSelfIllum) look the material up. For `lighting` (light it gives other surfaces), light textures give none:
// VRAD's emit_surface lights already hold their light.
float3 HitEmission(HitSurface surface, HitInfo hit, float distance, bool lighting = false)
{
    if ((surface.flags & kInstanceSelfIllum) == 0)
        return 0.0;
    Material material;
    float2 uv;
    float uvPerUnit;
    if (!HitMaterial(hit, material, uv, uvPerUnit) || material.selfIllumMask == kMaskOff || (lighting && (material.flags & kMaterialLightTexture) != 0))
        return 0.0;
    return material.selfIllumTint * surface.albedo * MaskValue(material.selfIllumMask, uv, uvPerUnit * max(distance, 1.0) * kHitMaskFootprint);
}

// The glow an indirect ray meets at a hit, which lights the surface the ray left (EmissiveLighting): none when off,
// and at the ray's first hit from the G-buffer none from emitters when they are sampled directly (SampleEmitter).
float3 BounceEmission(HitSurface surface, HitInfo hit, bool firstHit)
{
    if (g_frame.emissiveLighting == kEmissiveOff || (firstHit && g_frame.emitterCount != 0 && (surface.flags & kInstanceEmitter) != 0))
        return 0.0;
    return HitEmission(surface, hit, hit.t, true);
}

// Mirrors GpuEmitter in src/scene.h.
struct Emitter
{
    float3 p0;
    uint instance;
    float3 p1;
    uint primitive;
    float3 p2;
    // The share of all emitters' emission times area up to and with this one.
    float cdf;
};

// One entry of a light grid tail (Scene::LightLists): entries are (index, cumulative weight up to 1), and the first whose
// weight passes `u` in [0, 1) is picked. Its index and the probability of the pick; false for an empty tail.
bool SampleTail(uint first, uint count, float u, out uint index, out float probability)
{
    index = 0;
    probability = 0.0;
    if (count == 0)
        return false;
    ByteAddressBuffer tail = ResourceDescriptorHeap[g_frame.gridTailView];
    uint low = 0;
    uint high = count - 1;
    while (low < high) {
        uint middle = (low + high) / 2;
        if (asfloat(tail.Load((first + middle) * 8 + 4)) <= u)
            low = middle + 1;
        else
            high = middle;
    }
    uint2 entry = tail.Load2((first + low) * 8);
    float before = low > 0 ? asfloat(tail.Load((first + low - 1) * 8 + 4)) : 0.0;
    index = entry.x;
    probability = asfloat(entry.y) - before;
    return probability > 0.0;
}

// The light grid cell holding a point (x fastest); points outside the grid take the nearest cell.
uint GridCell(float3 position)
{
    int3 cell = clamp(int3(floor((position - g_frame.gridOrigin) / g_frame.gridCellSize)), 0, int3(g_frame.gridDims) - 1);
    return (cell.z * g_frame.gridDims.y + cell.y) * g_frame.gridDims.x + cell.x;
}

// Bytes per light grid cell: the ranges of its lights (LightsAt), then of its emitters' tail.
static const uint kGridCellStride = 32;

// An emitter's share of all emitters' emission times area.
float EmitterShare(uint index)
{
    ByteAddressBuffer emitters = ResourceDescriptorHeap[g_frame.emittersView];
    return asfloat(emitters.Load(index * 48 + 44)) - (index > 0 ? asfloat(emitters.Load((index - 1) * 48 + 44)) : 0.0);
}

// The probability with which a cell's emitter tail (entries in the order of their indices) picks an emitter, 0 for
// one it leaves out.
float TailProbability(uint first, uint count, uint index)
{
    if (count == 0)
        return 0.0;
    ByteAddressBuffer tail = ResourceDescriptorHeap[g_frame.gridTailView];
    uint low = 0;
    uint high = count - 1;
    while (low < high) {
        uint middle = (low + high) / 2;
        if (tail.Load((first + middle) * 8) < index)
            low = middle + 1;
        else
            high = middle;
    }
    uint2 entry = tail.Load2((first + low) * 8);
    if (entry.x != index)
        return 0.0;
    return asfloat(entry.y) - (low > 0 ? asfloat(tail.Load((first + low - 1) * 8 + 4)) : 0.0);
}

// A point of a glowing triangle as an area light (EmissiveLighting::Full), picked by `u`: the light grid cell holding
// `position` picks among the emitters that light it most in proportion to their emission times area over the square
// of their distance from the cell, and leaves the rest's share of that (the cell's third word) to a pick of any emitter
// by its emission times area. Either pick may find any emitter, so its probability sums both. A point uniformly on the
// triangle is picked by `v`. `glow` is its emission times its cosine towards `position` and its area over pi and the
// triangle's probability: the irradiance it gives, with the receiver's Lambert term and the inverse square of the
// distance to `target` still to apply. False where it faces away or no emitter can light the cell.
bool SampleEmitter(float3 position, float u, float2 v, out float3 target, out float3 glow)
{
    target = 0.0;
    glow = 0.0;
    if (g_frame.emitterCount == 0 || g_frame.gridCellsView == kNoView)
        return false;
    ByteAddressBuffer cells = ResourceDescriptorHeap[g_frame.gridCellsView];
    uint3 range = cells.Load3(GridCell(position) * kGridCellStride + 16);
    float rest = asfloat(range.z);
    uint index;
    float probability;
    if (u < 1.0 - rest) {
        if (!SampleTail(range.x, range.y, u, index, probability))
            return false;
        if (rest > 0.0)
            probability += rest * EmitterShare(index);
    } else {
        ByteAddressBuffer emitters = ResourceDescriptorHeap[g_frame.emittersView];
        float w = (u - (1.0 - rest)) / rest;
        uint low = 0;
        uint high = g_frame.emitterCount - 1;
        while (low < high) {
            uint middle = (low + high) / 2;
            if (asfloat(emitters.Load(middle * 48 + 44)) <= w)
                low = middle + 1;
            else
                high = middle;
        }
        index = low;
        probability = rest * EmitterShare(index) + TailProbability(range.x, range.y, index);
    }
    if (probability <= 0.0)
        return false;
    ByteAddressBuffer emitters = ResourceDescriptorHeap[g_frame.emittersView];
    Emitter emitter = emitters.Load<Emitter>(index * 48);
    float3 normal = cross(emitter.p1 - emitter.p0, emitter.p2 - emitter.p0);
    float area = 0.5 * length(normal);
    if (area <= 0.0)
        return false;
    float root = sqrt(v.x);
    float2 barycentrics = float2(root * (1.0 - v.y), root * v.y);
    target = emitter.p0 + barycentrics.x * (emitter.p1 - emitter.p0) + barycentrics.y * (emitter.p2 - emitter.p0);
    float3 toPosition = position - target;
    float distance = length(toPosition);
    // Source's front faces: cross(p1 - p0, p2 - p0) points out of the back.
    float cosine = dot(-normal / (2.0 * area), toPosition / max(distance, 1e-4));
    if (cosine <= 0.0)
        return false;
    InstanceRecord record = LoadInstance(emitter.instance);
    ByteAddressBuffer albedo = ResourceDescriptorHeap[NonUniformResourceIndex(record.albedo)];
    HitSurface surface = (HitSurface)0;
    surface.albedo = UnpackAlbedo(albedo.Load(emitter.primitive * 4));
    surface.flags = record.flags;
    HitInfo hit = (HitInfo)0;
    hit.instance = emitter.instance;
    hit.primitive = emitter.primitive;
    hit.barycentrics = barycentrics;
    glow = HitEmission(surface, hit, distance, true) * (cosine * area / (3.14159265 * probability));
    return any(glow > 0.0);
}

// kSkyboxMask in src/scene.h.
static const uint kSkyboxMask = 0x10;

// The share of light that gets from `origin` to tMax along a ray, per colour: 0 past anything solid, otherwise the
// product of what each translucent surface crossed lets through.
float3 Transmittance(float3 origin, float3 direction, float tMax, uint mask, uint rayFlags = RAY_FLAG_NONE)
{
    if (mask == 0)
        return 1.0;
    SCENE_RAY_QUERY(RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER) query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.01;
    ray.TMax = tMax;
    query.TraceRayInline(g_scene, rayFlags, mask, ray);
    float3 transmittance = 1.0;
    while (query.Proceed()) {
        bool translucent;
        float3 through = CandidateTransmission(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), translucent);
        if (translucent)
            transmittance *= through;
        if ((!translucent && through.x < 1.0) || all(transmittance < 0.01))
            query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : transmittance;
}

// `mask` is g_frame.sunShadowMask, or g_frame.bounceMask for the light of indirect bounces. As VRAD did
// (TestLine_DoesHitSky in src/utils/vrad/trace.cpp), rays from the map that reach the sky go on through the 3D
// skybox, whose buildings and terrain shadow the map.
float3 SunVisibility(float3 origin, uint mask, uint rayFlags = RAY_FLAG_NONE)
{
    float3 visibility = Transmittance(origin, g_frame.sunDirection, 65536.0, mask, rayFlags);
    if (any(visibility > 0.0) && g_frame.worldToSky > 0.0 && (mask & kSkyboxMask) == 0) {
        float3 camera = float3(g_frame.skyCameraX, g_frame.skyCameraY, g_frame.skyCameraZ);
        visibility *= Transmittance(camera + origin * g_frame.worldToSky, g_frame.sunDirection, 65536.0, kSkyboxMask);
    }
    return visibility;
}

// Sunlight reaching a hit surface, with its shadow ray starting at `origin`, off the side the ray came from. Leaves
// (kInstanceIgnoreNormals) also take the sunlight that comes through from their other side, evenly scattered.
float3 SunIrradiance(HitSurface hit, float3 origin, uint mask)
{
    if (g_frame.sunEnabled == 0.0)
        return 0.0;
    float cosine = dot(hit.normal, g_frame.sunDirection);
    bool leaf = (hit.flags & kInstanceIgnoreNormals) != 0;
    if (leaf && cosine < 0.0)
        return g_frame.sunColor * (kLeafTransmission * -cosine) * SunVisibility(2.0 * hit.position - origin, mask);
    if (cosine <= 0.0)
        return 0.0;
    return g_frame.sunColor * (cosine * (leaf ? 1.0 - kLeafTransmission : 1.0)) * SunVisibility(origin, mask);
}

// Map lights that can reach a position (Scene::LightLists): the (offset, count) range of the light indices its light
// grid cell lists, or for the bake its visibility cluster's, and the range of the cell's tail, the other lights that
// can reach it, sampled by weight (SampleTailLight).
struct LightRanges
{
    uint first;
    uint count;
    uint tailFirst;
    uint tailCount;
};

// Set on a listed light the PVS of its cell's clusters leaves out (kHiddenLight in src/scene.cpp): it may arrive
// through gaps the PVS misses, but it takes no exact shadow ray.
static const uint kHiddenLight = 0x80000000u;

// The visibility cluster holding a point, as found by the engine's point-in-leaf walk, or the cluster count
// when the point is in solid space or the map has no visibility data.
uint ClusterAt(float3 position)
{
    if (g_frame.clusterCount == 0)
        return 0;
    ByteAddressBuffer nodes = ResourceDescriptorHeap[g_frame.bspNodesView];
    int node = 0;
    for (uint steps = 0; node >= 0 && steps < 512; ++steps) {
        float4 plane = asfloat(nodes.Load4(node * 24));
        int2 children = asint(nodes.Load2(node * 24 + 16));
        node = dot(plane.xyz, position) - plane.w >= 0.0 ? children.x : children.y;
    }
    if (node >= 0)
        return g_frame.clusterCount;
    ByteAddressBuffer leafClusters = ResourceDescriptorHeap[g_frame.leafClustersView];
    int cluster = asint(leafClusters.Load((-node - 1) * 4));
    return cluster >= 0 ? uint(cluster) : g_frame.clusterCount;
}

// The lights VRAD let reach a point (its PVSCheck), for the bake: those of its visibility cluster. Surface points lie on
// leaf boundaries, so the lookup is made just off the surface. Without a tail; ClusterLightIndex reads them.
LightRanges ClusterLights(float3 position, float3 normal)
{
    LightRanges ranges = (LightRanges)0;
    if (g_frame.lightCount == 0)
        return ranges;
    ByteAddressBuffer lists = ResourceDescriptorHeap[g_frame.lightRangesView];
    uint2 range = lists.Load2(ClusterAt(position + normal * 2.0) * 8);
    ranges.first = range.x;
    ranges.count = range.y;
    return ranges;
}

uint ClusterLightIndex(LightRanges ranges, uint i)
{
    ByteAddressBuffer indices = ResourceDescriptorHeap[g_frame.lightIndicesView];
    return indices.Load((ranges.first + i) * 4);
}

// The lights of the light grid cell holding a point; points outside the grid take the nearest cell.
LightRanges LightsAt(float3 position)
{
    LightRanges ranges = (LightRanges)0;
    if (g_frame.lightCount == 0 || g_frame.gridCellsView == kNoView)
        return ranges;
    ByteAddressBuffer cells = ResourceDescriptorHeap[g_frame.gridCellsView];
    uint4 range = cells.Load4(GridCell(position) * kGridCellStride);
    ranges.first = range.x;
    ranges.count = range.y;
    ranges.tailFirst = range.z;
    ranges.tailCount = range.w;
    return ranges;
}

uint LightRangeCount(LightRanges ranges) { return ranges.count; }

// The i-th listed light of LightsAt's ranges, with kHiddenLight.
uint LightRangeEntry(LightRanges ranges, uint i)
{
    ByteAddressBuffer indices = ResourceDescriptorHeap[g_frame.gridIndicesView];
    return indices.Load((ranges.first + i) * 4);
}

uint LightRangeIndex(LightRanges ranges, uint i) { return LightRangeEntry(ranges, i) & ~kHiddenLight; }

// One light of a cell's tail by its weight there, from `u` in [0, 1): its index and the probability of the pick; false
// when the cell has no tail.
bool SampleTailLight(LightRanges ranges, float u, out uint light, out float probability)
{
    return SampleTail(ranges.tailFirst, ranges.tailCount, u, light, probability);
}

// Irradiance below which a light cannot change the encoded result (1/1024 after the tonemap scale), so its
// shadow ray is not traced. VRAD's falloff never reaches zero, and maps have hundreds of lights.
// The irradiance volume whose red, green and blue coefficients are `views` at a point. Points outside its window get -1, or with `nearest` the light of the window's nearest probes. The probes are read a cell out along the normal,
// so all eight blended lie in front of the surface: probes just behind a wall see open space there and leaked its light
// through the wall.
float3 VolumeIrradiance(uint3 views, float3 position, float3 normal, bool nearest = false)
{
    position += normal * g_frame.probeSpacing;
    static const float3 kGrid = float3(32.0, 32.0, 16.0);
    float3 window = (position / g_frame.probeSpacing - 0.5) - float3(g_frame.probeOrigin);
    if (any(window < 0.0) || any(window > kGrid - 1.0)) {
        if (!nearest)
            return -1.0;
        position = (clamp(window, 0.0, kGrid - 1.0) + 0.5 + float3(g_frame.probeOrigin)) * g_frame.probeSpacing;
    }
    float3 uvw = position / (g_frame.probeSpacing * kGrid);
    Texture3D<float4> red = ResourceDescriptorHeap[views.r];
    Texture3D<float4> green = ResourceDescriptorHeap[views.g];
    Texture3D<float4> blue = ResourceDescriptorHeap[views.b];
    float4 r = red.SampleLevel(g_linearWrap, uvw, 0.0);
    float4 g = green.SampleLevel(g_linearWrap, uvw, 0.0);
    float4 b = blue.SampleLevel(g_linearWrap, uvw, 0.0);
    return max(float3(r.x + dot(r.yzw, normal), g.x + dot(g.yzw, normal), b.x + dot(b.yzw, normal)), 0.0);
}

// The volume named by g_frame's probe views, or -1 without one: the main view's filled volume, or its indirect light
// alone for forward twins (shaders/forward_twin.hlsli).
float3 VolumeIrradiance(float3 position, float3 normal, bool nearest = false)
{
    if (g_frame.probeRedView == kNoView)
        return -1.0;
    return VolumeIrradiance(uint3(g_frame.probeRedView, g_frame.probeGreenView, g_frame.probeBlueView), position, normal, nearest);
}

// The map's fog lit at a point, before the tonemap scale (shaders/volumetrics.hlsl). In open sunlit air it is the fog's
// colour as the game blends it; shadowed air loses the sun's share and air that less indirect light reaches than the
// open sky's (`volume`, an indirect light volume, or kNoView to take the open sky's) loses some of the sky's. The shares
// are of the irradiance averaged over all directions in open air: a quarter of the sun's, half the sky's.
float3 LitFog(float3 position, uint3 volume)
{
    float sunLight = g_frame.sunEnabled != 0.0 ? 0.25 * Luminance(g_frame.sunColor) : 0.0;
    float skyLight = 0.5 * Luminance(g_frame.skyColor);
    float sunShare = sunLight / max(sunLight + skyLight, 1e-6);
    float3 sun = sunShare > 0.0 ? SunVisibility(position, g_frame.sunShadowMask) : 0.0;
    float ambient = 1.0;
    if (volume.r != kNoView) {
        float3 indirect = VolumeIrradiance(volume, position, 0.0);
        if (indirect.r >= 0.0)
            ambient = saturate(Luminance(indirect) / max(skyLight, 1e-6));
    }
    return g_frame.fogColor / max(g_frame.tonemapScale, 1e-3) * (sunShare * sun + (1.0 - sunShare) * ambient);
}

float VisibleIrradiance() { return (1.0 / 1024.0) / max(g_frame.tonemapScale, 1e-3); }

// Dynamic and projected lights are often attached to a player or weapon (flashlights, muzzle flashes) whose
// model surrounds the light, so occluders right next to them are ignored.
static const float kAttachedLightClearance = 48.0;

// kLocalPlayerMask and kLampShellMask in src/scene.h.
static const uint kLocalPlayerMask = 0x08;
static const uint kLampShellMask = 0x80;

// Other lights' shadow rays stop this far short of them.
static const float kLightClearance = 1.0;

float ShadowRayLength(float clearance, float distance) { return distance - min(clearance, distance * 0.5); }

// Shadow rays of local lights. Dynamic and projected lights are mostly the local player's own (flashlight, muzzle
// flashes), which the game never shadows with the player's body. Sealed map lights shine through lamp shells.
uint ShadowMask(bool attachedLight, bool sealed = false)
{
    uint mask = sealed ? g_frame.lightShadowMask & ~kLampShellMask : g_frame.lightShadowMask;
    return attachedLight ? mask & ~kLocalPlayerMask : mask;
}

// Set by a pass for a pixel of a prop that ignores normals, whose normal it turns towards the eye: light reaches it from
// both sides (LeafTerm). g_toEye is the direction from the pixel to the eye.
static bool g_twoSided = false;
static float3 g_toEye = 0.0;

// Set by a pass for the pixel it lights: Source's half-Lambert diffuse term ($halflambert, DiffuseTerm in
// common_vertexlitgeneric_dx9.h).
static bool g_halfLambert = false;

// Set by a pass that lights the air (shaders/volumetrics.hlsl): light reaches a point from every side.
static bool g_inAir = false;

// Set by a pass for the pixel it lights: the light warp texture ($lightwarptexture), or kNoView.
static uint g_lightWarpView = kNoView;

// A thin leaf's diffuse term for light arriving at `cosine` to its eye-facing normal: diffuse reflection of the light
// on the eye's side, and of the light behind it the share that comes through, partly scattered forward towards the eye.
// The forward lobe, (power + 2) / 2 * pow(cos, power), carries as much light as even scattering.
float LeafTerm(float cosine, float3 toLight)
{
    if (cosine >= 0.0)
        return (1.0 - kLeafTransmission) * cosine;
    float forward = (kLeafScatterPower + 2.0) * 0.5 * pow(saturate(dot(g_toEye, -toLight)), kLeafScatterPower);
    return kLeafTransmission * -cosine * lerp(1.0, forward, kLeafForwardScatter);
}

float Lambert(float3 normal, float3 toLight)
{
    if (g_inAir)
        return 1.0;
    if (g_twoSided)
        return LeafTerm(dot(normal, toLight), toLight);
    if (g_halfLambert) {
        float wrapped = saturate(dot(normal, toLight) * 0.5 + 0.5);
        return wrapped * wrapped;
    }
    return saturate(dot(normal, toLight));
}

// What the light warp makes of a light's diffuse term, relative to Lambert's: Source looks the (half-)Lambert term
// up in the warp texture, doubled, without squaring the half-Lambert term (DiffuseTerm in
// common_vertexlitgeneric_dx9.h). Highlights keep the plain term.
float3 LightWarp(float3 normal, float3 toLight)
{
    if (g_lightWarpView == kNoView || g_twoSided)
        return 1.0;
    float cosine = dot(normal, toLight);
    float term = g_halfLambert ? saturate(cosine * 0.5 + 0.5) : saturate(cosine);
    Texture2D<float4> warp = ResourceDescriptorHeap[NonUniformResourceIndex(g_lightWarpView)];
    float3 warped = 2.0 * warp.SampleLevel(g_linearClamp, float2(term, 0.5), 0.0).rgb;
    return warped / max(Lambert(normal, toLight), 1e-6);
}

// Map lights first, then the engine's dynamic lights.
Light LoadLight(uint index)
{
    if (index < g_frame.lightCount) {
        ByteAddressBuffer lights = ResourceDescriptorHeap[g_frame.lightsView];
        Light light = lights.Load<Light>(index * kLightStride);
        if (g_frame.lightStylesView != kNoView && light.style < kLightStyles) {
            ByteAddressBuffer styles = ResourceDescriptorHeap[g_frame.lightStylesView];
            light.intensity *= asfloat(styles.Load(light.style * 4));
        }
        return light;
    }
    ByteAddressBuffer dynamicLights = ResourceDescriptorHeap[g_frame.dynamicLightsView];
    return dynamicLights.Load<Light>((index - g_frame.lightCount) * kLightStride);
}

// A map light's flags (Light::flags) and size (asuint), without loading the rest of it.
uint2 MapLightShape(uint index)
{
    ByteAddressBuffer lights = ResourceDescriptorHeap[g_frame.lightsView];
    return lights.Load2(index * kLightStride + 68);
}

// A point of the disc a sphere of `radius` at `centre` shows `origin`, by two numbers in [0, 1).
float3 DiscPoint(float3 centre, float radius, float3 origin, float2 jitter)
{
    float3 t, b;
    BuildBasis(normalize(centre - origin), t, b);
    float r = radius * sqrt(jitter.x);
    float phi = 6.28318530718 * jitter.y;
    return centre + (t * cos(phi) + b * sin(phi)) * r;
}

// Half the width, in world units, over which spot light cones fade at least: about one luxel of a finely lit floor.
static const float kSpotEdgeBlur = 8.0;

// A spot light's share of its light towards `-toLight`, 1 for other lights: VRAD's cone, the cosine to the spot's
// axis times the fringe between its inner and outer cones (GatherSampleStandardLightSSE).
float SpotFactor(Light light, float3 toLight, float distance)
{
    if (light.type != kLightSpot)
        return 1.0;
    float spot = -dot(toLight, light.normal);
    // Lightmaps blur the cone's edge over their luxels; without that, cones whose inner and outer angles (nearly)
    // match end in a hard line. The fringe spans at least kSpotEdgeBlur units on either side.
    float edge = 0.5 * (light.stopdot + light.stopdot2);
    float spread = sqrt(saturate(1.0 - edge * edge)) * kSpotEdgeBlur / distance;
    float outer = max(min(light.stopdot2, edge - spread), -1.0);
    float inner = min(max(light.stopdot, edge + spread), 1.0);
    if (spot <= outer)
        return 0.0;
    if (spot > inner)
        return spot;
    float fringe = saturate((spot - outer) / max(inner - outer, 1e-4));
    if (light.exponent != 0.0 && light.exponent != 1.0)
        fringe = pow(fringe, light.exponent);
    return spot * fringe;
}

// Unshadowed irradiance from a light. Map point and spot lights are spheres of their size (Light::size) whose light
// falls off with the inverse square of the distance, matched to the mapper's attenuation at the light's reference
// distance (Light::referenceScale); spot lights keep their cones. Surface lights (VRAD's emit_surface patches) fall off
// with the inverse square and their own cosine. `toLight` and `distance` describe the shadow ray to the centre.
float3 LightIrradiance(Light light, float3 position, float3 normal, out float3 toLight, out float distance)
{
    float3 delta = light.origin - position;
    float distanceSquared = max(dot(delta, delta), 1e-4);
    distance = sqrt(distanceSquared);
    toLight = delta / distance;
    float lambert = Lambert(normal, toLight);
    if (lambert <= 0.0)
        return 0.0;
    float falloff;
    if (light.type == kLightDynamic) {
        float r = distance / light.radius;
        falloff = saturate(1.0 - r * r);
        falloff *= falloff;
        if (light.exponent != 0.0)
            falloff *= smoothstep(light.stopdot2, light.stopdot, -dot(toLight, light.normal));
        return light.intensity * (lambert * falloff) * g_frame.lightScale;
    }
    if (light.type == kLightSurface)
        falloff = saturate(-dot(toLight, light.normal)) / distanceSquared;
    else
        falloff = light.referenceScale / max(distanceSquared, light.size * light.size) * SpotFactor(light, toLight, distance);
    return light.intensity * (lambert * falloff) * g_frame.lightScale;
}

Projector LoadProjector(uint index)
{
    ByteAddressBuffer projectors = ResourceDescriptorHeap[g_frame.projectorsView];
    return projectors.Load<Projector>(index * 128);
}

float ProjectorClearance(Projector projector)
{
    return (projector.flags & kProjectorLocalFlashlight) != 0 ? kLightClearance : kAttachedLightClearance;
}

// The share of a projected light that reaches `origin`, on the straight path from there to the projector.
float3 ProjectorTransmittance(Projector projector, float3 origin)
{
    float3 path = projector.origin - origin;
    float distance = max(length(path), 1e-3);
    return Transmittance(origin, path / distance, ShadowRayLength(ProjectorClearance(projector), distance), ShadowMask(true));
}

// Below this distance from its lamp the local flashlight brightens more slowly than the inverse square: a
// reflector lamp's beam only forms beyond a few reflector widths.
static const float kFlashlightNearField = 16.0;

// Unshadowed irradiance from a projected light, following DoFlashlight in common_flashlight_fxc.h; without a
// cookie texture the frustum gets a round spot. Source's falloff (1/d for the flashlight) is replaced by the
// physical inverse square beyond the distance where it reaches full strength; closer, it stays capped as in Source,
// except for the local flashlight, whose lamp keeps brightening (kFlashlightNearField).
float3 ProjectorIrradiance(Projector projector, float3 position, float3 normal, out float3 toLight, out float distance)
{
    float4 world = float4(position, 1.0);
    float4 projected = float4(dot(projector.worldToTexture[0], world), dot(projector.worldToTexture[1], world),
        dot(projector.worldToTexture[2], world), dot(projector.worldToTexture[3], world));
    float3 delta = projector.origin - position;
    distance = max(length(delta), 1e-3);
    toLight = delta / distance;
    if (projected.w <= 0.0)
        return 0.0;
    // The depth range is left out: the local flashlight's apex was moved forward, past where its near plane began.
    float2 coords = projected.xy / projected.w;
    if (any(coords < 0.0) || any(coords > 1.0))
        return 0.0;
    float lambert = Lambert(normal, toLight);
    if (lambert <= 0.0)
        return 0.0;
    float3 cookie;
    if (projector.cookieView != kNoCookie) {
        Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(projector.cookieView)];
        cookie = texture.SampleLevel(g_linearClamp, coords, 0.0).rgb;
    } else {
        cookie = smoothstep(0.5, 0.35, length(coords - 0.5));
    }
    float attenuation = saturate(dot(projector.attenuation, float3(1.0, 1.0 / distance, 1.0 / (distance * distance))));
    float fullSquared = projector.fullDistance * projector.fullDistance;
    if ((projector.flags & kProjectorLocalFlashlight) != 0 && projector.fullDistance > 0.0)
        attenuation = (fullSquared + kFlashlightNearField * kFlashlightNearField) / (distance * distance + kFlashlightNearField * kFlashlightNearField);
    else if (projector.fullDistance > 0.0 && distance > projector.fullDistance)
        attenuation = fullSquared / (distance * distance);
    float endFalloff = saturate((distance - projector.farZ) / (-0.4 * projector.farZ));
    return cookie * projector.color * (attenuation * lambert * endFalloff);
}

// The lights that can reach a point, in one index space: its grid cell's listed map lights, then the engine's dynamic
// lights, then the projected lights; from `count` on, any map light by its index, as the cell's tail samples them.
struct LightSet
{
    LightRanges ranges;
    uint mapLights;
    uint pointLights;
    uint count;
};

LightSet LightSetAt(float3 position)
{
    LightSet set;
    set.ranges = LightsAt(position);
    set.mapLights = LightRangeCount(set.ranges);
    set.pointLights = set.mapLights + g_frame.dynamicLightCount;
    set.count = set.pointLights + g_frame.projectorCount;
    return set;
}

// Dynamic and projected lights are the ones attached to players and weapons.
bool IsAttachedLight(LightSet set, uint i) { return i >= set.mapLights && i < set.count; }

// The index in the map's lights of a LightSet's i-th light that is a map light.
uint SetMapLight(LightSet set, uint i) { return i >= set.count ? i - set.count : LightRangeIndex(set.ranges, i); }

// Unshadowed irradiance from the i-th light of a LightSet.
float3 SetLightIrradiance(LightSet set, uint i, float3 position, float3 normal, out float3 toLight, out float distance)
{
    if (i >= set.pointLights && i < set.count)
        return ProjectorIrradiance(LoadProjector(i - set.pointLights), position, normal, toLight, distance);
    uint index = IsAttachedLight(set, i) ? g_frame.lightCount + i - set.mapLights : SetMapLight(set, i);
    return LightIrradiance(LoadLight(index), position, normal, toLight, distance);
}

// The share of the i-th light of a LightSet, centred at `lightPosition`, that reaches `origin`. The ray aims at the light
// from the origin itself: origins sit off the surface by a bias that grows with the distance to the eye, and rays
// parallel to the surface's own path ended beside the light, inside or outside its lamp as the camera moved. Map lights
// are spheres (Light::size): `jitter`, two numbers in [0, 1), picks the point of the disc they show the origin that the
// ray aims at, for soft shadows; kLightCentre aims at the centre.
float3 LightTransmittance(LightSet set, uint i, float3 origin, float3 lightPosition, float2 jitter, uint rayFlags = RAY_FLAG_NONE)
{
    bool attached = IsAttachedLight(set, i);
    float clearance = !attached ? kLightClearance : i >= set.pointLights ? ProjectorClearance(LoadProjector(i - set.pointLights)) : kAttachedLightClearance;
    bool sealed = false;
    if (!attached) {
        uint2 shape = MapLightShape(SetMapLight(set, i));
        sealed = (shape.x & kLightSealed) != 0;
        if (jitter.x >= 0.0)
            lightPosition = DiscPoint(lightPosition, asfloat(shape.y), origin, jitter);
    }
    float3 path = lightPosition - origin;
    float distance = max(length(path), 1e-3);
    return Transmittance(origin, path / distance, ShadowRayLength(clearance, distance), ShadowMask(attached, sealed), rayFlags);
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

static const uint kLightCandidates = 8;

// One light of a LightSet by resampled importance sampling: up to kLightCandidates lights drawn uniformly (every
// light when there are no more) and one from the cell's tail, one of them, `picked`, kept in proportion to its
// unshadowed luminance over the probability of its draw. `irradiance` is its unshadowed light divided by the
// probability of the pick; false when no candidate can light the point.
bool SampleLight(LightSet set, float3 position, float3 normal, inout uint seed, out float3 irradiance, out float3 toLight, out float distance,
    out uint picked)
{
    irradiance = 0.0;
    toLight = 0.0;
    distance = 0.0;
    picked = 0;
    bool exhaustive = set.count <= kLightCandidates;
    uint draws = exhaustive ? set.count : kLightCandidates;
    // Uniform draws weigh each candidate by count / draws.
    float drawWeight = exhaustive ? 1.0 : float(set.count) / float(max(draws, 1u));
    float total = 0.0;
    float pickedLuminance = 0.0;
    float3 candidateToLight;
    float candidateDistance;
    for (uint d = 0; d < draws; ++d) {
        uint i = exhaustive ? d : min(uint(Random(seed) * set.count), set.count - 1);
        float luminance = Luminance(SetLightIrradiance(set, i, position, normal, candidateToLight, candidateDistance));
        if (luminance <= 0.0)
            continue;
        total += luminance * drawWeight;
        if (Random(seed) * total < luminance * drawWeight) {
            picked = i;
            pickedLuminance = luminance;
        }
    }
    uint tailLight;
    float probability;
    if (SampleTailLight(set.ranges, Random(seed), tailLight, probability)) {
        float luminance = Luminance(SetLightIrradiance(set, set.count + tailLight, position, normal, candidateToLight, candidateDistance));
        if (luminance > 0.0) {
            total += luminance / probability;
            if (Random(seed) * total < luminance / probability) {
                picked = set.count + tailLight;
                pickedLuminance = luminance;
            }
        }
    }
    if (total <= 0.0)
        return false;
    irradiance = SetLightIrradiance(set, picked, position, normal, toLight, distance) * (total / pickedLuminance);
    return true;
}

// Direct light at a hit off screen: the sun, with its shadow ray under `sunMask`, and one local light picked by
// SampleLight, both from `origin` off the hit's surface.
float3 HitDirectIrradiance(HitSurface hit, float3 origin, uint sunMask, inout uint seed)
{
    float3 irradiance = SunIrradiance(hit, origin, sunMask);
    float3 light;
    float3 toLight;
    float distance;
    uint picked;
    LightSet set = LightSetAt(hit.position);
    if (SampleLight(set, hit.position, hit.normal, seed, light, toLight, distance, picked))
        irradiance += light * LightTransmittance(set, picked, origin, hit.position + toLight * distance, float2(Random(seed), Random(seed)));
    return irradiance;
}

#endif
