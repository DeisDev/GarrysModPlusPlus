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
    // kInstanceIgnoreNormals.
    uint flags;
    uint3 padding;
};

// kInstanceIgnoreNormals in src/scene.h.
static const uint kInstanceIgnoreNormals = 0x01;

// Mirrors GpuLight in src/scene.cpp (bsp::Light, VRAD units and falloff).
struct Light
{
    float3 origin;
    uint type;
    float3 intensity;
    float radius;
    float3 normal;
    float stopdot;
    float stopdot2;
    float exponent;
    float constantAttn;
    float linearAttn;
    float quadraticAttn;
    // Byte offset in the lights buffer of the convex hulls that block the light (HullBlocks), and their number.
    uint hullOffset;
    uint hullCount;
    // Light style (switchable and animated lights), or kNoLightStyle.
    uint style;
};

static const uint kNoLightStyle = 0xFFFFFFFFu;
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
    uint padding;
};

// Opacity where a ray crosses a candidate in a model's non-opaque geometry: 0 or 1 for alpha-tested triangles,
// texture alpha times $alpha for translucent ones (`translucent`). The alpha is filtered as the game's alpha tests
// filter it; unfiltered, the low-resolution alpha's texels showed as square blocks in leaf shadows.
float CandidateOpacity(uint instance, uint alphaPrimitive, float2 barycentrics, out bool translucent)
{
    ByteAddressBuffer alpha = ResourceDescriptorHeap[LoadInstance(instance).alpha];
    AlphaTriangle entry = alpha.Load<AlphaTriangle>(alphaPrimitive * 32);
    uint packed = entry.packed;
    translucent = (packed & kTranslucentFlag) != 0;
    float value = ((packed >> 24) & 0x7F) * (1.0 / 127.0);
    float textureAlpha = 1.0;
    if ((packed & 0xFFFFFF) != kNoAlphaTexture) {
        float2 uv = entry.uv0 + barycentrics.x * (entry.uv1 - entry.uv0) + barycentrics.y * (entry.uv2 - entry.uv0);
        Texture2D<float4> texture = ResourceDescriptorHeap[packed & 0xFFFFFF];
        textureAlpha = texture.SampleLevel(g_linearWrap, uv, 0.0).a;
    }
    if (translucent)
        return saturate(textureAlpha * value);
    return textureAlpha >= value ? 1.0 : 0.0;
}

// Rays other than shadow rays stop at candidates at least this opaque.
static const float kSolidOpacity = 0.5;

// With `gbufferSurface`, translucent candidates never stop the ray: the game draws them after the G-buffer.
bool CandidateIsSolid(uint instance, uint alphaPrimitive, float2 barycentrics, bool gbufferSurface = false)
{
    bool translucent;
    float opacity = CandidateOpacity(instance, alphaPrimitive, barycentrics, translucent);
    return opacity >= kSolidOpacity && !(gbufferSurface && translucent);
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
    ByteAddressBuffer positions = ResourceDescriptorHeap[record.positions];
    ByteAddressBuffer indices = ResourceDescriptorHeap[record.indices];
    ByteAddressBuffer albedo = ResourceDescriptorHeap[record.albedo];
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
    return surface;
}

// The closest surface along a ray, or false when it reaches tMax.
bool TraceClosest(float3 origin, float3 direction, float tMax, uint mask, out HitSurface surface, uint rayFlags = RAY_FLAG_NONE)
{
    surface = (HitSurface)0;
    HitInfo hit;
    if (!TraceHit(origin, direction, tMax, mask, hit, rayFlags))
        return false;
    surface = SurfaceOf(hit, origin, direction);
    return true;
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
    float padding;
};

static const uint kNoMaterial = 0xFFFFFFFFu;
// Material flags (Scene::kMaterialUnlit): the surface shows its albedo as it is.
static const uint kMaterialUnlit = 1;

// The material flags of a hit triangle, 0 without a material.
uint MaterialFlags(HitInfo hit)
{
    InstanceRecord record = LoadInstance(hit.instance);
    if (record.materials == kNoView)
        return 0;
    ByteAddressBuffer materials = ResourceDescriptorHeap[record.materials];
    uint index = materials.Load(hit.primitive * 4);
    if (index == kNoMaterial)
        return 0;
    ByteAddressBuffer table = ResourceDescriptorHeap[g_frame.materialsView];
    return table.Load(index * 48 + 40);
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
    ByteAddressBuffer materials = ResourceDescriptorHeap[record.materials];
    uint index = materials.Load(hit.primitive * 4);
    if (index == kNoMaterial)
        return false;
    ByteAddressBuffer table = ResourceDescriptorHeap[g_frame.materialsView];
    material = table.Load<Material>(index * 48);
    if (record.uvs != kNoView) {
        ByteAddressBuffer indices = ResourceDescriptorHeap[record.indices];
        ByteAddressBuffer uvs = ResourceDescriptorHeap[record.uvs];
        ByteAddressBuffer positions = ResourceDescriptorHeap[record.positions];
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
    Texture2D<float4> texture = ResourceDescriptorHeap[view];
    uint width, height, levels;
    texture.GetDimensions(0, width, height, levels);
    float lod = log2(max(footprint * max(width, height), 1e-6));
    float value = texture.SampleLevel(g_linearWrap, uv, lod)[(mask >> 24) & 3];
    return (mask & (1u << 26)) != 0 ? 1.0 - value : value;
}

// The share of light that gets from `origin` to tMax along a ray: 0 past anything solid, otherwise the product of
// what each translucent surface crossed lets through.
float Transmittance(float3 origin, float3 direction, float tMax, uint mask)
{
    if (mask == 0)
        return 1.0;
    SCENE_RAY_QUERY(RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER) query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.01;
    ray.TMax = tMax;
    query.TraceRayInline(g_scene, RAY_FLAG_NONE, mask, ray);
    float transmittance = 1.0;
    while (query.Proceed()) {
        bool translucent;
        float opacity = CandidateOpacity(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), translucent);
        if (translucent)
            transmittance *= 1.0 - opacity;
        if ((!translucent && opacity > 0.0) || transmittance < 0.01)
            query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : transmittance;
}

// `mask` is g_frame.sunShadowMask, or g_frame.bounceMask for the light of indirect bounces.
float SunVisibility(float3 origin, uint mask)
{
    return Transmittance(origin, g_frame.sunDirection, 65536.0, mask);
}

// Sunlight reaching a hit surface, with its shadow ray starting at `origin`.
float3 SunIrradiance(HitSurface hit, float3 origin, uint mask)
{
    float lambert = saturate(dot(hit.normal, g_frame.sunDirection));
    if (g_frame.sunEnabled == 0.0 || lambert <= 0.0)
        return 0.0;
    return g_frame.sunColor * lambert * SunVisibility(origin, mask);
}

// Map lights that can reach a position: the (offset, count) range of light indices of its visibility cluster
// (Scene::LightLists).
struct LightRanges
{
    uint first;
    uint count;
};

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

// Surface points lie on leaf boundaries, so the lookup is made just off the surface.
LightRanges LightsAt(float3 position, float3 normal)
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

uint LightRangeCount(LightRanges ranges) { return ranges.count; }

// The i-th map light of LightsAt's ranges.
uint LightRangeIndex(LightRanges ranges, uint i)
{
    ByteAddressBuffer indices = ResourceDescriptorHeap[g_frame.lightIndicesView];
    return indices.Load((ranges.first + i) * 4);
}

// Irradiance below which a light cannot change the encoded result (1/1024 after the tonemap scale), so its
// shadow ray is not traced. VRAD's falloff never reaches zero, and maps have hundreds of lights.
float VisibleIrradiance() { return (1.0 / 1024.0) / max(g_frame.tonemapScale, 1e-3); }

// Dynamic and projected lights are often attached to a player or weapon (flashlights, muzzle flashes) whose
// model surrounds the light, so occluders right next to them are ignored.
static const float kAttachedLightClearance = 48.0;

// kLocalPlayerMask in src/scene.h.
static const uint kLocalPlayerMask = 0x08;
// kSkyboxMask in src/scene.h.
static const uint kSkyboxMask = 0x10;
// kShadowlessPropMask in src/scene.h.
static const uint kShadowlessPropMask = 0x20;

// Other lights' shadow rays stop this far short of them.
static const float kLightClearance = 1.0;

float ShadowRayLength(float clearance, float distance) { return distance - min(clearance, distance * 0.5); }

// Shadow rays of local lights. Dynamic and projected lights are mostly the local player's own (flashlight, muzzle
// flashes), which the game never shadows with the player's body.
uint ShadowMask(bool attachedLight)
{
    uint mask = g_frame.traceMask & ~kShadowlessPropMask;
    return attachedLight ? mask & ~kLocalPlayerMask : mask;
}

// VRAD lights props that ignore normals (STATIC_PROP_IGNORE_NORMALS) as if every light met them at CONSTANT_DOT;
// a zero normal stands for them.
static const float kIgnoredNormalLambert = 0.35;

float Lambert(float3 normal, float3 toLight)
{
    return any(normal != 0.0) ? saturate(dot(normal, toLight)) : kIgnoredNormalLambert;
}

// Map lights first, then the engine's dynamic lights.
Light LoadLight(uint index)
{
    if (index < g_frame.lightCount) {
        ByteAddressBuffer lights = ResourceDescriptorHeap[g_frame.lightsView];
        Light light = lights.Load<Light>(index * 80);
        if (g_frame.lightStylesView != kNoView && light.style < kLightStyles) {
            ByteAddressBuffer styles = ResourceDescriptorHeap[g_frame.lightStylesView];
            light.intensity *= asfloat(styles.Load(light.style * 4));
        }
        return light;
    }
    ByteAddressBuffer dynamicLights = ResourceDescriptorHeap[g_frame.dynamicLightsView];
    return dynamicLights.Load<Light>((index - g_frame.lightCount) * 80);
}

// Whether the straight path from a map light to a point crosses one of the light's convex hulls: the static prop
// meshes VRAD shadowed it with (FixtureHulls in src/effects.cpp). Points inside a hull with the light stay lit.
bool HullBlocks(Light light, float3 position)
{
    ByteAddressBuffer lights = ResourceDescriptorHeap[g_frame.lightsView];
    float3 path = position - light.origin;
    // Paths that only touch a hull, within half a unit, pass.
    float margin = 0.5 / max(length(path), 1.0);
    uint offset = light.hullOffset;
    for (uint h = 0; h < light.hullCount; ++h) {
        uint planeCount = lights.Load(offset);
        offset += 16;
        float enter = 0.0;
        float leave = 1.0;
        for (uint p = 0; p < planeCount; ++p, offset += 16) {
            float4 plane = lights.Load<float4>(offset);
            float start = dot(plane.xyz, light.origin) - plane.w - 0.1;
            float rate = dot(plane.xyz, path);
            if (rate > 1e-6)
                leave = min(leave, -start / rate);
            else if (rate < -1e-6)
                enter = max(enter, -start / rate);
            else if (start > 0.0)
                leave = -1.0;
        }
        if (leave - enter > margin && (enter > margin || leave < 1.0 - margin))
            return true;
    }
    return false;
}

// Half the width, in world units, over which spot light cones fade at least: about one luxel of a finely lit floor.
static const float kSpotEdgeBlur = 8.0;

// Unshadowed irradiance from a light, following VRAD's GatherSampleStandardLightSSE. `toLight` and
// `distance` describe the shadow ray.
float3 LightIrradiance(Light light, float3 position, float3 normal, out float3 toLight, out float distance)
{
    float3 delta = light.origin - position;
    float distanceSquared = max(dot(delta, delta), 1e-4);
    distance = sqrt(distanceSquared);
    toLight = delta / distance;
    float lambert = Lambert(normal, toLight);
    if (lambert <= 0.0 || (light.radius > 0.0 && distance > light.radius))
        return 0.0;
    float falloffDistance = max(distance, 1.0);
    float falloff;
    if (light.type == kLightDynamic) {
        float r = distance / light.radius;
        falloff = saturate(1.0 - r * r);
        falloff *= falloff;
        if (light.exponent != 0.0)
            falloff *= smoothstep(light.stopdot2, light.stopdot, -dot(toLight, light.normal));
        return light.intensity * (lambert * falloff) * g_frame.lightScale;
    }
    if (light.type == kLightSurface) {
        falloff = saturate(-dot(toLight, light.normal)) / distanceSquared;
    } else {
        falloff = 1.0 / max(light.constantAttn + light.linearAttn * falloffDistance + light.quadraticAttn * falloffDistance * falloffDistance, 1e-4);
        if (light.type == kLightSpot) {
            float spot = -dot(toLight, light.normal);
            // Lightmaps blur the cone's edge over their luxels; without that, cones whose inner and outer angles
            // (nearly) match end in a hard line. The fringe spans at least kSpotEdgeBlur units on either side.
            float edge = 0.5 * (light.stopdot + light.stopdot2);
            float spread = sqrt(saturate(1.0 - edge * edge)) * kSpotEdgeBlur / distance;
            float outer = max(min(light.stopdot2, edge - spread), -1.0);
            float inner = min(max(light.stopdot, edge + spread), 1.0);
            if (spot <= outer)
                return 0.0;
            falloff *= spot;
            if (spot <= inner) {
                float fringe = saturate((spot - outer) / max(inner - outer, 1e-4));
                if (light.exponent != 0.0 && light.exponent != 1.0)
                    fringe = pow(fringe, light.exponent);
                falloff *= fringe;
            }
        }
    }
    // The engine stops lighting at the radius VRAD computed; fade the last part so the cut does not show.
    if (light.radius > 0.0)
        falloff *= saturate((light.radius - distance) / (0.2 * light.radius));
    if (light.hullCount != 0 && HullBlocks(light, position))
        return 0.0;
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
        Texture2D<float4> texture = ResourceDescriptorHeap[projector.cookieView];
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

// The lights that can reach a point, in one index space: its cluster's map lights, then the engine's dynamic
// lights, then the projected lights.
struct LightSet
{
    LightRanges ranges;
    uint mapLights;
    uint pointLights;
    uint count;
};

LightSet LightSetAt(float3 position, float3 normal)
{
    LightSet set;
    set.ranges = LightsAt(position, normal);
    set.mapLights = LightRangeCount(set.ranges);
    set.pointLights = set.mapLights + g_frame.dynamicLightCount;
    set.count = set.pointLights + g_frame.projectorCount;
    return set;
}

// Dynamic and projected lights are the ones attached to players and weapons.
bool IsAttachedLight(LightSet set, uint i) { return i >= set.mapLights; }

// Unshadowed irradiance from the i-th light of a LightSet.
float3 SetLightIrradiance(LightSet set, uint i, float3 position, float3 normal, out float3 toLight, out float distance)
{
    if (i >= set.pointLights)
        return ProjectorIrradiance(LoadProjector(i - set.pointLights), position, normal, toLight, distance);
    uint index = i >= set.mapLights ? g_frame.lightCount + i - set.mapLights : LightRangeIndex(set.ranges, i);
    return LightIrradiance(LoadLight(index), position, normal, toLight, distance);
}

// The share of the i-th light of a LightSet that reaches `origin`, `distance` away along `toLight`.
float LightTransmittance(LightSet set, uint i, float3 origin, float3 toLight, float distance)
{
    bool attached = IsAttachedLight(set, i);
    float clearance = !attached ? kLightClearance : i >= set.pointLights ? ProjectorClearance(LoadProjector(i - set.pointLights)) : kAttachedLightClearance;
    return Transmittance(origin, toLight, ShadowRayLength(clearance, distance), ShadowMask(attached));
}

static const uint kLightCandidates = 8;

// One light of a LightSet by resampled importance sampling: up to kLightCandidates lights drawn uniformly (every
// light when there are no more), one of them, `picked`, kept in proportion to its unshadowed luminance. `irradiance`
// is its unshadowed light divided by the probability of the pick; false when no candidate can light the point.
bool SampleLight(LightSet set, float3 position, float3 normal, inout uint seed, out float3 irradiance, out float3 toLight, out float distance,
    out uint picked)
{
    irradiance = 0.0;
    toLight = 0.0;
    distance = 0.0;
    picked = 0;
    if (set.count == 0)
        return false;
    bool exhaustive = set.count <= kLightCandidates;
    uint draws = exhaustive ? set.count : kLightCandidates;
    // Uniform draws weigh each candidate by count / draws, which cancels out of the pick itself.
    float drawWeight = exhaustive ? 1.0 : float(set.count) / float(draws);
    float total = 0.0;
    float pickedWeight = 0.0;
    for (uint d = 0; d < draws; ++d) {
        uint i = exhaustive ? d : min(uint(Random(seed) * set.count), set.count - 1);
        float3 candidateToLight;
        float candidateDistance;
        float weight = Luminance(SetLightIrradiance(set, i, position, normal, candidateToLight, candidateDistance));
        if (weight <= 0.0)
            continue;
        total += weight;
        if (Random(seed) * total < weight) {
            picked = i;
            pickedWeight = weight;
        }
    }
    if (total <= 0.0)
        return false;
    irradiance = SetLightIrradiance(set, picked, position, normal, toLight, distance) * (total * drawWeight / pickedWeight);
    return true;
}

#endif
