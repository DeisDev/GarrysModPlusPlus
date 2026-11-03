#ifndef GMODPP_WATER_HLSLI
#define GMODPP_WATER_HLSLI

#include "reflection.hlsli"

// Water surfaces (shaders/water_surface.hlsl wrote them into the G-buffer's targets where it holds no surface): the
// surface's Fresnel split between a ray-traced reflection and a ray-traced refraction through the water below it
// (ThroughWater), or from under the water into the air above, with a sun glint.
// albedo: Source's refraction, fogged by the water, which water outside the map's bodies of water (brush entities)
// keeps; normal: the normal map's tangent-space slope (xy), the reflect tint's brightness and the angle of the
// tangent-space eye vector (kNoEyeAngle without one); flags: g 1, a the share of the reflection Source's shore fade
// leaves.

// The normal map's slopes are softened for the traced reflections' directions, which mirror the scene where Source
// distorted a texture by a few texels. The Fresnel term keeps the full normal, as Source's does: flattened, the water
// near the horizon became a white mirror of the sky.
static const float kWaveSlope = 0.35;
// The sun's highlight: a GGX lobe of this roughness (alpha). Reflections are mirrors at the rippled normal: water has
// no roughness besides its ripples, and a lobe sampled per pixel added noise that the denoisers smeared.
static const float kGlintRoughness = 0.03;
// Rays through the water gather its light at this many points (ThroughWater).
static const uint kWaterSteps = 2;
// Seen from above, the surface fades out where the water under it is shallower than this, straight down from what the
// refraction meets: its reflection, glint, tint and the 1 / n^2 its refraction loses leave what lies under it as it is
// at the waterline. Without it the flat surface cut a hard line along the shore, darker by about half.
static const float kShoreDepth = 8.0;

bool IsWater(float4 flags) { return flags.r < 0.5 && flags.g > 0.5; }
// A mirror's surface (translator::Variant::markMirror), which shows the game's reflection view.
bool IsMirror(float4 flags) { return flags.r < 0.5 && flags.g < 0.5 && flags.b > 0.5; }

static const float kNoEyeAngle = 10.0;

// The world-space normal of a horizontal water surface, facing the eye. The tangent frame is the surface's texture
// axes; the direction of the eye vector in it (`eyeAngle`) and in the world give its rotation about the plane normal.
float3 WaterNormal(float2 tangentSlope, float eyeAngle, float3 toEye, float slopeScale)
{
    float side = toEye.z >= 0.0 ? 1.0 : -1.0;
    float2 slope = tangentSlope * slopeScale;
    if (eyeAngle < kNoEyeAngle && dot(toEye.xy, toEye.xy) > 1e-6) {
        float angle = atan2(toEye.y, toEye.x) - eyeAngle;
        float s = sin(angle);
        float c = cos(angle);
        slope = float2(c * slope.x - s * slope.y, s * slope.x + c * slope.y);
    }
    float z = sqrt(saturate(1.0 - dot(tangentSlope, tangentSlope)));
    return normalize(float3(slope, max(z, 0.2) * side));
}

// The body of water under a drawn water pixel at `position`, or kNoWater (also without ray tracing).
uint DrawnWaterBody(float3 position)
{
    return g_frame.traceMask != 0 ? WaterBodyAt(position - float3(0.0, 0.0, 1.0)) : kNoWater;
}

// The world normal of a drawn water pixel from the normal map's tangent-space slope the G-buffer holds: by its body's
// texture axes where they are known (tangent x along s, y along t, as Water_vs20 takes them; also where the axes are
// mirrored, which no rotation from the eye angle can match), else by the eye angle (WaterNormal).
float3 DrawnWaterNormal(float2 tangentSlope, float eyeAngle, float3 toEye, float slopeScale, uint water)
{
    float2 s, t;
    if (water == kNoWater || !WaterTextureAxes(water, LoadWaterBody(water), s, t))
        return WaterNormal(tangentSlope, eyeAngle, toEye, slopeScale);
    float z = sqrt(saturate(1.0 - dot(tangentSlope, tangentSlope)));
    float2 slope = tangentSlope * slopeScale;
    return normalize(float3(s * slope.x + t * slope.y, max(z, 0.2) * (toEye.z >= 0.0 ? 1.0 : -1.0)));
}

// The sun's specular highlight on the water, before the tonemap scale.
float3 SunGlint(float3 position, float3 normal, float3 toEye)
{
    float3 toSun = g_frame.sunDirection;
    float nl = dot(normal, toSun);
    float nv = dot(normal, toEye);
    if (g_frame.sunEnabled == 0.0 || nl <= 0.0 || nv <= 0.0)
        return 0.0;
    float3 halfway = normalize(toSun + toEye);
    float nh = saturate(dot(normal, halfway));
    float a2 = kGlintRoughness * kGlintRoughness;
    float d = a2 / (3.14159265 * pow(nh * nh * (a2 - 1.0) + 1.0, 2.0));
    float k = kGlintRoughness * 0.5;
    float g = (nl / (nl * (1.0 - k) + k)) * (nv / (nv * (1.0 - k) + k));
    float f = Fresnel(dot(halfway, toEye), kWaterIor);
    float3 visibility = SunVisibility(position + normal * 2.0, g_frame.sunShadowMask);
    return ClampSample(g_frame.sunColor * (d * g * f / (4.0 * nv)) * visibility);
}

struct WaterResult
{
    float3 color;
    float3 specularAlbedo;
    // Ray Reconstruction's diffuse albedo guide, where `refractedGuide` (traced water seen from above).
    float3 diffuseAlbedo;
    bool refractedGuide;
    float hitDistance;
};

// The shaded water at a pixel, before the tonemap scale. The rippled normal splits the light by Fresnel's equations
// into a mirror reflection and a refraction bent by Snell's law. From above, the refraction crosses the water to what
// lies under it (ThroughWater), and its radiance spreads over the wider cone it leaves the water in (1 / n^2); from
// below, light from the air in Snell's window is
// gathered into the narrower cone (n^2), beyond it the surface mirrors the water entirely, and the reflection crosses
// the water too. Source's $refracttint tints what comes through. Seen from above, the surface fades out towards the
// shore (kShoreDepth). Water outside the map's bodies of water (brush entities) keeps Source's refraction, fogged by
// Source, under the traced reflection.
// The two rays share one call of ReflectedRadiance and the water one of ThroughWater: inlined more often, their code
// slowed every pixel of the pass.
WaterResult ShadeWater(int2 pixel, float3 position)
{
    Texture2D<float4> normals = ResourceDescriptorHeap[g_frame.gbufferNormalView];
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    float4 tangentNormal = normals.Load(int3(pixel, 0));
    float4 marks = flags.Load(int3(pixel, 0));
    float3 toEye = g_frame.eyePosition - position;
    float distance = length(toEye);
    toEye /= max(distance, 1e-4);
    uint water = DrawnWaterBody(position);
    bool traced = water != kNoWater;
    float tint = tangentNormal.z;
    float3 normal = DrawnWaterNormal(tangentNormal.xy, tangentNormal.w, toEye, kWaveSlope, water);
    float3 fresnelNormal = DrawnWaterNormal(tangentNormal.xy, tangentNormal.w, toEye, 1.0, water);
    bool below = toEye.z < 0.0;
    float side = below ? -1.0 : 1.0;
    float ior = below ? 1.0 / kWaterIor : kWaterIor;
    // Source's shore fade (the refraction's alpha) only for its own refraction.
    float fresnel = Fresnel(dot(fresnelNormal, toEye), ior) * (traced || below ? 1.0 : marks.a);
    uint seed = PixelSeed(pixel, 0x5Au);
    // Rays leave from the body's surface itself: the depth buffer puts far water units off it, and a reflected ray that
    // started under the surface would meet its underside.
    float3 surface = traced ? float3(position.xy, LoadWaterBody(water).surfaceZ) : position;

    WaterResult result;
    float3 direction = reflect(-toEye, normal);
    // Ripples can tilt the direction into the surface; reflect it back off the plane.
    if (direction.z * side < 0.02)
        direction.z = side * max(abs(direction.z), 0.02);
    direction = normalize(direction);
    float3 refracted = refract(-toEye, fresnelNormal, 1.0 / ior);
    bool refracts = traced && fresnel < 1.0 && dot(refracted, refracted) > 0.0;
    if (refracts) {
        if (refracted.z * side > -0.02)
            refracted.z = -side * max(abs(refracted.z), 0.02);
        refracted = normalize(refracted);
    }

    float3 reflection = 0.0;
    float3 seen = 0.0;
    float seenDistance = kSkyHitDistance;
    float3 seenAlbedo = 0.0;
    result.hitDistance = kSkyHitDistance;
    uint rays = g_frame.traceMask == 0 ? 0 : refracts ? 2 : 1;
    for (uint r = 0; r < rays; ++r) {
        bool reflected = r == 0;
        float3 rayDirection = reflected ? direction : refracted;
        // Both rays leave from the eye's side of the surface, which the refraction's mask leaves out: one that started past
        // it began under the ground where the shore meets the water within that offset, saw nothing, and drew the water's
        // own dark colour along the waterline.
        float offset = side * (traced ? kWaterSurfaceOffset : 2.0);
        // The refraction carries on the view, which never shows the local player (shaders/primary.hlsl): of a body the
        // game does not draw in first person, only the feet showed under the water. Reflections show it.
        uint mask = reflected ? g_frame.traceMask | (traced && !below ? kWaterMask : 0u) : g_frame.traceMask & ~kLocalPlayerMask;
        float hitDistance;
        float3 hitAlbedo;
        float3 light = ReflectedRadiance(surface + float3(0.0, 0.0, offset), rayDirection, PixelAngle() * distance, PixelAngle(), mask, seed, hitDistance,
            hitAlbedo, true);
        if (reflected) {
            reflection = light * (tint * g_frame.reflectionScale);
            result.hitDistance = hitDistance;
        } else {
            seen = light;
            seenDistance = hitDistance;
            seenAlbedo = hitAlbedo;
        }
    }

    float3 transmitted;
    float shore = 1.0;
    if (traced) {
        // From below the reflection crosses the water; from above what is seen through it does.
        float3 inWater = below ? reflection : seenDistance < kSkyHitDistance ? seen : 0.0;
        float3 crossedDirection = below ? direction : refracted;
        float crossedDistance = below ? result.hitDistance : seenDistance;
        float3 crossed = ThroughWater(inWater, surface, crossedDirection, crossedDistance, water, kWaterSteps, seed) +
            WaterLampsAlong(surface, crossedDirection, crossedDistance, water, kWaterSteps, seed);
        if (below) {
            reflection = crossed;
            transmitted = seen * (kWaterIor * kWaterIor);
        } else {
            transmitted = refracts ? crossed / (kWaterIor * kWaterIor) : 0.0;
        }
        transmitted *= LoadWaterBody(water).refractTint;
        if (!below && refracts && seenDistance < kSkyHitDistance)
            shore = smoothstep(0.0, kShoreDepth, -refracted.z * seenDistance);
        fresnel *= shore;
        transmitted = lerp(seen, transmitted, shore);
    } else {
        // Source's refraction holds the colour the game would have written, linear before its tonemap scale. Seen from
        // below, the reflected underwater scene lies behind more of Source's water fog.
        Texture2D<float4> albedos = ResourceDescriptorHeap[g_frame.albedoView];
        transmitted = albedos.Load(int3(pixel, 0)).rgb / max(g_frame.tonemapScale, 1e-3);
        if (below && g_frame.fogEnabled != 0) {
            float fog = saturate(min(g_frame.fogParams.z, result.hitDistance * g_frame.fogParams.w - g_frame.fogParams.x));
            reflection = lerp(reflection, g_frame.fogColor * FogDaylight() / max(g_frame.tonemapScale, 1e-3), g_frame.fogParams.y > 0.0 ? fog : fog * fog);
        }
    }
    result.color = fresnel * reflection + (1.0 - fresnel) * transmitted;
    // Ray Reconstruction keeps the detail its albedo guide shows and smooths the rest as noisy light, so a refracting
    // surface's guide should match the refracted colour (DLSS-RR Integration Guide, 3.5): from above, the albedo of what
    // the refraction meets as far as the water lets it through, and the water's own albedo for the light it scatters,
    // both as the surface transmits them. Source's refraction, which the guide holds elsewhere, is lit and bent otherwise.
    result.refractedGuide = traced && !below;
    result.diffuseAlbedo = 0.0;
    if (result.refractedGuide && refracts) {
        WaterBody body = LoadWaterBody(water);
        float3 through = seenDistance < kSkyHitDistance ? exp(-body.extinction * seenDistance) : 0.0;
        float3 waterGuide = body.refractTint / (kWaterIor * kWaterIor) * (through * seenAlbedo + (1.0 - through) * WaterAlbedo(body));
        result.diffuseAlbedo = saturate((1.0 - fresnel) * lerp(seenAlbedo, waterGuide, shore));
    }
    if (!below)
        result.color += SunGlint(surface, normal, toEye) * (g_frame.reflectionScale * (traced ? shore : marks.a));
    result.specularAlbedo = saturate(fresnel * tint);
    return result;
}

#endif
