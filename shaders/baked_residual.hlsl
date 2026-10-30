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
// Step 2 (leaf ambient): the leaf ambient samples lose the direct light of the dim surface lights VRAD added to them
// (AddEmitSurfaceLights in src/utils/vrad/leaf_ambient_lighting.cpp), which ray tracing adds back. input0:
// AmbientLights; input1: their count; input3: first sample; input4: sample count; output0: ambient samples, changed
// in place; output1: counters (3: samples with a cube side clamped at zero).

// VRAD's occluders: the world, and static props and brushes as VRAD shaped them (kWorldMask | kVradOccluderMask in
// src/scene.h).
static const uint kVradOccluders = 0x01 | 0x40;
// A luxel counts as clamped when the direct light exceeds the baked light by more than this share of it.
static const float kClampTolerance = 0.05;

// VRAD's shadows: every triangle of its occluders blocks, alpha-tested and translucent world faces included. VRAD
// traces brushes without alpha (AddBrushToRaytraceEnvironment in src/utils/vrad/trace.cpp), and blocking translucent
// faces too matched de_investment's lightmaps better (18.2% of luxels clamped instead of 19.4%).
float SolidTransmittance(float3 origin, float3 direction, float tMax, uint mask)
{
    SCENE_RAY_QUERY(RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE) query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.01;
    ray.TMax = tMax;
    query.TraceRayInline(g_scene, RAY_FLAG_NONE, mask, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

// Whether VRAD let the sun reach a point's cluster: it lights a sample only from lights whose PVS holds the sample's
// cluster, and the sun's is what the light_environment and the clusters with sky faces see.
bool SunReaches(float3 position)
{
    if (g_frame.sunClustersView == kNoView)
        return true;
    uint cluster = ClusterAt(position);
    if (cluster >= g_frame.clusterCount)
        return true;
    ByteAddressBuffer clusters = ResourceDescriptorHeap[g_frame.sunClustersView];
    return (clusters.Load((cluster >> 5) * 4) & (1u << (cluster & 31))) != 0;
}

// SunVisibility (shaders/scene.hlsli) with SolidTransmittance, through the 3D skybox as VRAD's TestLine_DoesHitSky.
float SolidSunVisibility(float3 origin)
{
    float visibility = SolidTransmittance(origin, g_frame.sunDirection, 65536.0, kVradOccluders);
    if (visibility > 0.0 && g_frame.worldToSky > 0.0) {
        float3 camera = float3(g_frame.skyCameraX, g_frame.skyCameraY, g_frame.skyCameraZ);
        // The 3D skybox's faces and props, and its brushes as VRAD traced them (kVradSkyboxOccluderMask).
        visibility *= SolidTransmittance(camera + origin * g_frame.worldToSky, g_frame.sunDirection, 65536.0, kSkyboxMask | 0x80);
    }
    return visibility;
}

// The direct light of style-0 lights and the sun at a luxel, for its flat normal (direct[0]) and, when bumped,
// its three bump basis normals (direct[1-3]), as VRAD gathers it (GatherSampleLightSSE and
// GatherSampleStandardLightSSE in src/utils/vrad/lightmap.cpp): a light behind the flat face or blocked adds nothing
// to any bump sample, and point and spot lights' bump samples are not scaled by partial visibility.
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
    if (g_frame.sunEnabled != 0.0 && dot(normals[0], g_frame.sunDirection) > 0.0 && SunReaches(origin)) {
        float visibility = SolidSunVisibility(origin);
        for (uint n = 0; n < count && visibility > 0.0; ++n)
            direct[n] += g_frame.sunColor * saturate(dot(normals[n], g_frame.sunDirection)) * visibility;
    }
    LightRanges ranges = LightsAt(luxel.position, luxel.normal);
    for (uint i = 0; i < LightRangeCount(ranges); ++i) {
        Light light = LoadLight(LightRangeIndex(ranges, i));
        // Other styles have their own lightmaps, which hold no bounce light.
        if (light.style != 0)
            continue;
        float3 toLight;
        float distance;
        float3 flat = LightIrradiance(light, luxel.position, normals[0], toLight, distance);
        if (all(flat <= 0.0))
            continue;
        float visibility = SolidTransmittance(origin, toLight, ShadowRayLength(kLightClearance, distance), kVradOccluders);
        if (visibility <= 0.0)
            continue;
        direct[0] += flat * visibility;
        for (uint n = 1; n < count; ++n)
            direct[n] += LightIrradiance(light, luxel.position, normals[n], toLight, distance);
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

struct AmbientLight
{
    float3 origin;
    float radius;
    float4 intensity;
    float4 normal;
};

static const float3 kBoxDirections[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};

// VRAD's falloff for these lights is the engine's for surface lights (Engine_WorldLightDistanceFalloff and
// Engine_WorldLightAngle, l_studio.cpp): inverse square from 1 unit, within the radius, times the cosine at the
// emitting surface; the receiving direction is the light's own, so each cube side takes its cosine to the light.
void RemoveAmbientLights(uint index)
{
    ByteAddressBuffer lights = ResourceDescriptorHeap[g_pass.input0];
    RWByteAddressBuffer samples = ResourceDescriptorHeap[g_pass.output0];
    AmbientSample sample = samples.Load<AmbientSample>(index * 112);
    float3 position = sample.position.xyz;
    bool changed = false;
    bool clamped = false;
    for (uint i = 0; i < g_pass.input1; ++i) {
        AmbientLight light = lights.Load<AmbientLight>(i * 48);
        float3 delta = light.origin - position;
        float distanceSquared = dot(delta, delta);
        if (light.radius != 0.0 && distanceSquared > light.radius * light.radius)
            continue;
        float distance = sqrt(distanceSquared);
        float3 toLight = delta / max(distance, 1e-4);
        // ON_EPSILON / 10: behind the light's surface.
        float emitter = -dot(toLight, light.normal.xyz);
        if (emitter <= 0.01)
            continue;
        float visibility = SolidTransmittance(position, toLight, ShadowRayLength(kLightClearance, distance), kVradOccluders);
        if (visibility <= 0.0)
            continue;
        float ratio = emitter * visibility / max(distanceSquared, 1.0);
        for (uint s = 0; s < 6; ++s) {
            float t = dot(kBoxDirections[s], toLight);
            if (t <= 0.0)
                continue;
            float3 value = sample.cube[s].rgb - light.intensity.rgb * (t * ratio);
            clamped = clamped || any(value < -kClampTolerance * max(sample.cube[s].rgb, 1e-4));
            sample.cube[s].rgb = max(value, 0.0);
            changed = true;
        }
    }
    if (changed)
        samples.Store<AmbientSample>(index * 112, sample);
    if (clamped) {
        RWByteAddressBuffer counters = ResourceDescriptorHeap[g_pass.output1];
        counters.InterlockedAdd(12, 1);
    }
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
    else if (g_pass.step == 1)
        Filter(index);
    else
        RemoveAmbientLights(index);
}
