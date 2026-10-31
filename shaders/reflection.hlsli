#ifndef GMODPP_REFLECTION_HLSLI
#define GMODPP_REFLECTION_HLSLI

#include "radiance_cache.hlsli"

// Radiance along reflected rays, shared by the reflections pass and water (shaders/water.hlsli).

static const float kMaxDistance = 16384.0;
// Ray Reconstruction's hit distance for reflections of the sky.
static const float kSkyHitDistance = 65504.0;

// What a ray that leaves the map sees: as the game draws the 3D skybox behind the map, the ray goes on from the matching
// point of the skybox (as SunVisibility's do), lit by the sun and the sky and in the skybox's fog, and past it the 2D
// sky.
float3 BeyondMap(float3 origin, float3 direction)
{
    if (g_frame.worldToSky <= 0.0 || (g_frame.traceMask & kSkyboxMask) != 0)
        return SkyRadiance(direction);
    float3 skyOrigin = float3(g_frame.skyCameraX, g_frame.skyCameraY, g_frame.skyCameraZ) + origin * g_frame.worldToSky;
    HitSurface hit;
    HitInfo hitInfo;
    if (!TraceClosest(skyOrigin, direction, kMaxDistance, kSkyboxMask, hit, hitInfo))
        return SkyRadiance(direction);
    float distance = length(hit.position - skyOrigin);
    float3 irradiance = SunIrradiance(hit, hit.position + hit.normal * 0.25, kSkyboxMask) + SkyLight(hit.normal) * g_frame.indirectScale;
    float3 radiance = ClampSample(hit.albedo * irradiance + HitEmission(hit, hitInfo, distance));
    if (g_frame.skyboxFogEnabled == 0)
        return radiance;
    // The skybox view's range fog (ApplyFog in shaders/deferred_lighting.hlsl), whose colour is after the tonemap scale.
    float fog = saturate(min(g_frame.skyboxFogParams.z, distance * g_frame.skyboxFogParams.w - g_frame.skyboxFogParams.x));
    return lerp(radiance, g_frame.skyboxFogColor / g_frame.tonemapScale, g_frame.skyboxFogParams.y > 0.0 ? fog : fog * fog);
}

// The indirect light reaching a reflected hit: one cosine-weighted bounce that ends in the radiance cache, or else the
// main view's indirect light volume there, or else the sky's light. Never the reflecting pixel's own light, which lit
// far rooms seen in mirrors with the light of the mirror's room.
float3 HitIndirectIrradiance(HitSurface hit, float3 origin, inout uint seed)
{
    if (RadianceCacheEnabled()) {
        float3 direction = CosineSample(hit.normal, Random(seed), Random(seed));
        HitSurface next;
        HitInfo nextHit;
        if (!TraceClosest(origin, direction, kMaxDistance, g_frame.bounceMask, next, nextHit, g_frame.indirectRayFlags))
            return SkyLight(direction);
        float3 cached;
        if (CachedRadiance(next, BounceEmission(next, nextHit, false), nextHit.t, nextHit.t, cached))
            return cached;
    }
    float3 volume = VolumeIrradiance(hit.position, hit.normal);
    return volume.x >= 0.0 ? volume : SkyLight(hit.normal);
}

// The light along a reflected ray. Its cone is `coneWidth` across at the origin and widens by `coneSpread` per unit of
// length: where it is wide enough for the radiance cache's voxels the cache gives the hit's light; elsewhere the hit is
// lit where it is by the sun and one local light, with HitIndirectIrradiance as its indirect light.
float3 ReflectedRadiance(float3 origin, float3 direction, float coneWidth, float coneSpread, inout uint seed, out float hitDistance)
{
    HitSurface hit;
    HitInfo hitInfo;
    if (!TraceClosest(origin, direction, kMaxDistance, g_frame.traceMask, hit, hitInfo)) {
        hitDistance = kSkyHitDistance;
        return BeyondMap(origin, direction);
    }
    hitDistance = hitInfo.t;
    float3 emission = HitEmission(hit, hitInfo, hitDistance);
    float3 cached;
    if (CachedRadiance(hit, emission, hitDistance, coneWidth + coneSpread * hitDistance, cached))
        return ClampSample(cached);
    float3 shadowOrigin = hit.position + hit.normal;
    float3 irradiance = HitDirectIrradiance(hit, shadowOrigin, g_frame.sunShadowMask, seed) + HitIndirectIrradiance(hit, shadowOrigin, seed) * g_frame.indirectScale;
    return ClampSample(hit.albedo * irradiance + emission);
}

// The angle one pixel of the view spans.
float PixelAngle()
{
    return 2.0 / (float(g_frame.size.y) * length(g_frame.viewProj[1].xyz));
}

// How fast a GGX lobe of roughness `alpha` widens per unit of length (SHaRC's effective cone spread of a glossy lobe).
float LobeSpread(float alpha)
{
    float a2 = min(alpha * alpha, 0.999);
    return 2.0 * sqrt(0.5 * a2 / (1.0 - a2));
}

// A microfacet normal of a GGX lobe of roughness `alpha` around `normal`, sampled from the normals visible from `toEye`
// (Heitz 2018, "Sampling the GGX Distribution of Visible Normals", JCGT 7(4)), by two numbers in [0, 1).
float3 SampleGgxVndf(float3 normal, float3 toEye, float alpha, float2 u)
{
    float3 t, b;
    BuildBasis(normal, t, b);
    float3 ve = float3(dot(toEye, t), dot(toEye, b), dot(toEye, normal));
    float3 vh = normalize(float3(alpha * ve.x, alpha * ve.y, ve.z));
    float lengthSquared = vh.x * vh.x + vh.y * vh.y;
    float3 t1 = lengthSquared > 0.0 ? float3(-vh.y, vh.x, 0.0) * rsqrt(lengthSquared) : float3(1.0, 0.0, 0.0);
    float3 t2 = cross(vh, t1);
    float r = sqrt(u.x);
    float phi = 6.28318530718 * u.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;
    float3 nh = p1 * t1 + p2 * t2 + sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0)) * vh;
    float3 ne = normalize(float3(alpha * nh.x, alpha * nh.y, max(nh.z, 0.0)));
    return normalize(t * ne.x + b * ne.y + normal * ne.z);
}

float SmithLambda(float cosine, float alpha)
{
    float c2 = max(cosine * cosine, 1e-6);
    return 0.5 * (sqrt(1.0 + alpha * alpha * (1.0 - c2) / c2) - 1.0);
}

// The weight of a reflection sampled from a GGX lobe's visible normals (SampleGgxVndf), before Fresnel: the
// height-correlated Smith shadowing-masking over the masking alone, G2 / G1(view).
float GgxVndfWeight(float cosineView, float cosineLight, float alpha)
{
    if (cosineLight <= 0.0)
        return 0.0;
    float view = SmithLambda(cosineView, alpha);
    return (1.0 + view) / (1.0 + view + SmithLambda(cosineLight, alpha));
}

// A reflection's direction around `normal`: the mirror direction, or below kMirrorRoughness or without glossy
// reflections, a direction from the GGX lobe with its weight; mirrored back above the face (`faceNormal`) where it
// points into it.
float3 ReflectionDirection(float3 view, float3 normal, float3 faceNormal, float alpha, float2 u, out float weight)
{
    weight = 1.0;
    float3 toEye = -view;
    if (alpha < kMirrorRoughness || g_frame.glossyReflections == 0 || dot(normal, toEye) <= 0.0)
        return AboveSurface(reflect(view, normal), faceNormal);
    float3 microfacet = SampleGgxVndf(normal, toEye, alpha, u);
    float3 direction = reflect(view, microfacet);
    weight = GgxVndfWeight(dot(normal, toEye), dot(normal, direction), alpha);
    return AboveSurface(direction, faceNormal);
}

// The specular albedo Ray Reconstruction takes: the reflectance `f0` integrated over a GGX lobe of roughness `alpha`
// (EnvBRDFApprox2 in the DLSS-RR Integration Guide's appendix, from Ray Tracing Gems chapter 32).
float3 SpecularAlbedo(float3 f0, float alpha, float cosineView)
{
    float nv = abs(cosineView);
    float4 x = float4(1.0, nv, nv * nv, nv * nv * nv);
    float4 y = float4(1.0, alpha, alpha * alpha, alpha * alpha * alpha);
    float2x2 m1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
    float3x3 m2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627);
    float2x2 m3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
    float3x3 m4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123);
    float bias = dot(mul(m1, x.xy), y.xy) * rcp(dot(mul(m2, x.xyw), y.xyw));
    float scale = dot(mul(m3, x.xy), y.xy) * rcp(dot(mul(m4, x.xzw), y.xyw));
    bias *= saturate(f0.g * 50.0);
    return mad(f0, max(0.0, scale), max(0.0, bias));
}

#endif
