#ifndef GMODPP_REFLECTION_HLSLI
#define GMODPP_REFLECTION_HLSLI

#include "night_sky.hlsli"
#include "radiance_cache.hlsli"

// Radiance along reflected rays, shared by the reflections pass and water (shaders/water.hlsli).

static const float kMaxDistance = 16384.0;
// Ray Reconstruction's hit distance for reflections of the sky.
static const float kSkyHitDistance = 65504.0;

// The 2D sky toward a ray whose cone widens by `coneSpread`, before the tonemap scale, with `night` the night sky over it
// as the sky's draw shows it, and with `disc` the sun's or Moon's disc while the cone is narrower than it: for mirrors no
// highlight lights (water's glint and Phong highlights already show the sun). Clouds hide what lies beyond them.
float3 SkyBeyond(float3 direction, float coneSpread, bool night, bool disc)
{
    float3 radiance = SkyRadiance(direction);
    if (g_frame.nightSkyView == kNoView || (!night && !disc))
        return radiance;
    StructuredBuffer<NightSkyConstants> constants = ResourceDescriptorHeap[g_frame.nightSkyView];
    NightSkyConstants beyond = constants[0];
    Texture2D<float4> transmittance = ResourceDescriptorHeap[beyond.transmittanceView];
    float clouds = CloudsToward(direction).a;
    float logGain = 0.0;
    if (night && g_frame.skyCubeView != kNoView && (beyond.parts != 0 || beyond.moonIrradiance > 0.0)) {
        TextureCube<float4> sky = ResourceDescriptorHeap[g_frame.skyCubeView];
        logGain = LogGainThroughClouds(sky.SampleLevel(g_linearClamp, direction, 0.0).a, clouds);
    }
    float moonCoverage = 0.0;
    float narrow = saturate(2.0 - coneSpread / beyond.sunRadius);
    if (disc && night)
        radiance += MoonDisc(beyond, direction, coneSpread, logGain, g_frame.tonemapScale, transmittance, g_linearClamp, moonCoverage) *
            (narrow / g_frame.tonemapScale);
    if (night && beyond.parts != 0)
        radiance += NightSkyLight(beyond, direction, logGain, kPointFootprint * coneSpread, g_frame.tonemapScale, transmittance, g_linearClamp) *
            (1.0 - moonCoverage * narrow);
    if (disc && beyond.disc != 0) {
        float coverage;
        radiance += SunDisc(beyond, direction, coneSpread, transmittance, g_linearClamp, coverage) *
            (narrow * DiscThroughClouds(beyond, clouds, g_frame.tonemapScale) / g_frame.tonemapScale);
    }
    return radiance;
}

// What a ray that leaves the map sees: as the game draws the 3D skybox behind the map, the ray goes on from the matching
// point of the skybox (as SunVisibility's do), textured, lit by the sun and the sky and in the skybox's fog, and past it
// the 2D sky (SkyBeyond, with `night` and `disc`). The ray's cone is `coneWidth` across at `origin` and widens by
// `coneSpread` per unit of length.
float3 BeyondMap(float3 origin, float3 direction, float coneWidth, float coneSpread, bool night = true, bool disc = false)
{
    HitSurface hit;
    HitInfo hitInfo;
    float3 skyOrigin = float3(g_frame.skyCameraX, g_frame.skyCameraY, g_frame.skyCameraZ) + origin * g_frame.worldToSky;
    if (g_frame.worldToSky <= 0.0 || (g_frame.traceMask & kSkyboxMask) != 0 || !TraceClosest(skyOrigin, direction, kMaxDistance, kSkyboxMask, hit, hitInfo))
        return SkyBeyond(direction, coneSpread, night, disc);
    float distance = length(hit.position - skyOrigin);
    // Untextured, alpha-tested tree cards took their texture's average, grey from its clear texels, and shone white.
    hit.albedo = TexturedAlbedo(hitInfo, hit.albedo, coneWidth * g_frame.worldToSky + coneSpread * distance);
    float3 irradiance = SunIrradiance(hit, hit.position + hit.normal * 0.25, kSkyboxMask) + SkyLight(hit.normal) * g_frame.indirectScale;
    float3 radiance = ClampSample(hit.albedo * irradiance + HitEmission(hit, hitInfo, distance));
    if (g_frame.skyboxFogEnabled == 0)
        return radiance;
    // The skybox view's range fog, whose colour is after the tonemap scale, lit as the light is now (FogDaylight).
    float fog = saturate(min(g_frame.skyboxFogParams.z, distance * g_frame.skyboxFogParams.w - g_frame.skyboxFogParams.x));
    return lerp(radiance, g_frame.skyboxFogColor * FogDaylight() / g_frame.tonemapScale, g_frame.skyboxFogParams.y > 0.0 ? fog : fog * fog);
}

// The light leaving a reflected hit lit where it is: its albedo times the sun and one local light, and its indirect
// light from one cosine-weighted bounce that ends in the radiance cache, or, where the bounce is too short for the
// cache's voxel, at the surface it meets lit by its own sun and local light. Without the cache the main view's indirect
// light volume (else the sky's light) stands in for the bounce. Never the reflecting pixel's own light, which lit far
// rooms seen in mirrors with the light of the mirror's room; and not the volume where the bounce was traced, whose
// probes in the open air around enclosed spaces (under gm_construct's deck) lit them in reflections. `tracedSun` traces
// the hit's own sunlight under water (HitSunVisibility).
float3 LiveHitRadiance(HitSurface hit, float3 emission, inout uint seed, bool tracedSun)
{
    float3 radiance = 0.0;
    float3 throughput = 1.0;
    [loop]
    for (uint vertex = 0; vertex < 2; ++vertex) {
        float3 origin = hit.position + hit.normal;
        radiance += throughput * (hit.albedo * HitDirectIrradiance(hit, origin, vertex == 0 ? g_frame.sunShadowMask : g_frame.bounceMask, seed,
            tracedSun && vertex == 0) + emission);
        if (vertex == 1)
            break;
        throughput = hit.albedo * g_frame.indirectScale;
        if (!RadianceCacheEnabled()) {
            float3 volume = VolumeIrradiance(hit.position, hit.normal);
            return radiance + throughput * (volume.x >= 0.0 ? volume : SkyLight(hit.normal));
        }
        float3 direction = CosineSample(hit.normal, Random(seed), Random(seed));
        HitSurface next;
        HitInfo nextHit;
        float3 water;
        float3 waterLight;
        if (!TraceThroughWater(origin, direction, kMaxDistance, g_frame.bounceMask, g_frame.indirectRayFlags, seed, next, nextHit, water, waterLight))
            return radiance + throughput * (SkyLight(direction) * water + waterLight);
        emission = BounceEmission(next, nextHit, false);
        float3 cached;
        if (CachedRadiance(next, emission, nextHit.t, nextHit.t, cached))
            return radiance + throughput * (cached * water + waterLight);
        radiance += throughput * waterLight;
        throughput *= water;
        hit = next;
    }
    return radiance;
}

// The angle one pixel of the view spans.
float PixelAngle()
{
    return 2.0 / (float(g_frame.size.y) * length(g_frame.viewProj[1].xyz));
}

// The light along one ray from a water surface, for SeenWater: its first hit, textured and lit by the radiance cache
// where the ray's cone is wide enough for it, else by the sun, one local light and the indirect light volume (or the
// sky's light), or the sky and 3D skybox (BeyondMap) without the night sky: a second copy of it, inlined where forward
// twins see water, cost them compile time for stars seen in water in mirrors and glass. `seed` is fixed by the caller, so
// the same point always takes the same light. `distance` is the hit's distance, kSkyHitDistance for none.
float3 WaterRayRadiance(float3 origin, float3 direction, float coneWidth, uint seed, out float distance)
{
    HitSurface hit;
    HitInfo hitInfo;
    if (!TraceClosest(origin, direction, kMaxDistance, g_frame.traceMask & ~kWaterMask, hit, hitInfo)) {
        distance = kSkyHitDistance;
        return BeyondMap(origin, direction, coneWidth, PixelAngle(), false);
    }
    distance = hitInfo.t;
    hit.albedo = TexturedAlbedo(hitInfo, hit.albedo, coneWidth);
    float3 emission = HitEmission(hit, hitInfo, distance);
    // Where the cone has grown as wide as the radiance cache's voxels, the cache's light, as for other reflected hits.
    float3 cached;
    if (CachedRadiance(hit, emission, distance, coneWidth + PixelAngle() * distance, cached))
        return ClampSample(cached);
    float3 indirect = VolumeIrradiance(hit.position, hit.normal, true);
    if (indirect.x < 0.0)
        indirect = SkyLight(hit.normal);
    float3 shadowOrigin = hit.position + hit.normal;
    return ClampSample(hit.albedo * (HitDirectIrradiance(hit, shadowOrigin, g_frame.sunShadowMask, seed) + indirect * g_frame.indirectScale) + emission);
}

// What a ray sees where it meets a water surface (kWaterMask) on its way: at the rippled surface (WaterSurfaceNormal),
// Fresnel's equations split it into a reflected and a refracted ray, each traced once (WaterRayRadiance). Light from
// under the water crosses it (ThroughWater) and spreads over the wider cone it leaves the water in (1 / n^2); seen from
// below, light from the air is gathered into Snell's window (n^2) and the reflection crosses the water. Every choice is
// fixed, so forward-lit glass and other undenoised rays do not sparkle. `coneWidth` is the arriving ray's width.
float3 SeenWater(float3 position, float3 direction, uint instance, uint primitive, float coneWidth = 1.0)
{
    bool below = direction.z > 0.0;
    float side = below ? -1.0 : 1.0;
    uint water = WaterBodyOfTriangle(instance, primitive);
    bool traced = water != kNoWater;
    float3 normal = traced ? WaterSurfaceNormal(water, LoadWaterBody(water), position) * side : float3(0.0, 0.0, side);
    float fresnel = traced ? Fresnel(-dot(direction, normal), below ? 1.0 / kWaterIor : kWaterIor) : 1.0;
    float3 mirrored = reflect(direction, normal);
    mirrored.z = side * max(mirrored.z * side, 0.02);
    mirrored = normalize(mirrored);
    float3 refracted = refract(direction, normal, below ? kWaterIor : 1.0 / kWaterIor);
    bool refracts = traced && fresnel < 1.0 && dot(refracted, refracted) > 0.0;
    // One call of WaterRayRadiance for both rays keeps the code small where it is inlined.
    float3 reflected = 0.0;
    float3 transmitted = 0.0;
    float reflectedDistance = kSkyHitDistance;
    float transmittedDistance = kSkyHitDistance;
    for (uint r = 0; r < (refracts ? 2u : 1u); ++r) {
        bool mirror = r == 0;
        float distance;
        float3 light = WaterRayRadiance(position + float3(0.0, 0.0, (mirror ? side : -side) * kWaterSurfaceOffset), mirror ? mirrored : normalize(refracted),
            coneWidth, 0x9Eu, distance);
        if (mirror) {
            reflected = light;
            reflectedDistance = distance;
        } else {
            transmitted = light;
            transmittedDistance = distance;
        }
    }
    if (!traced)
        return reflected;
    // From below the reflection crosses the water; from above what is seen through it does.
    uint seed = 0x9Eu;
    float3 inWater = below ? reflected : transmittedDistance < kSkyHitDistance ? transmitted : 0.0;
    float3 crossed = ThroughWater(inWater, position, below ? mirrored : normalize(refracted), below ? reflectedDistance : transmittedDistance, water, 1, seed);
    if (below) {
        reflected = crossed;
        transmitted *= kWaterIor * kWaterIor;
    } else {
        reflected *= LoadWaterBody(water).reflectTint;
        transmitted = refracts ? crossed / (kWaterIor * kWaterIor) : 0.0;
    }
    return fresnel * reflected + (1.0 - fresnel) * transmitted * LoadWaterBody(water).refractTint;
}

// The light along a reflected ray. Its cone is `coneWidth` across at the origin and widens by `coneSpread` per unit of
// length: where it is wide enough for the radiance cache's voxels the cache gives the hit's light; elsewhere the hit is
// lit where it is (LiveHitRadiance). `mask` is what the ray meets; with kWaterMask, water surfaces (SeenWater).
// `hitAlbedo` is the textured albedo of the surface it meets (0 for the sky and water). `tracedSun` as LiveHitRadiance's,
// `disc` as SkyBeyond's.
float3 ReflectedRadiance(float3 origin, float3 direction, float coneWidth, float coneSpread, uint mask, inout uint seed, out float hitDistance,
    out float3 hitAlbedo, bool tracedSun = false, bool disc = false)
{
    HitSurface hit;
    HitInfo hitInfo;
    hitAlbedo = 0.0;
    if (!TraceClosest(origin, direction, kMaxDistance, mask, hit, hitInfo)) {
        hitDistance = kSkyHitDistance;
        return BeyondMap(origin, direction, coneWidth, coneSpread, true, disc);
    }
    hitDistance = hitInfo.t;
    if ((hit.flags & kInstanceWater) != 0)
        return ClampSample(SeenWater(hit.position, direction, hitInfo.instance, hitInfo.primitive, coneWidth + coneSpread * hitDistance));
    hit.albedo = TexturedAlbedo(hitInfo, hit.albedo, coneWidth + coneSpread * hitDistance);
    hitAlbedo = hit.albedo;
    float3 emission = HitEmission(hit, hitInfo, hitDistance);
    float3 cached;
    if (CachedRadiance(hit, emission, hitDistance, coneWidth + coneSpread * hitDistance, cached))
        return ClampSample(cached);
    return ClampSample(LiveHitRadiance(hit, emission, seed, tracedSun));
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
