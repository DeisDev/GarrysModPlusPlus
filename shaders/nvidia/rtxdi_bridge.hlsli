#ifndef GMODPP_RTXDI_BRIDGE_HLSLI
#define GMODPP_RTXDI_BRIDGE_HLSLI

// The application bridge of NVIDIA RTXDI (ReSTIR DI with a ReGIR light grid): the surfaces, lights and light samples its
// headers resample (the RAB_ functions). Built at compile time with RTXDI's headers (CMakeLists.txt,
// RTXDI_SHADER_OPTIONS); nothing here is compiled at runtime.
//
// Local lights, in one index space: the map's lights (Light), its glowing triangles (Emitter, with
// EmissiveLighting::Full), the engine's dynamic lights and the projected lights. The sun stays outside RTXDI. A light's
// sample is a point picked by two numbers in [0, 1): on the disc a map light's sphere shows the surface, uniformly on a
// glowing triangle, or the light's own origin.

#include "../scene.hlsli"

#define RTXDI_REGIR_MODE RTXDI_REGIR_ONION
#define RTXDI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_BASIC
#include "Rtxdi/DI/ReSTIRDIParameters.h"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

// Mirrors gmodpp::RestirConstants in src/restir.h.
struct RestirConstants
{
    RTXDI_Parameters restir;
    RTXDI_LightBufferParameters lightBuffer;
    RTXDI_RuntimeParameters runtime;
    RTXDI_RISBufferSegmentParameters localLightRis;
    RTXDI_RISBufferSegmentParameters environmentRis;
    ReGIR_Parameters regir;
    // Last frame's view-projection inverted, without jitter, for its surfaces (RAB_GetGBufferSurface).
    row_major float4x4 previousInvViewProj;
    // Where each kind of light starts in the index space, and how many there are.
    uint mapLights;
    uint emitters;
    uint dynamicLights;
    uint projectors;
    // The light PDF texture (SRV of all mips; UAVs of each mip from pdfMipUav) and its size.
    uint pdfView;
    uint pdfMipUav;
    uint2 pdfSize;
    // Last frame's light style values, for last frame's map lights.
    uint previousLightStylesView;
    // Emitters' total emission times area (Scene::EmitterPower), which turns their shares into powers.
    float emitterPower;
    // 1 when the reservoirs of last frame may be reused.
    uint historyValid;
    // ReGIR's light slots (ReGIRContext::GetReGIRLightSlotCount).
    uint regirLightSlots;
};

ConstantBuffer<RestirConstants> g_restir : register(b3);
RWStructuredBuffer<uint2> g_risBuffer : register(u1, space2);
RWStructuredBuffer<RTXDI_PackedDIReservoir> g_reservoirs : register(u2, space2);
StructuredBuffer<float2> g_neighborOffsets : register(t1, space2);

#define RTXDI_RIS_BUFFER g_risBuffer
#define RTXDI_LIGHT_RESERVOIR_BUFFER g_reservoirs
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER g_neighborOffsets

// Surface material bits RTXDI compares between neighbours.
static const uint kRestirHalfLambert = 1;
static const uint kRestirTwoSided = 2;
static const uint kRestirOnModel = 4;

struct RAB_Material
{
    uint bits;
};

struct RAB_Surface
{
    float3 position;
    // The normal light is evaluated with (a leaf's turned to the side it is seen from) and the face's.
    float3 normal;
    float3 faceNormal;
    // Where shadow rays start: off the face towards the eye, and behind a leaf for light from behind it.
    float3 rayOrigin;
    float3 behind;
    float3 toEye;
    // The distance from the eye, or 0 for no surface.
    float distance;
    uint material;
};

struct RAB_LightInfo
{
    uint index;
    uint previous;
};

struct RAB_LightSample
{
    // The point shadow rays aim at.
    float3 position;
    // The unshadowed irradiance it gives the surface, over the density of picking that point on the light.
    float3 irradiance;
    float3 toLight;
    float distance;
    // For lights rays can hit (glowing triangles): the density of the point per solid angle; 0 for the others.
    float solidAnglePdf;
    uint light;
};

RAB_Surface RAB_EmptySurface()
{
    RAB_Surface surface = (RAB_Surface)0;
    return surface;
}

RAB_LightInfo RAB_EmptyLightInfo()
{
    RAB_LightInfo light;
    light.index = RTXDI_INVALID_LIGHT_INDEX;
    light.previous = 0;
    return light;
}

RAB_LightSample RAB_EmptyLightSample()
{
    RAB_LightSample sample = (RAB_LightSample)0;
    sample.light = RTXDI_INVALID_LIGHT_INDEX;
    return sample;
}

bool RAB_IsSurfaceValid(RAB_Surface surface) { return surface.distance > 0.0; }
float3 RAB_GetSurfaceWorldPos(RAB_Surface surface) { return surface.position; }
float3 RAB_GetSurfaceNormal(RAB_Surface surface) { return surface.normal; }
float RAB_GetSurfaceLinearDepth(RAB_Surface surface) { return surface.distance; }

RAB_Material RAB_GetMaterial(RAB_Surface surface)
{
    RAB_Material material;
    material.bits = surface.material & (kRestirHalfLambert | kRestirTwoSided);
    return material;
}

bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b) { return a.bits == b.bits; }

int2 RAB_ClampSamplePositionIntoView(int2 pixel, bool previous)
{
    return clamp(pixel, int2(0, 0), int2(g_frame.size) - 1);
}

// A G-buffer pixel as a surface; last frame's has its normal and distance alone (shaders/surface.hlsl) and is lit as
// plain Lambert.
RAB_Surface RAB_GetGBufferSurface(int2 pixel, bool previous)
{
    RAB_Surface surface = RAB_EmptySurface();
    if (any(pixel < 0) || any(pixel >= int2(g_frame.size)))
        return surface;
    if (previous) {
        float4 last = SurfaceAt(g_frame.prevSurfaceView, pixel);
        if (last.w <= 0.0 || g_restir.historyValid == 0)
            return surface;
        float2 uv = (float2(pixel) + 0.5) * g_frame.invSize;
        float4 far = mul(g_restir.previousInvViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
        float3 direction = normalize(far.xyz / far.w - g_frame.prevEyePosition);
        surface.position = g_frame.prevEyePosition + direction * last.w;
        surface.normal = last.xyz;
        surface.faceNormal = last.xyz;
        surface.toEye = -direction;
        surface.distance = last.w;
        surface.rayOrigin = surface.position + last.xyz * SurfaceBias(last.w);
        surface.behind = surface.rayOrigin;
        return surface;
    }
    float4 current = SurfaceAt(g_frame.surfaceView, pixel);
    Texture2D<uint> pixelFlags = ResourceDescriptorHeap[g_frame.pixelFlagsView];
    uint flags = pixelFlags.Load(int3(pixel, 0));
    if (current.w <= 0.0 || (flags & kPixelUnlit) != 0)
        return surface;
    surface.position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    surface.faceNormal = GeometricNormalAt(pixel);
    surface.toEye = normalize(g_frame.eyePosition - surface.position);
    surface.distance = current.w;
    bool twoSided = (flags & kPixelIgnoreNormals) != 0;
    surface.normal = twoSided && dot(current.xyz, surface.faceNormal) < 0.0 ? -current.xyz : current.xyz;
    surface.rayOrigin = SurfaceRayOrigin(surface.position, surface.faceNormal, current.w);
    surface.behind = twoSided ? surface.position - surface.faceNormal * SurfaceBias(current.w) : surface.rayOrigin;
    Texture2D<float4> gbufferFlags = ResourceDescriptorHeap[g_frame.flagsView];
    Texture2D<uint4> visibility = ResourceDescriptorHeap[g_frame.visibilityView];
    uint instance = visibility.Load(int3(pixel, 0)).x;
    surface.material = ((MaterialBits(gbufferFlags.Load(int3(pixel, 0))) & kMaterialHalfLambert) != 0 ? kRestirHalfLambert : 0u) |
        (twoSided ? kRestirTwoSided : 0u) | (instance != kNoView && (LoadInstance(instance).flags & kInstanceModel) != 0 ? kRestirOnModel : 0u);
    return surface;
}

// Lights the surface's diffuse terms (Lambert in shaders/scene.hlsli) as it is.
void UseSurface(RAB_Surface surface)
{
    g_twoSided = (surface.material & kRestirTwoSided) != 0;
    g_halfLambert = (surface.material & kRestirHalfLambert) != 0;
    g_toEye = surface.toEye;
    g_inAir = false;
}

RAB_LightInfo RAB_LoadLightInfo(uint index, bool previous)
{
    RAB_LightInfo light;
    light.index = index;
    light.previous = previous ? 1u : 0u;
    return light;
}

// Light information is read where it is needed; the RIS buffer holds indices alone.
bool RAB_StoreCompactLightInfo(uint linearIndex, RAB_LightInfo light) { return false; }
RAB_LightInfo RAB_LoadCompactLightInfo(uint linearIndex) { return RAB_EmptyLightInfo(); }

// Map lights and glowing triangles keep their indices from frame to frame; the engine's dynamic lights and projected
// lights are listed anew each frame and are not reused across frames.
int RAB_TranslateLightIndex(uint index, bool currentToPrevious)
{
    return index < g_restir.mapLights + g_restir.emitters ? int(index) : -1;
}

// A map light with the style values of this frame or the last.
Light LoadRestirMapLight(uint index, bool previous)
{
    ByteAddressBuffer lights = ResourceDescriptorHeap[g_frame.lightsView];
    Light light = lights.Load<Light>(index * kLightStride);
    uint styles = previous ? g_restir.previousLightStylesView : g_frame.lightStylesView;
    if (styles != kNoView && light.style < kLightStyles) {
        ByteAddressBuffer values = ResourceDescriptorHeap[styles];
        light.intensity *= asfloat(values.Load(light.style * 4));
    }
    return light;
}

Emitter LoadEmitter(uint index)
{
    ByteAddressBuffer emitters = ResourceDescriptorHeap[g_frame.emittersView];
    return emitters.Load<Emitter>(index * 48);
}

// The glow of a point of an emitter, as HitEmission gives it to other surfaces.
float3 EmitterGlow(Emitter emitter, float2 barycentrics)
{
    InstanceRecord record = LoadInstance(emitter.instance);
    ByteAddressBuffer albedo = ResourceDescriptorHeap[NonUniformResourceIndex(record.albedo)];
    HitSurface surface = (HitSurface)0;
    surface.albedo = UnpackAlbedo(albedo.Load(emitter.primitive * 4));
    surface.flags = record.flags;
    HitInfo hit = (HitInfo)0;
    hit.instance = emitter.instance;
    hit.primitive = emitter.primitive;
    hit.barycentrics = barycentrics;
    return HitEmission(surface, hit, 1.0, true);
}

// Barycentrics uniform over a triangle from two numbers in [0, 1), and back.
float2 TriangleBarycentrics(float2 uv)
{
    float root = sqrt(uv.x);
    return float2(root * (1.0 - uv.y), root * uv.y);
}

float2 TriangleSampleUv(float2 barycentrics)
{
    float sum = barycentrics.x + barycentrics.y;
    return float2(sum * sum, sum > 0.0 ? barycentrics.y / sum : 0.0);
}

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo info, RAB_Surface surface, float2 uv)
{
    RAB_LightSample sample = RAB_EmptyLightSample();
    if (info.index == RTXDI_INVALID_LIGHT_INDEX || !RAB_IsSurfaceValid(surface))
        return sample;
    UseSurface(surface);
    sample.light = info.index;
    uint index = info.index;
    if (index < g_restir.mapLights) {
        Light light = LoadRestirMapLight(index, info.previous != 0);
        sample.irradiance = LightIrradiance(light, surface.position, surface.normal, sample.toLight, sample.distance);
        sample.position = DiscPoint(light.origin, light.size, surface.position, uv);
        return sample;
    }
    index -= g_restir.mapLights;
    if (index < g_restir.emitters) {
        Emitter emitter = LoadEmitter(index);
        float2 barycentrics = TriangleBarycentrics(uv);
        sample.position = emitter.p0 + barycentrics.x * (emitter.p1 - emitter.p0) + barycentrics.y * (emitter.p2 - emitter.p0);
        float3 toLight = sample.position - surface.position;
        float distanceSquared = max(dot(toLight, toLight), 1e-4);
        sample.distance = sqrt(distanceSquared);
        sample.toLight = toLight / sample.distance;
        float3 cross_ = cross(emitter.p1 - emitter.p0, emitter.p2 - emitter.p0);
        float area = 0.5 * length(cross_);
        // Source's front faces: cross(p1 - p0, p2 - p0) points out of the back.
        float cosine = dot(cross_ / max(2.0 * area, 1e-8), sample.toLight);
        if (cosine <= 0.0 || area <= 0.0)
            return sample;
        sample.irradiance = EmitterGlow(emitter, barycentrics) * (cosine * area / 3.14159265) * Lambert(surface.normal, sample.toLight) /
            max(distanceSquared, 1.0);
        sample.solidAnglePdf = distanceSquared / (cosine * area);
        return sample;
    }
    index -= g_restir.emitters;
    if (index < g_restir.dynamicLights) {
        ByteAddressBuffer dynamicLights = ResourceDescriptorHeap[g_frame.dynamicLightsView];
        Light light = dynamicLights.Load<Light>(index * kLightStride);
        sample.irradiance = LightIrradiance(light, surface.position, surface.normal, sample.toLight, sample.distance);
        sample.position = light.origin;
        return sample;
    }
    index -= g_restir.dynamicLights;
    if (index < g_restir.projectors) {
        Projector projector = LoadProjector(index);
        sample.irradiance = ProjectorIrradiance(projector, surface.position, surface.normal, sample.toLight, sample.distance);
        sample.position = projector.origin;
    }
    return sample;
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample sample, RAB_Surface surface)
{
    return Luminance(sample.irradiance);
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample sample, out float3 direction, out float distance)
{
    direction = sample.toLight;
    distance = sample.distance;
}

float RAB_LightSampleSolidAnglePdf(RAB_LightSample sample) { return sample.solidAnglePdf; }
bool RAB_IsAnalyticLightSample(RAB_LightSample sample) { return sample.solidAnglePdf <= 0.0; }

// Surfaces are lit as diffuse: their BRDF is sampled by the cosine.
float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface surface, float3 direction)
{
    return saturate(dot(surface.normal, direction)) / 3.14159265;
}

bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 direction)
{
    float u1 = RTXDI_GetNextRandom(rng);
    float u2 = RTXDI_GetNextRandom(rng);
    direction = AboveSurface(CosineSample(surface.normal, u1, u2), surface.faceNormal);
    return true;
}

// The share of a sample's light that reaches the surface, as LightTransmittance traces it (LightPath): from in front of the face
// (behind a leaf for light through it), past lamp shells for sealed lights, and past the local player for its
// attached lights.
float3 SampleTransmittance(RAB_Surface surface, RAB_LightSample sample)
{
    if (sample.light == RTXDI_INVALID_LIGHT_INDEX)
        return 0.0;
    float3 origin = dot(surface.faceNormal, sample.toLight) < 0.0 ? surface.behind : surface.rayOrigin;
    float3 path = sample.position - origin;
    float distance = max(length(path), 1e-3);
    uint index = sample.light;
    float clearance = kLightClearance;
    uint mask = ShadowMask(false);
    if (index < g_restir.mapLights) {
        mask = ShadowMask(false, (MapLightShape(index).x & kLightSealed) != 0);
    } else if (index >= g_restir.mapLights + g_restir.emitters) {
        uint attached = index - g_restir.mapLights - g_restir.emitters;
        clearance = attached < g_restir.dynamicLights ? kAttachedLightClearance : ProjectorClearance(LoadProjector(attached - g_restir.dynamicLights));
        mask = ShadowMask(true);
    }
    bool onModel = (surface.material & kRestirOnModel) != 0;
    // Under water the light follows its bent path through the surface (LightPath).
    g_receiverNormal = surface.normal;
    float3 toLight;
    float3 visibility = LightPath(origin, sample.position, clearance, mask, SelfShadowFlags(onModel, surface.faceNormal, sample.toLight), toLight);
    g_receiverNormal = 0.0;
    return index < g_restir.mapLights && any(visibility > 0.0) ? visibility * BentSpotShare(index, origin, sample.position, toLight) : visibility;
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample sample)
{
    return any(SampleTransmittance(surface, sample) > 0.0);
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, RAB_LightSample sample)
{
    return RAB_GetConservativeVisibility(currentSurface, sample);
}

// A BRDF ray that may meet a glowing triangle: its light index (InstanceRecord::emitterLights) and where on it.
bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax, out uint lightIndex, out float2 uv)
{
    lightIndex = RTXDI_INVALID_LIGHT_INDEX;
    uv = 0.0;
    HitInfo hit;
    if (!TraceHit(origin + direction * tMin, direction, min(tMax, 16384.0) - tMin, g_frame.lightShadowMask, hit))
        return false;
    uint view = LoadInstance(hit.instance).emitterLights;
    if (view != kNoView && g_restir.emitters != 0) {
        // Emitter indices plus one, 0 (or past the table's end) for triangles that do not glow.
        ByteAddressBuffer lights = ResourceDescriptorHeap[NonUniformResourceIndex(view)];
        uint emitter = lights.Load(hit.primitive * 4);
        if (emitter != 0) {
            lightIndex = g_restir.mapLights + emitter - 1;
            uv = TriangleSampleUv(hit.barycentrics);
        }
    }
    return true;
}

// The probability that presampling picks a light: its power over all lights' (the light PDF texture's last mip holds
// the sum).
float RAB_EvaluateLocalLightSourcePdf(uint index)
{
    Texture2D<float> pdf = ResourceDescriptorHeap[g_restir.pdfView];
    uint2 texel = RTXDI_LinearIndexToZCurve(index);
    uint levels = uint(log2(float(max(g_restir.pdfSize.x, g_restir.pdfSize.y)))) + 1;
    float total = pdf.Load(int3(0, 0, levels - 1));
    return total > 0.0 ? pdf.Load(int3(texel, 0)) / total : 0.0;
}

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction) { return 0.0; }
float RAB_EvaluateEnvironmentMapSamplingPdf(float3 direction) { return 0.0; }

// A light's power, in proportion to which presampling picks lights (the light PDF texture): the irradiance it gives at
// unit distance over the directions it shines in.
float LightPower(uint index)
{
    if (index < g_restir.mapLights) {
        Light light = LoadRestirMapLight(index, false);
        float intensity = Luminance(max(light.intensity, 0.0)) * g_frame.lightScale;
        if (light.type == kLightSurface)
            return intensity * 3.14159265;
        float power = intensity * light.referenceScale * 12.5663706;
        // A spot's share of the sphere, from its outer cone.
        return light.type == kLightSpot ? power * saturate(0.5 * (1.0 - light.stopdot2) + 0.05) : power;
    }
    index -= g_restir.mapLights;
    if (index < g_restir.emitters) {
        float share = LoadEmitter(index).cdf - (index > 0 ? LoadEmitter(index - 1).cdf : 0.0);
        return share * g_restir.emitterPower;
    }
    index -= g_restir.emitters;
    if (index < g_restir.dynamicLights) {
        ByteAddressBuffer dynamicLights = ResourceDescriptorHeap[g_frame.dynamicLightsView];
        Light light = dynamicLights.Load<Light>(index * kLightStride);
        return Luminance(max(light.intensity, 0.0)) * g_frame.lightScale * light.radius * light.radius;
    }
    index -= g_restir.dynamicLights;
    if (index < g_restir.projectors) {
        Projector projector = LoadProjector(index);
        float full = max(projector.fullDistance, 16.0);
        return Luminance(projector.color) * full * full;
    }
    return 0.0;
}

// The visibility cluster of the ReGIR cell being filled (shaders/restir.hlsl, RegirMain).
static uint g_volumeCluster = 0;

// Whether the map's visibility data lets a map light reach a cluster: whether the light is in the cluster's list (sorted
// by index; Scene::BuildLightLists).
bool PvsReaches(uint light, uint cluster)
{
    if (g_frame.clusterCount == 0)
        return true;
    ByteAddressBuffer lists = ResourceDescriptorHeap[g_frame.lightRangesView];
    uint2 range = lists.Load2(cluster * 8);
    if (range.y == 0)
        return false;
    ByteAddressBuffer indices = ResourceDescriptorHeap[g_frame.lightIndicesView];
    uint low = 0;
    uint high = range.y - 1;
    while (low < high) {
        uint middle = (low + high) / 2;
        if (indices.Load((range.x + middle) * 4) < light)
            low = middle + 1;
        else
            high = middle;
    }
    return indices.Load((range.x + low) * 4) == light;
}

// The weight, relative to its bound, of a map light the PVS of a ReGIR cell's cluster leaves out (kHiddenWeight in
// src/scene.cpp): such light rarely arrives, but nothing proves it cannot.
static const float kHiddenWeight = 1.0 / 16.0;

// The most a light gives any point of a sphere before shadows, with a Lambert term of 1 (as LightBound in src/scene.cpp
// bounds it for the light grid): ReGIR fills each cell with lights in proportion to it. Zero only where no point of the
// sphere can receive the light.
float RAB_GetLightTargetPdfForVolume(RAB_LightInfo info, float3 centre, float radius)
{
    uint index = info.index;
    if (index == RTXDI_INVALID_LIGHT_INDEX)
        return 0.0;
    if (index < g_restir.mapLights) {
        Light light = LoadRestirMapLight(index, false);
        float3 delta = centre - light.origin;
        float distance = length(delta);
        float nearest = max(distance - radius, 0.0);
        float intensity = Luminance(max(light.intensity, 0.0)) * g_frame.lightScale;
        float bound;
        if (light.type == kLightSurface) {
            if (dot(delta, light.normal) < -radius)
                return 0.0;
            bound = intensity / max(nearest * nearest, 1.0);
        } else {
            if (light.type == kLightSpot && distance > radius) {
                float spread = asin(saturate(radius / distance));
                float margin = asin(saturate(16.0 / max(distance - radius, 16.0)));
                if (acos(clamp(dot(delta / distance, light.normal), -1.0, 1.0)) - spread > acos(clamp(light.stopdot2, -1.0, 1.0)) + margin)
                    return 0.0;
            }
            bound = intensity * light.referenceScale / max(nearest * nearest, light.size * light.size);
        }
        return PvsReaches(index, g_volumeCluster) ? bound : bound * kHiddenWeight;
    }
    index -= g_restir.mapLights;
    if (index < g_restir.emitters) {
        Emitter emitter = LoadEmitter(index);
        float3 middle = (emitter.p0 + emitter.p1 + emitter.p2) / 3.0;
        float3 cross_ = cross(emitter.p1 - emitter.p0, emitter.p2 - emitter.p0);
        float area = 0.5 * length(cross_);
        float extent = max(length(emitter.p0 - middle), max(length(emitter.p1 - middle), length(emitter.p2 - middle)));
        float3 delta = centre - middle;
        if (area <= 0.0 || dot(delta, -cross_ / (2.0 * area)) < -(radius + extent))
            return 0.0;
        float nearest = max(length(delta) - radius - extent, 0.0);
        return Luminance(EmitterGlow(emitter, 1.0 / 3.0)) * area / 3.14159265 / max(nearest * nearest, 1.0);
    }
    index -= g_restir.emitters;
    if (index < g_restir.dynamicLights) {
        ByteAddressBuffer dynamicLights = ResourceDescriptorHeap[g_frame.dynamicLightsView];
        Light light = dynamicLights.Load<Light>(index * kLightStride);
        float nearest = max(length(centre - light.origin) - radius, 0.0) / max(light.radius, 1e-3);
        float falloff = saturate(1.0 - nearest * nearest);
        return Luminance(max(light.intensity, 0.0)) * g_frame.lightScale * falloff * falloff;
    }
    index -= g_restir.dynamicLights;
    if (index < g_restir.projectors) {
        Projector projector = LoadProjector(index);
        float nearest = max(length(centre - projector.origin) - radius, 1.0);
        float full = max(projector.fullDistance, 1.0);
        return Luminance(projector.color) * min(full * full / (nearest * nearest), 1.0);
    }
    return 0.0;
}

#endif
