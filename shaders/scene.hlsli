#ifndef GMODPP_SCENE_HLSLI
#define GMODPP_SCENE_HLSLI

#include "common.hlsli"

// Mirrors Scene::InstanceRecord in src/scene.cpp: descriptor indices of raw buffers. A model's alpha-tested
// triangles follow its opaque ones in a second BLAS geometry; `alpha` holds one Scene::PackAlpha entry for each,
// `materials` a material table index per triangle.
struct InstanceRecord
{
    uint positions;
    uint indices;
    uint albedo;
    uint uvs;
    uint alpha;
    uint opaqueTriangles;
    uint materials;
    uint padding;
};

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
    float3 padding;
};

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
    float padding;
};

static const uint kNoCookie = 0xFFFFFFFFu;

struct HitSurface
{
    float3 position;
    float3 normal;
    float3 albedo;
};

InstanceRecord LoadInstance(uint instance)
{
    ByteAddressBuffer records = ResourceDescriptorHeap[g_frame.instancesView];
    return records.Load<InstanceRecord>(instance * 32);
}

float3 UnpackAlbedo(uint packed)
{
    return float3(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF) * (1.0 / 255.0);
}

// Whether a candidate in a model's alpha-tested geometry is solid where the ray crosses it.
bool AlphaTestPasses(uint instance, uint alphaPrimitive, float2 barycentrics)
{
    InstanceRecord record = LoadInstance(instance);
    ByteAddressBuffer alpha = ResourceDescriptorHeap[record.alpha];
    ByteAddressBuffer indices = ResourceDescriptorHeap[record.indices];
    ByteAddressBuffer uvs = ResourceDescriptorHeap[record.uvs];
    uint packed = alpha.Load(alphaPrimitive * 4);
    uint3 index = indices.Load3((record.opaqueTriangles + alphaPrimitive) * 12);
    float2 uv0 = uvs.Load<float2>(index.x * 8);
    float2 uv = uv0 + barycentrics.x * (uvs.Load<float2>(index.y * 8) - uv0) + barycentrics.y * (uvs.Load<float2>(index.z * 8) - uv0);
    Texture2D<float4> texture = ResourceDescriptorHeap[packed & 0xFFFFFF];
    return texture.SampleLevel(g_pointWrap, uv, 0.0).a >= (packed >> 24) * (1.0 / 255.0);
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

// The closest triangle along a ray, or false when it reaches tMax.
bool TraceHit(float3 origin, float3 direction, float tMax, uint mask, out HitInfo hit)
{
    hit = (HitInfo)0;
    RayQuery<RAY_FLAG_NONE> query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.5;
    ray.TMax = tMax;
    query.TraceRayInline(g_scene, RAY_FLAG_NONE, mask, ray);
    while (query.Proceed()) {
        if (AlphaTestPasses(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;
    hit.instance = query.CommittedInstanceID();
    // Geometry 1 is the alpha-tested one; with no opaque triangles the alpha-tested geometry is 0 and starts at 0.
    hit.primitive = query.CommittedPrimitiveIndex() + (query.CommittedGeometryIndex() != 0 ? LoadInstance(hit.instance).opaqueTriangles : 0);
    hit.barycentrics = query.CommittedTriangleBarycentrics();
    hit.t = query.CommittedRayT();
    hit.objectToWorld = query.CommittedObjectToWorld3x4();
    return true;
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
bool TraceClosest(float3 origin, float3 direction, float tMax, uint mask, out HitSurface surface)
{
    surface = (HitSurface)0;
    HitInfo hit;
    if (!TraceHit(origin, direction, tMax, mask, hit))
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
    float2 padding;
};

static const uint kNoMaterial = 0xFFFFFFFFu;
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

bool Occluded(float3 origin, float3 direction, float tMax, uint mask)
{
    if (mask == 0)
        return false;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.5;
    ray.TMax = tMax;
    query.TraceRayInline(g_scene, RAY_FLAG_NONE, mask, ray);
    while (query.Proceed()) {
        if (AlphaTestPasses(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float SunVisibility(float3 origin)
{
    return Occluded(origin, g_frame.sunDirection, 65536.0, g_frame.traceMask) ? 0.0 : 1.0;
}

// Sunlight reaching a hit surface, with its shadow ray starting at `origin`.
float3 SunIrradiance(HitSurface hit, float3 origin)
{
    float lambert = saturate(dot(hit.normal, g_frame.sunDirection));
    if (g_frame.sunEnabled == 0.0 || lambert <= 0.0)
        return 0.0;
    return g_frame.sunColor * lambert * SunVisibility(origin);
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

float ShadowRayLength(bool attachedLight, float distance)
{
    return attachedLight ? distance - min(kAttachedLightClearance, distance * 0.5) : distance - 1.0;
}

// Dynamic and projected lights are mostly the local player's own (flashlight, muzzle flashes), which the game
// never shadows with the player's body.
uint ShadowMask(bool attachedLight)
{
    return attachedLight ? g_frame.traceMask & ~kLocalPlayerMask : g_frame.traceMask;
}

// Map lights first, then the engine's dynamic lights.
Light LoadLight(uint index)
{
    if (index < g_frame.lightCount) {
        ByteAddressBuffer lights = ResourceDescriptorHeap[g_frame.lightsView];
        return lights.Load<Light>(index * 80);
    }
    ByteAddressBuffer dynamicLights = ResourceDescriptorHeap[g_frame.dynamicLightsView];
    return dynamicLights.Load<Light>((index - g_frame.lightCount) * 80);
}

// Unshadowed irradiance from a light, following VRAD's GatherSampleStandardLightSSE. `toLight` and
// `distance` describe the shadow ray.
float3 LightIrradiance(Light light, float3 position, float3 normal, out float3 toLight, out float distance)
{
    float3 delta = light.origin - position;
    float distanceSquared = max(dot(delta, delta), 1e-4);
    distance = sqrt(distanceSquared);
    toLight = delta / distance;
    float lambert = saturate(dot(normal, toLight));
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
            if (spot <= light.stopdot2)
                return 0.0;
            falloff *= spot;
            if (spot <= light.stopdot) {
                float fringe = saturate((spot - light.stopdot2) / max(light.stopdot - light.stopdot2, 1e-4));
                if (light.exponent != 0.0 && light.exponent != 1.0)
                    fringe = pow(fringe, light.exponent);
                falloff *= fringe;
            }
        }
    }
    // The engine stops lighting at the radius VRAD computed; fade the last part so the cut does not show.
    if (light.radius > 0.0)
        falloff *= saturate((light.radius - distance) / (0.2 * light.radius));
    return light.intensity * (lambert * falloff) * g_frame.lightScale;
}

Projector LoadProjector(uint index)
{
    ByteAddressBuffer projectors = ResourceDescriptorHeap[g_frame.projectorsView];
    return projectors.Load<Projector>(index * 112);
}

// Unshadowed irradiance from a projected light, following DoFlashlight in common_flashlight_fxc.h; without a
// cookie texture the frustum gets a round spot.
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
    float3 coords = projected.xyz / projected.w;
    if (any(coords < 0.0) || any(coords > 1.0))
        return 0.0;
    float lambert = saturate(dot(normal, toLight));
    if (lambert <= 0.0)
        return 0.0;
    float3 cookie;
    if (projector.cookieView != kNoCookie) {
        Texture2D<float4> texture = ResourceDescriptorHeap[projector.cookieView];
        cookie = texture.SampleLevel(g_linearClamp, coords.xy, 0.0).rgb;
    } else {
        cookie = smoothstep(0.5, 0.35, length(coords.xy - 0.5));
    }
    float attenuation = saturate(dot(projector.attenuation, float3(1.0, 1.0 / distance, 1.0 / (distance * distance))));
    float endFalloff = saturate((distance - projector.farZ) / (-0.4 * projector.farZ));
    return cookie * projector.color * (attenuation * lambert * endFalloff);
}

#endif
