#ifndef GMODPP_SCENE_HLSLI
#define GMODPP_SCENE_HLSLI

#include "common.hlsli"
#include "water_body.hlsli"

// Ray queries of the Shader Model 6.9 variants (OPACITY_MICROMAPS) let traversal use opacity micromaps, which
// settle alpha-tested candidates in their known opaque and transparent regions without the shader. The scene holds
// triangles alone, so every query skips procedural primitives, which compiles their traversal out.
#if OPACITY_MICROMAPS
#define SCENE_RAY_QUERY(flags) RayQuery<(flags) | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>
#else
#define SCENE_RAY_QUERY(flags) RayQuery<(flags) | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES>
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
    // kInstanceIgnoreNormals, kInstanceSelfIllum, kInstanceModel.
    uint flags;
    // For instances with emitters: each triangle's emitter index (Scene::EmittersView) plus one, 0 for none; kNoView
    // for instances without.
    uint emitterLights;
    // Brush entity water's body of water (WaterBodyOfTriangle), kNoView for other instances.
    uint waterBody;
    // Each vertex's blend towards its material's second base texture (ModelGeometry::blends), or kNoView.
    uint blends;
};

// kInstanceIgnoreNormals, kInstanceSelfIllum and kInstanceModel in src/scene.h.
static const uint kInstanceIgnoreNormals = 0x01;
static const uint kInstanceSelfIllum = 0x02;
static const uint kInstanceModel = 0x04;
static const uint kInstanceEmitter = 0x10;
static const uint kInstanceCharacter = 0x20;
// kInstanceWater in src/scene.h: the water surfaces (kWaterMask).
static const uint kInstanceWater = 0x40;

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

// Scene::kTintFilter and Scene::kTintRefract, and Scene::kTintGlass added to either.
static const uint kTintFilter = 1;
static const uint kTintRefract = 2;
static const uint kTintGlass = 4;
// Scene::kTintGlassFace: one face of a thick pane (its other face lies within Scene::kMaxGlassThickness behind it).
static const uint kTintGlassFace = 8;
static const float kMaxGlassThickness = 16.0;

// Glass's index of refraction (soda-lime glass, about 1.52).
static const float kGlassIor = 1.52;

// The share of light glass lets through at `cosine` to its normal. A face of a thick pane (`face`) lets through what it
// does not reflect, 1 - F, the pane's other face taking its own share where the ray crosses it; a thin pane, both of whose
// faces the one triangle stands for, lets through (1 - F) / (1 + F), light reflected back and forth between its faces
// included (absorption is the tint's).
float GlassTransmission(float cosine, bool face = false)
{
    float fresnel = Fresnel(abs(cosine), kGlassIor);
    return face ? 1.0 - fresnel : (1.0 - fresnel) / (1.0 + fresnel);
}

// The object-space face normal of a model's alpha triangle (Source's front: the negated edge cross product), unnormalised.
float3 AlphaTriangleNormal(uint instance, uint alphaPrimitive)
{
    InstanceRecord record = LoadInstance(instance);
    ByteAddressBuffer positions = ResourceDescriptorHeap[NonUniformResourceIndex(record.positions)];
    ByteAddressBuffer indices = ResourceDescriptorHeap[NonUniformResourceIndex(record.indices)];
    uint3 index = indices.Load3((record.opaqueTriangles + alphaPrimitive) * 12);
    float3 p0 = positions.Load<float3>(index.x * 12);
    return -cross(positions.Load<float3>(index.y * 12) - p0, positions.Load<float3>(index.z * 12) - p0);
}

// The cosine between a ray (in the instance's object space) and a translucent candidate's face.
float CandidateCosine(uint instance, uint alphaPrimitive, float3 objectDirection)
{
    return dot(normalize(AlphaTriangleNormal(instance, alphaPrimitive)), normalize(objectDirection));
}

// Scene::PackTint's mode bits of a model's alpha triangle.
uint AlphaTriangleTintMode(uint instance, uint alphaPrimitive)
{
    ByteAddressBuffer alpha = ResourceDescriptorHeap[NonUniformResourceIndex(LoadInstance(instance).alpha)];
    return alpha.Load(alphaPrimitive * 32 + 28) >> 24;
}

// The light that gets through where a ray crosses a candidate in a model's non-opaque geometry: 0 or 1 for alpha-tested
// triangles; for translucent ones (`translucent`), with opacity the texture's alpha times $alpha, (1 - opacity) as it
// is and the covered part filtered by the texture's colour; for Refract glass its tint times its tint texture's colour.
// The alpha is filtered as the game's alpha tests filter it; unfiltered, the low-resolution alpha's texels showed as
// square blocks in leaf shadows. Textures are read as sRGB, as the game reads base and tint textures.
float3 CandidateTransmission(uint instance, uint alphaPrimitive, float2 barycentrics, out bool translucent, float3 objectDirection = 0.0)
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
    uint mode = entry.tint >> 24;
    float glass = (mode & kTintGlass) != 0 && any(objectDirection != 0.0)
        ? GlassTransmission(CandidateCosine(instance, alphaPrimitive, objectDirection), (mode & kTintGlassFace) != 0)
        : 1.0;
    if ((mode & 3u) == kTintRefract)
        return colour * glass;
    float opacity = saturate(texel.a * value);
    return ((1.0 - opacity) + opacity * colour) * glass;
}

// The other face of a thick pane of glass, along a ray inside it from `origin`: the first glass candidate within
// kMaxGlassThickness, its distance and world-space face normal (facing back along the ray). False for a thin pane, or
// when something else comes first.
bool GlassExit(float3 origin, float3 direction, out float distance, out float3 normal)
{
    distance = 0.0;
    normal = 0.0;
    if (g_frame.traceMask == 0)
        return false;
    SCENE_RAY_QUERY(RAY_FLAG_NONE) query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.01;
    ray.TMax = kMaxGlassThickness;
    query.TraceRayInline(g_scene, RAY_FLAG_NONE, g_frame.traceMask & ~kWaterMask, ray);
    while (query.Proceed()) {
        if ((AlphaTriangleTintMode(query.CandidateInstanceID(), query.CandidatePrimitiveIndex()) & kTintGlass) != 0)
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT || query.CommittedGeometryIndex() == 0)
        return false;
    distance = query.CommittedRayT();
    normal = normalize(mul((float3x3)query.CommittedObjectToWorld3x4(), AlphaTriangleNormal(query.CommittedInstanceID(), query.CommittedPrimitiveIndex())));
    if (dot(normal, direction) > 0.0)
        normal = -normal;
    return true;
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

// The local player's body and what it holds (kLocalPlayerMask in src/scene.h); in the viewmodel's own scene, the viewmodel.
static const uint kLocalPlayerMask = 0x08;

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
    // What lies in front of the weapon, never the weapon itself.
    if (!TraceHit(g_frame.eyePosition, direction, distance, g_frame.traceMask & ~kLocalPlayerMask, hit))
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

// Whether the ray met the back of the triangle it hit. Source's triangles are clockwise seen from the front, so the
// cross product of their edges points out of the back.
bool HitBackFace(HitInfo hit, float3 direction)
{
    InstanceRecord record = LoadInstance(hit.instance);
    ByteAddressBuffer positions = ResourceDescriptorHeap[NonUniformResourceIndex(record.positions)];
    ByteAddressBuffer indices = ResourceDescriptorHeap[NonUniformResourceIndex(record.indices)];
    uint3 index = indices.Load3(hit.primitive * 12);
    float3 p0 = positions.Load<float3>(index.x * 12);
    float3 normal = mul((float3x3)hit.objectToWorld, cross(positions.Load<float3>(index.y * 12) - p0, positions.Load<float3>(index.z * 12) - p0));
    return dot(normal, direction) < 0.0;
}

// A point whose rays meet back faces this often is inside geometry.
static const float kInsideFraction = 0.3;

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
    uint baseTexture;
    float reflectivity;
    uint baseTexture2;
    uint blendModulate;
    uint2 padding;
};
static const uint kMaterialStride = 80;

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
    return table.Load(index * kMaterialStride + 40);
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
// move per world unit, for choosing mip levels; `blend` its vertices' blend towards a second base texture there.
bool HitMaterial(HitInfo hit, out Material material, out float2 uv, out float uvPerUnit, out float blend)
{
    material = (Material)0;
    uv = 0.0;
    uvPerUnit = 0.0;
    blend = 0.0;
    InstanceRecord record = LoadInstance(hit.instance);
    if (record.materials == kNoView)
        return false;
    ByteAddressBuffer materials = ResourceDescriptorHeap[NonUniformResourceIndex(record.materials)];
    uint index = materials.Load(hit.primitive * 4);
    if (index == kNoMaterial)
        return false;
    ByteAddressBuffer table = ResourceDescriptorHeap[g_frame.materialsView];
    material = table.Load<Material>(index * kMaterialStride);
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
        if (record.blends != kNoView && material.baseTexture2 != kNoView) {
            ByteAddressBuffer blends = ResourceDescriptorHeap[NonUniformResourceIndex(record.blends)];
            float3 corners = float3(asfloat(blends.Load(vertex.x * 4)), asfloat(blends.Load(vertex.y * 4)), asfloat(blends.Load(vertex.z * 4)));
            blend = dot(corners, float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics));
        }
    }
    return true;
}

// A texture's texel at a texture coordinate, from the mip where a texel spans `footprint` texture coordinates.
float4 SampleFootprint(uint view, float2 uv, float footprint)
{
    Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(view)];
    uint width, height, levels;
    texture.GetDimensions(0, width, height, levels);
    return texture.SampleLevel(g_linearWrap, uv, log2(max(footprint * max(width, height), 1e-6)));
}

// A hit's colour with its base texture's detail, where its material has one: the texture's linear colour at the hit,
// scaled by how the triangle's albedo (its texture's average, with the material's and lighting's tints) compares with
// the texture's average. `coneWidth` is the ray's width there, for the mip. Reflections and refractions show the scene's
// textures; light gathered from the hit (indirect bounces) keeps the triangle's average. WorldVertexTransition blends
// towards $basetexture2 as the game does (lightmappedgeneric_ps2_3_x.h): by the vertices' blend, which
// $blendmodulatetexture narrows to a smoothstep around its green channel, its red channel wide.
float3 TexturedAlbedo(HitInfo hit, float3 albedo, float coneWidth)
{
    Material material;
    float2 uv;
    float uvPerUnit;
    float blend;
    if (!HitMaterial(hit, material, uv, uvPerUnit, blend) || material.baseTexture == kNoView || uvPerUnit <= 0.0 || material.reflectivity <= 0.0)
        return albedo;
    float footprint = coneWidth * uvPerUnit;
    float3 texel = SrgbToLinear(saturate(SampleFootprint(material.baseTexture, uv, footprint).rgb));
    if (material.baseTexture2 != kNoView) {
        if (material.blendModulate != kNoView) {
            float2 modulate = SampleFootprint(material.blendModulate, uv, footprint).rg;
            blend = smoothstep(saturate(modulate.g - modulate.r), saturate(modulate.g + modulate.r), blend);
        }
        texel = lerp(texel, SrgbToLinear(saturate(SampleFootprint(material.baseTexture2, uv, footprint).rgb)), blend);
    }
    return saturate(texel * (Luminance(albedo) / material.reflectivity));
}

// A Scene::PackMask value at a texture coordinate; `footprint` is how many texture coordinates a pixel spans.
float MaskValue(uint mask, float2 uv, float footprint)
{
    if (mask == kMaskOff)
        return 0.0;
    uint view = mask & 0xFFFFFF;
    if (view == 0xFFFFFF)
        return 1.0;
    float value = SampleFootprint(view, uv, footprint)[(mask >> 24) & 3];
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
    float blend;
    if (!HitMaterial(hit, material, uv, uvPerUnit, blend) || material.selfIllumMask == kMaskOff || (lighting && (material.flags & kMaterialLightTexture) != 0))
        return 0.0;
    float3 emission = material.selfIllumTint * surface.albedo * MaskValue(material.selfIllumMask, uv, uvPerUnit * max(distance, 1.0) * kHitMaskFootprint);
    return lighting ? emission * g_frame.emissiveScale : emission;
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
        float3 through = CandidateTransmission(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), translucent,
            query.CandidateObjectRayDirection());
        if (translucent)
            transmittance *= through;
        if ((!translucent && through.x < 1.0) || all(transmittance < 0.01))
            query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : transmittance;
}

// The share of the sun's light the clouds and smoke let through to a point (SunOcclusionAt in shaders/common.hlsli), in
// the 3D skybox's space when `mask` holds kSkyboxMask.
float SunOcclusionAt(float3 origin, uint mask)
{
    return SunOcclusionAt(origin, (mask & kSkyboxMask) != 0);
}

// `mask` is g_frame.sunShadowMask, or g_frame.bounceMask for the light of indirect bounces. As VRAD did
// (TestLine_DoesHitSky in src/utils/vrad/trace.cpp), rays from the map that reach the sky go on through the 3D
// skybox, whose buildings and terrain shadow the map. The clouds take their share.
float3 SunVisibilityAlong(float3 origin, float3 direction, uint mask, uint rayFlags)
{
    float3 visibility = Transmittance(origin, direction, 65536.0, mask, rayFlags) * SunOcclusionAt(origin, mask);
    if (any(visibility > 0.0) && g_frame.worldToSky > 0.0 && (mask & kSkyboxMask) == 0) {
        float3 camera = float3(g_frame.skyCameraX, g_frame.skyCameraY, g_frame.skyCameraZ);
        visibility *= Transmittance(camera + origin * g_frame.worldToSky, direction, 65536.0, kSkyboxMask);
    }
    return visibility;
}

// Rays leave a water surface this far to its side.
static const float kWaterSurfaceOffset = 0.05;

// SunVisibilityAlong for a point in `water` (kNoWater for air). Under water sunlight from `airDirection` (towards a point
// of the sun's disc) bends at the flat surface (RefractedSunDirection), crosses the water, which takes its share, and is
// focused by the ripples (CausticsAt); above the surface it is SunVisibilityAlong's. One call of each ray query: inlined
// more often, their code slowed every pass that lights.
float3 SunVisibilityFrom(float3 origin, uint water, float3 airDirection, uint mask, uint rayFlags = RAY_FLAG_NONE)
{
    float3 share = 1.0;
    float3 start = origin;
    if (water != kNoWater) {
        if (airDirection.z <= 0.0)
            return 0.0;
        WaterBody body = LoadWaterBody(water);
        float3 up = RefractedSunDirection(airDirection);
        float length = max(body.surfaceZ - origin.z, 0.0) / up.z;
        share = Transmittance(origin, up, length, mask & ~kWaterMask, rayFlags);
        if (all(share <= 0.0))
            return 0.0;
        float3 surface = origin + up * length;
        share *= (SunIntoWater(airDirection) * CausticsAt(surface)) * exp(-body.extinction * length);
        start = surface + float3(0.0, 0.0, kWaterSurfaceOffset);
    }
    return share * SunVisibilityAlong(start, airDirection, mask & ~kWaterMask, rayFlags);
}

float3 UnderwaterSunVisibility(float3 origin, uint water, float3 airDirection, uint mask, uint rayFlags)
{
    return SunVisibilityFrom(origin, water, airDirection, mask, rayFlags);
}

// The direction sunlight from `airDirection` arrives from at a point in `water` (kNoWater for air).
float3 SunArrival(uint water, float3 airDirection)
{
    return water == kNoWater ? airDirection : RefractedSunDirection(airDirection);
}

float3 SunVisibility(float3 origin, uint mask, uint rayFlags = RAY_FLAG_NONE)
{
    return SunVisibilityFrom(origin, WaterBodyAt(origin), g_frame.sunDirection, mask, rayFlags);
}

// SunVisibilityFrom for the sunlight on secondary hits (indirect rays, reflections, probes): under water the water's sun
// map (WaterSunAt) stands in for the bent shadow rays where it covers the point. Hits on the lake floor traced both
// legs for every indirect sample, which made the viewmodel's run over water several ms slower. Hits the eye sees through
// a water surface (`traced`) trace them: the sun map's 2-unit cells showed as steps in their shadows.
float3 HitSunVisibility(float3 origin, uint water, uint mask, bool traced)
{
    if (!traced && water != kNoWater && g_frame.sunDirection.z > 0.0) {
        float3 up = RefractedSunDirection(g_frame.sunDirection);
        float length = max(LoadWaterBody(water).surfaceZ - origin.z, 0.0) / up.z;
        float3 light;
        if (WaterSunMapAt(origin, light))
            return light * CausticsAt(origin) * SunIntoWater(g_frame.sunDirection) * exp(-LoadWaterBody(water).extinction * length);
    }
    return SunVisibilityFrom(origin, water, g_frame.sunDirection, mask);
}

// The direction to the point of the sun's disc that two numbers in [0, 1) pick: the first the share of the disc's area
// inside the point's ring, the second its angle.
float3 SunDiscDirectionAt(float2 coordinates)
{
    float cosine = 1.0 - coordinates.x * g_frame.sunConeSpan;
    float sine = sqrt(saturate(1.0 - cosine * cosine));
    float phi = 6.28318530718 * coordinates.y;
    float3 t, b;
    BuildBasis(g_frame.sunDirection, t, b);
    return normalize(t * (sine * cos(phi)) + b * (sine * sin(phi)) + g_frame.sunDirection * cosine);
}

// A direction to a uniformly random point of the sun's disc.
float3 SunDiscDirection(inout uint seed)
{
    float area = Random(seed);
    return SunDiscDirectionAt(float2(area, Random(seed)));
}

// Transmittance that also finds the nearest surface taking any light (`occluder`, tMax when none), as NRD's shadow
// denoiser needs for the penumbra's size: no early end at the first hit.
float3 TransmittanceToNearest(float3 origin, float3 direction, float tMax, uint mask, uint rayFlags, out float occluder)
{
    occluder = tMax;
    if (mask == 0)
        return 1.0;
    SCENE_RAY_QUERY(RAY_FLAG_SKIP_CLOSEST_HIT_SHADER) query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.01;
    ray.TMax = tMax;
    query.TraceRayInline(g_scene, rayFlags, mask, ray);
    float3 transmittance = 1.0;
    while (query.Proceed()) {
        bool translucent;
        float3 through = CandidateTransmission(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), translucent,
            query.CandidateObjectRayDirection());
        if (translucent) {
            transmittance *= through;
            occluder = min(occluder, query.CandidateTriangleRayT());
        } else if (through.x < 1.0) {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        occluder = min(occluder, query.CommittedRayT());
        return 0.0;
    }
    return transmittance;
}

// SunVisibility along `direction` (a point of the sun's disc), with the distance to the nearest occluder; occluders in
// the 3D skybox are as far as their scaled distance.
float3 SunVisibilityToNearest(float3 origin, float3 direction, uint mask, uint rayFlags, out float occluder)
{
    uint water = WaterBodyAt(origin);
    if (water != kNoWater) {
        // Under water the occluder that matters for the penumbra lies along the bent ray; the rest is UnderwaterSunVisibility's.
        float3 up = RefractedSunDirection(direction);
        float length = max(LoadWaterBody(water).surfaceZ - origin.z, 0.0) / up.z;
        float3 under = TransmittanceToNearest(origin, up, length, mask & ~kWaterMask, rayFlags, occluder);
        occluder = occluder < length ? occluder : 65536.0;
        return any(under > 0.0) ? UnderwaterSunVisibility(origin, water, direction, mask, rayFlags) : 0.0;
    }
    float3 visibility = TransmittanceToNearest(origin, direction, 65536.0, mask, rayFlags, occluder) * SunOcclusionAt(origin, mask);
    if (any(visibility > 0.0) && g_frame.worldToSky > 0.0 && (mask & kSkyboxMask) == 0) {
        float3 camera = float3(g_frame.skyCameraX, g_frame.skyCameraY, g_frame.skyCameraZ);
        float skyOccluder;
        visibility *= TransmittanceToNearest(camera + origin * g_frame.worldToSky, direction, 65536.0, kSkyboxMask, RAY_FLAG_NONE, skyOccluder);
        if (skyOccluder < 65536.0)
            occluder = min(occluder, skyOccluder / g_frame.worldToSky);
    }
    return visibility;
}

// Sunlight reaching a hit surface, with its shadow ray starting at `origin`, off the side the ray came from. Leaves
// (kInstanceIgnoreNormals) also take the sunlight that comes through from their other side, evenly scattered. `traced`
// as for HitSunVisibility.
float3 SunIrradiance(HitSurface hit, float3 origin, uint mask, bool traced = false)
{
    if (g_frame.sunEnabled == 0.0)
        return 0.0;
    uint water = WaterBodyAt(origin);
    float cosine = dot(hit.normal, SunArrival(water, g_frame.sunDirection));
    bool leaf = (hit.flags & kInstanceIgnoreNormals) != 0;
    if (leaf && cosine < 0.0)
        return g_frame.sunColor * (kLeafTransmission * -cosine) * SunVisibility(2.0 * hit.position - origin, mask);
    if (cosine <= 0.0)
        return 0.0;
    return g_frame.sunColor * (cosine * (leaf ? 1.0 - kLeafTransmission : 1.0)) * HitSunVisibility(origin, water, mask, traced);
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
    int leaf = LeafAt(position);
    if (leaf < 0)
        return g_frame.clusterCount;
    ByteAddressBuffer leafClusters = ResourceDescriptorHeap[g_frame.leafClustersView];
    int cluster = asint(leafClusters.Load(leaf * 4));
    return cluster >= 0 ? uint(cluster) : g_frame.clusterCount;
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

// The map's fog lit at a point, before the tonemap scale (shaders/volumetrics.hlsl). In open air lit by the map's own sun
// and sky it is the fog's colour as the game blends it. Its sun's share (fogSunShare, of the irradiance averaged over all
// directions in open air: a quarter of the sun's, half the sky's) follows the sunlight that reaches the point now
// (fogSunScale: day and night, the sun's strength and colour; shadows), its sky's share the sky light now (fogSkyScale)
// and how much of the open sky's indirect light reaches the point (`volume`, an indirect light volume, or kNoView to take
// the open sky's).
float3 LitFog(float3 position, uint3 volume)
{
    float3 sun = 0.0;
    if (g_frame.sunEnabled != 0.0 && g_frame.fogSunShare > 0.0)
        sun = g_frame.fogSunScale * SunVisibility(position, g_frame.sunShadowMask);
    float ambient = 1.0;
    if (volume.r != kNoView) {
        float3 indirect = VolumeIrradiance(volume, position, 0.0);
        if (indirect.r >= 0.0)
            ambient = saturate(Luminance(indirect) / max(0.5 * Luminance(g_frame.skyColor), 1e-6));
    }
    return g_frame.fogColor / max(g_frame.tonemapScale, 1e-3) * (g_frame.fogSunShare * sun + (1.0 - g_frame.fogSunShare) * g_frame.fogSkyScale * ambient);
}

// Water scatters half its light evenly and half strongly forward (Henyey-Greenstein, g = 0.9), as particles in water
// do, so sunlight shows as shafts and a glow towards the sun. Times 4 pi: 1 averaged over all directions.
static const float kWaterForwardShare = 0.5;
static const float kWaterPhaseG = 0.9;

float WaterPhase(float cosine)
{
    float g2 = kWaterPhaseG * kWaterPhaseG;
    return (1.0 - kWaterForwardShare) + kWaterForwardShare * (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * kWaterPhaseG * cosine, 1e-4), 1.5);
}

// The light the water of `body` scatters towards a ray along `direction` at a point, before the tonemap scale. In open
// water just under the surface, lit by the map's own sun and sky, it averages the water's fog colour, as LitFog keeps
// the map fog's, and its shares follow the light now as LitFog's do: the sun's the sunlight that reaches the point (the
// surface, the water's sun map: shadows above and in the water, caustics; and the water above) and the phase function,
// the sky's the light that reaches its depth.
float3 LitWater(float3 position, WaterBody body, float3 direction)
{
    float3 sun = 0.0;
    if (g_frame.sunEnabled != 0.0 && g_frame.fogSunShare > 0.0 && g_frame.sunDirection.z > 0.0) {
        float3 up = RefractedSunDirection(g_frame.sunDirection);
        float length = max(body.surfaceZ - position.z, 0.0) / up.z;
        float3 arriving = SunIntoWater(g_frame.sunDirection) * WaterSunAt(position) * exp(-body.extinction * length);
        sun = g_frame.fogSunScale * arriving * WaterPhase(dot(direction, up));
    }
    float3 sky = exp(-body.extinction * max(body.surfaceZ - position.z, 0.0));
    return body.fogColor * (g_frame.fogSunShare * sun + (1.0 - g_frame.fogSunShare) * g_frame.fogSkyScale * sky);
}

// The water's scattering albedo by colour: the share of the light reaching it that it scatters, from the light it shows
// in open water under the map's own sun and sky (LitWater), at most 1.
float3 WaterAlbedo(WaterBody body)
{
    return saturate(body.fogColor / max(g_frame.fogAirLight, 1e-6));
}

// Light along a ray through the water of `water` from `origin` along `direction` for `length` units: `radiance` from its
// end, attenuated by the water, plus the water's own light (LitWater) gathered at `steps` points jittered by `seed`
// over the stretch that still shows (four mean free paths), each standing for the light its part of the ray adds.
float3 ThroughWater(float3 radiance, float3 origin, float3 direction, float length, uint water, uint steps, inout uint seed)
{
    WaterBody body = LoadWaterBody(water);
    float3 far = exp(-body.extinction * length);
    float span = min(length, 4.0 / max(min(body.extinction.r, min(body.extinction.g, body.extinction.b)), 1e-6));
    float3 light = 0.0;
    float3 before = 1.0;
    for (uint s = 0; s < steps; ++s) {
        float3 after = s + 1 == steps ? far : exp(-body.extinction * (span * (s + 1) / steps));
        float t = span * (s + Random(seed)) / steps;
        light += LitWater(origin + direction * t, body, direction) * max(before - after, 0.0);
        before = after;
    }
    return radiance * far + light;
}

// The most water surfaces a ray of TraceThroughWater crosses or is mirrored by.
static const uint kWaterInterfaces = 3;

// The closest surface along a ray that water may bend, for paths that pick one of Fresnel's branches at random (indirect
// light, the radiance cache, the probe volume). Through water, `light` gathers the water's own light (ThroughWater, one
// point a stretch) and `throughput` what the water lets through. At a water surface the ray is mirrored with Fresnel's
// probability, else refracted, whose radiance changes by the square of the ratio of the indices of refraction (into
// water n^2, out of it 1 / n^2). `direction` ends as the last stretch's direction (the sky's, on a miss). False when
// the ray meets nothing; with a zero throughput, nothing more can reach its origin.
bool TraceThroughWater(float3 origin, inout float3 direction, float tMax, uint mask, uint rayFlags, inout uint seed, out HitSurface surface,
    out HitInfo hit, out float3 throughput, out float3 light)
{
    throughput = 1.0;
    light = 0.0;
    surface = (HitSurface)0;
    hit = (HitInfo)0;
    uint water = g_frame.waterBodyCount != 0 ? WaterBodyAt(origin) : kNoWater;
    // One trace a stretch: in water up to its surface, in air up to the first surface including water's.
    // Bodies are loaded field by field where they are needed: a whole WaterBody kept across the trace made every pass
    // that traces these rays slower.
    for (uint crossing = 0; crossing <= kWaterInterfaces; ++crossing) {
        float length = tMax;
        uint stretchMask = mask & ~kWaterMask;
        if (water != kNoWater) {
            if (direction.z > 0.0)
                length = min(length, max(LoadWaterBody(water).surfaceZ - origin.z, 0.0) / direction.z);
        } else if (crossing < kWaterInterfaces && g_frame.waterBodyCount != 0) {
            stretchMask |= kWaterMask;
        }
        bool found = TraceClosest(origin, direction, length, stretchMask, surface, hit, rayFlags);
        if (water != kNoWater) {
            length = found ? hit.t : length;
            light += throughput * ThroughWater(0.0, origin, direction, found || direction.z > 0.0 ? length : 1e9, water, 1, seed);
            throughput *= exp(-LoadWaterBody(water).extinction * length);
            if (found)
                return true;
            if (direction.z <= 0.0 || crossing == kWaterInterfaces) {
                throughput = 0.0;
                return false;
            }
            // At the surface from below: mirrored with Fresnel's probability, else out into the air.
            float3 surfacePoint = origin + direction * length;
            float3 down = -WaterSurfaceNormal(water, LoadWaterBody(water), surfacePoint);
            float fresnel = Fresnel(-dot(direction, down), 1.0 / kWaterIor);
            float3 refracted = refract(direction, down, kWaterIor);
            if (Random(seed) < fresnel || dot(refracted, refracted) <= 0.0 || refracted.z <= 0.0) {
                direction = reflect(direction, down);
                direction.z = min(direction.z, -0.02);
                direction = normalize(direction);
                origin = surfacePoint - float3(0.0, 0.0, kWaterSurfaceOffset);
            } else {
                direction = normalize(refracted);
                throughput *= kWaterIor * kWaterIor;
                origin = surfacePoint + float3(0.0, 0.0, kWaterSurfaceOffset);
                water = kNoWater;
            }
            continue;
        }
        if (!found)
            return false;
        if ((surface.flags & kInstanceWater) == 0)
            return true;
        // A water surface from above: mirrored with Fresnel's probability, else into the water.
        float3 surfacePoint = surface.position;
        uint below = WaterBodyOfTriangle(hit.instance, hit.primitive);
        if (below == kNoWater || direction.z >= 0.0) {
            origin = surfacePoint + direction * kWaterSurfaceOffset;
            continue;
        }
        float3 up = WaterSurfaceNormal(below, LoadWaterBody(below), surfacePoint);
        float fresnel = Fresnel(-dot(direction, up), kWaterIor);
        float3 refracted = refract(direction, up, 1.0 / kWaterIor);
        if (Random(seed) < fresnel || dot(refracted, refracted) <= 0.0 || refracted.z >= 0.0) {
            direction = reflect(direction, up);
            direction.z = max(direction.z, 0.02);
            direction = normalize(direction);
            origin = surfacePoint + float3(0.0, 0.0, kWaterSurfaceOffset);
        } else {
            direction = normalize(refracted);
            throughput /= kWaterIor * kWaterIor;
            origin = surfacePoint - float3(0.0, 0.0, kWaterSurfaceOffset);
            water = below;
        }
    }
    throughput = 0.0;
    return false;
}

float VisibleIrradiance() { return (1.0 / 1024.0) / max(g_frame.tonemapScale, 1e-3); }

// Dynamic and projected lights are often attached to a player or weapon (flashlights, muzzle flashes) whose
// model surrounds the light, so occluders right next to them are ignored.
static const float kAttachedLightClearance = 48.0;
// The muzzle flash sits just past the barrel, so where the viewmodel's own triangles shadow it its shadow rays start closer.
static const float kViewmodelAttachedClearance = 4.0;

// kLocalPlayerMask and kLampShellMask in src/scene.h.
static const uint kLampShellMask = 0x80;

// Other lights' shadow rays stop this far short of them.
static const float kLightClearance = 1.0;

float ShadowRayLength(float clearance, float distance) { return distance - min(clearance, distance * 0.5); }

// Shadow rays of local lights. Dynamic and projected lights are mostly the local player's own (flashlight, muzzle
// flashes), which the game never shadows with the player's body. Sealed map lights shine through lamp shells.
uint ShadowMask(bool attachedLight, bool sealed = false)
{
    uint mask = sealed ? g_frame.lightShadowMask & ~kLampShellMask : g_frame.lightShadowMask;
    // The viewmodel's own triangles do shadow its pixels from the flashlight and muzzle flashes.
    return attachedLight && g_frame.viewmodel == 0 ? mask & ~kLocalPlayerMask : mask;
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
    return light.intensity * g_frame.lightTint * (lambert * falloff * g_frame.lightScale);
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

// The normal of the surface a pass lights, when it has one, for LightPath's Lambert term under water (0 for none).
static float3 g_receiverNormal = 0.0;

// The share of a light's light that reaches `origin` from `lightPosition`, its shadow ray stopping `clearance` short of
// the light, relative to what the straight, unshadowed path would bring. Outside water it is that straight shadow ray.
// From under water, light from a lamp in the same water crosses it on the straight path (Beer-Lambert); light from above
// the surface bends where it crosses it (Snell's law; the point found by bisection along the horizontal line between
// them), and its shadow is traced along both legs of that path. It loses what the surface reflects (Fresnel) and what
// the water takes on its underwater leg, and its beam spreads differently than the straight inverse square: for a point
// source at height h_a above the surface and a point h_w below, the irradiance across the beam is
// I sin(a) / (x dx/da cos(w)) with x = h_a tan(a) + h_w tan(w), sin(a) = n sin(w) (the Jacobian of the bent rays),
// against I / d^2. With g_receiverNormal set, the Lambert term follows the bent direction too. `toLight` is the direction
// the light leaves towards the origin, reversed: along its last leg.
float3 LightPath(float3 origin, float3 lightPosition, float clearance, uint mask, uint rayFlags, out float3 toLight)
{
    float3 path = lightPosition - origin;
    float distance = max(length(path), 1e-3);
    uint water = origin.z < g_frame.waterTop ? WaterBodyAt(origin) : kNoWater;
    WaterBody body = (WaterBody)0;
    if (water != kNoWater)
        body = LoadWaterBody(water);
    // The shadow ray's legs (one, or two through the surface) share one ray query, which keeps the passes that trace
    // them small.
    float3 share = 1.0;
    float3 start = origin;
    float3 direction = path / distance;
    float stretch = ShadowRayLength(clearance, distance);
    float3 nextStart = origin;
    float3 nextDirection = direction;
    float nextLength = 0.0;
    uint legs = 1;
    toLight = direction;
    if (water != kNoWater && lightPosition.z <= body.surfaceZ) {
        share = exp(-body.extinction * distance);
    } else if (water != kNoWater) {
        float underDepth = max(body.surfaceZ - origin.z, 1e-3);
        float aboveHeight = max(lightPosition.z - body.surfaceZ, 1e-3);
        float2 across = lightPosition.xy - origin.xy;
        float reach = length(across);
        float2 heading = reach > 1e-4 ? across / reach : float2(1.0, 0.0);
        float low = 0.0;
        float high = reach;
        for (uint step = 0; step < 20; ++step) {
            float r = 0.5 * (low + high);
            float sineWater = r / sqrt(r * r + underDepth * underDepth);
            float sineAir = (reach - r) / sqrt((reach - r) * (reach - r) + aboveHeight * aboveHeight);
            if (kWaterIor * sineWater > sineAir)
                high = r;
            else
                low = r;
        }
        float3 crossing = float3(origin.xy + heading * (0.5 * (low + high)), body.surfaceZ);
        float3 under = crossing - origin;
        float underLength = max(length(under), 1e-3);
        float3 over = lightPosition - crossing;
        float overLength = max(length(over), 1e-3);
        direction = under / underLength;
        stretch = underLength;
        nextStart = crossing + float3(0.0, 0.0, kWaterSurfaceOffset);
        nextDirection = over / overLength;
        toLight = nextDirection;
        nextLength = ShadowRayLength(clearance, overLength);
        legs = 2;
        float cosineAir = aboveHeight / overLength;
        float cosineWater = underDepth / underLength;
        float sineAir = sqrt(saturate(1.0 - cosineAir * cosineAir));
        float spread;
        if (reach < 1e-2) {
            float apparent = aboveHeight + underDepth / kWaterIor;
            spread = distance * distance / (apparent * apparent);
        } else {
            float dx = aboveHeight / (cosineAir * cosineAir) + underDepth / (cosineWater * cosineWater) * cosineAir / (kWaterIor * cosineWater);
            spread = distance * distance * sineAir / (reach * dx * cosineWater);
        }
        float lambert = 1.0;
        if (any(g_receiverNormal != 0.0)) {
            float straight = dot(g_receiverNormal, path / distance);
            lambert = straight > 1e-3 ? saturate(dot(g_receiverNormal, direction)) / straight : 0.0;
        }
        share = ((1.0 - Fresnel(cosineAir, kWaterIor)) * spread * lambert) * exp(-body.extinction * underLength);
        mask &= ~kWaterMask;
    }
    for (uint leg = 0; leg < legs && any(share > 0.0); ++leg) {
        share *= Transmittance(start, direction, stretch, mask, rayFlags);
        start = nextStart;
        direction = nextDirection;
        stretch = nextLength;
    }
    return share;
}

float3 LightPath(float3 origin, float3 lightPosition, float clearance, uint mask, uint rayFlags)
{
    float3 toLight;
    return LightPath(origin, lightPosition, clearance, mask, rayFlags, toLight);
}

// A map spot light's cone along the path its light takes to `origin` (LightPath's toLight) over its weight along the
// straight line from `lightPosition`, which its irradiance was weighed on: 1 where the path is straight.
float BentSpotShare(uint mapLight, float3 origin, float3 lightPosition, float3 toLight)
{
    float3 straight = lightPosition - origin;
    float distance = max(length(straight), 1e-3);
    straight /= distance;
    if (dot(toLight, straight) >= 0.99999)
        return 1.0;
    Light light = LoadLight(mapLight);
    float weighed = SpotFactor(light, straight, distance);
    return weighed > 0.0 ? SpotFactor(light, toLight, distance) / weighed : 0.0;
}

// A projector's cookie towards a point, 0 outside its frustum.
float3 ProjectorPattern(Projector projector, float3 position)
{
    float4 world = float4(position, 1.0);
    float4 projected = float4(dot(projector.worldToTexture[0], world), dot(projector.worldToTexture[1], world),
        dot(projector.worldToTexture[2], world), dot(projector.worldToTexture[3], world));
    if (projected.w <= 0.0)
        return 0.0;
    // The depth range is left out: the local flashlight's apex was moved forward, past where its near plane began.
    float2 coords = projected.xy / projected.w;
    if (any(coords < 0.0) || any(coords > 1.0))
        return 0.0;
    if (projector.cookieView != kNoCookie) {
        Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(projector.cookieView)];
        return texture.SampleLevel(g_linearClamp, coords, 0.0).rgb;
    }
    return smoothstep(0.5, 0.35, length(coords - 0.5));
}

// A projector's pattern (ProjectorPattern) along the path its light takes to `origin` (LightPath's toLight), bent at a
// water surface, over the pattern along the straight line ProjectorIrradiance weighed it on: 1 where the path is
// straight.
float3 BentPatternShare(Projector projector, float3 origin, float3 toLight)
{
    float3 straight = projector.origin - origin;
    float distance = length(straight);
    if (dot(toLight, straight) >= 0.99999 * distance)
        return 1.0;
    float3 weighed = ProjectorPattern(projector, origin);
    float3 bent = ProjectorPattern(projector, projector.origin - toLight * distance);
    return select(weighed > 0.0, bent / max(weighed, 1e-6), 0.0);
}

// The share of a projected light that reaches `origin` from the projector (LightPath), its pattern weighed along that
// path (BentPatternShare).
float3 ProjectorTransmittance(Projector projector, float3 origin)
{
    float3 toLight;
    float3 share = LightPath(origin, projector.origin, ProjectorClearance(projector), ShadowMask(true), RAY_FLAG_NONE, toLight);
    return all(share <= 0.0) ? share : share * BentPatternShare(projector, origin, toLight);
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
    float3 delta = projector.origin - position;
    distance = max(length(delta), 1e-3);
    toLight = delta / distance;
    float lambert = Lambert(normal, toLight);
    if (lambert <= 0.0)
        return 0.0;
    float3 cookie = ProjectorPattern(projector, position);
    if (all(cookie <= 0.0))
        return 0.0;
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

// The i-th light of a LightSet that is a map or dynamic light (not a projector).
Light SetPointLight(LightSet set, uint i)
{
    return LoadLight(IsAttachedLight(set, i) ? g_frame.lightCount + i - set.mapLights : SetMapLight(set, i));
}

// Unshadowed irradiance from the i-th light of a LightSet.
float3 SetLightIrradiance(LightSet set, uint i, float3 position, float3 normal, out float3 toLight, out float distance)
{
    if (i >= set.pointLights && i < set.count)
        return ProjectorIrradiance(LoadProjector(i - set.pointLights), position, normal, toLight, distance);
    return LightIrradiance(SetPointLight(set, i), position, normal, toLight, distance);
}

// The share of the i-th light of a LightSet, centred at `lightPosition`, that reaches `origin`. The ray aims at the light
// from the origin itself: origins sit off the surface by a bias that grows with the distance to the eye, and rays
// parallel to the surface's own path ended beside the light, inside or outside its lamp as the camera moved. Map lights
// are spheres (Light::size): `jitter`, two numbers in [0, 1), picks the point of the disc they show the origin that the
// ray aims at, for soft shadows; kLightCentre aims at the centre. A spot light's cone and a projector's pattern weigh
// the light along the path it takes (bent at a water surface) in place of the straight one its irradiance was weighed
// along.
float3 LightTransmittance(LightSet set, uint i, float3 origin, float3 lightPosition, float2 jitter, uint rayFlags = RAY_FLAG_NONE)
{
    bool attached = IsAttachedLight(set, i);
    float clearance = !attached ? kLightClearance : i >= set.pointLights ? ProjectorClearance(LoadProjector(i - set.pointLights))
        : g_frame.viewmodel == 2 && (g_frame.lightShadowMask & kLocalPlayerMask) != 0 ? kViewmodelAttachedClearance : kAttachedLightClearance;
    bool sealed = false;
    if (!attached) {
        uint2 shape = MapLightShape(SetMapLight(set, i));
        sealed = (shape.x & kLightSealed) != 0;
        if (jitter.x >= 0.0)
            lightPosition = DiscPoint(lightPosition, asfloat(shape.y), origin, jitter);
    }
    float3 toLight;
    float3 share = LightPath(origin, lightPosition, clearance, ShadowMask(attached, sealed), rayFlags, toLight);
    if (all(share <= 0.0))
        return share;
    if (!attached)
        return share * BentSpotShare(SetMapLight(set, i), origin, lightPosition, toLight);
    return i >= set.pointLights ? share * BentPatternShare(LoadProjector(i - set.pointLights), origin, toLight) : share;
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

// The local lights' direct light at a hit off screen, from `origin` off the hit's surface.
float3 HitLampIrradiance(HitSurface hit, float3 origin, inout uint seed)
{
    float3 irradiance = 0.0;
    LightSet set = LightSetAt(hit.position);
    // Projected lights (the flashlight) are few and bright, and each is traced: picked as one light among a cell's many,
    // their rare, heavily weighted samples were clamped (ClampSample), and the flashlight barely lit the floor under
    // water or what mirrors show. The others are sampled. One shadow ray call serves both.
    uint projectors = set.count - set.pointLights;
    LightSet sampled = set;
    sampled.count = set.pointLights;
    for (uint k = 0; k <= projectors; ++k) {
        float3 light;
        float3 toLight;
        float distance;
        uint index;
        bool projector = k < projectors;
        if (projector) {
            index = set.pointLights + k;
            light = SetLightIrradiance(set, index, hit.position, hit.normal, toLight, distance);
        } else if (!SampleLight(sampled, hit.position, hit.normal, seed, light, toLight, distance, index)) {
            break;
        }
        if (all(light <= 0.0))
            continue;
        LightSet lit = sampled;
        if (projector)
            lit = set;
        g_receiverNormal = hit.normal;
        irradiance += light * LightTransmittance(lit, index, origin, hit.position + toLight * distance,
            projector ? kLightCentre : float2(Random(seed), Random(seed)));
        g_receiverNormal = 0.0;
    }
    return irradiance;
}

// Direct light at a hit off screen: the sun, with its shadow ray under `sunMask`, and the local lights, all from
// `origin` off the hit's surface. `tracedSun` as SunIrradiance's `traced`.
float3 HitDirectIrradiance(HitSurface hit, float3 origin, uint sunMask, inout uint seed, bool tracedSun = false)
{
    return SunIrradiance(hit, origin, sunMask, tracedSun) + HitLampIrradiance(hit, origin, seed);
}

// The light lamps and projected lights scatter in the water of `body` towards a ray along `direction` (away from the
// eye) at a point, before the tonemap scale: each projected light and one sampled lamp (as HitDirectIrradiance lights a
// hit), their shadow rays along their bent paths (LightTransmittance), times the water's albedo (WaterAlbedo) and phase
// function (WaterPhase; light travelling along -toLight scatters towards the eye by the angle whose cosine is
// dot(direction, toLight)), in the units LitWater gives the sun's.
float3 WaterLampLight(float3 position, WaterBody body, float3 direction, inout uint seed)
{
    LightSet set = LightSetAt(position);
    uint projectors = set.count - set.pointLights;
    LightSet sampled = set;
    sampled.count = set.pointLights;
    float3 scattered = 0.0;
    for (uint k = 0; k <= projectors; ++k) {
        float3 light;
        float3 toLight;
        float distance;
        uint index;
        bool projector = k < projectors;
        if (projector) {
            index = set.pointLights + k;
            light = SetLightIrradiance(set, index, position, 0.0, toLight, distance);
        } else if (!SampleLight(sampled, position, 0.0, seed, light, toLight, distance, index)) {
            break;
        }
        if (all(light <= 0.0))
            continue;
        LightSet lit = sampled;
        if (projector)
            lit = set;
        scattered += light * WaterPhase(dot(direction, toLight)) *
            LightTransmittance(lit, index, position, position + toLight * distance, projector ? kLightCentre : float2(Random(seed), Random(seed)));
    }
    return scattered * WaterAlbedo(body);
}

// WaterLampLight along a ray through the water of `water` from `origin` along `direction` for `length` units, at `steps`
// points jittered by `seed`, each standing for its stretch of what still shows (as ThroughWater gathers the water's own
// light).
float3 WaterLampsAlong(float3 origin, float3 direction, float length, uint water, uint steps, inout uint seed)
{
    WaterBody body = LoadWaterBody(water);
    float span = min(length, 4.0 / max(min(body.extinction.r, min(body.extinction.g, body.extinction.b)), 1e-6));
    float3 light = 0.0;
    float3 before = 1.0;
    for (uint s = 0; s < steps; ++s) {
        float3 after = exp(-body.extinction * (span * (s + 1) / steps));
        light += WaterLampLight(origin + direction * (span * (s + Random(seed)) / steps), body, direction, seed) * max(before - after, 0.0);
        before = after;
    }
    return light;
}

#endif
