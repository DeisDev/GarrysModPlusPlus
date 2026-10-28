#ifndef GMODPP_WATER_HLSLI
#define GMODPP_WATER_HLSLI

#include "reflection.hlsli"

// Water surfaces (shaders/water_surface.hlsl wrote them into the G-buffer's targets where it holds no surface): Source's
// refraction, already fogged by the water, under ray-traced reflections, a sun glint and the Fresnel term of water.
// albedo: refraction (as the game would write it); normal: the normal map's tangent-space slope (xy), the reflect tint's
// brightness and the angle of the tangent-space eye vector (kNoEyeAngle without one); flags: g 1, a the share of the
// reflection the shore leaves.

// The normal map's slopes are softened for the traced reflections' directions, which mirror the scene where Source
// distorted a texture by a few texels. The Fresnel term keeps the full normal, as Source's does: flattened, the water
// near the horizon became a white mirror of the sky.
static const float kWaveSlope = 0.35;
// The sun's highlight: a GGX lobe of this roughness (alpha).
static const float kGlintRoughness = 0.03;
static const float kWaterIor = 1.333;
static const float kWaterF0 = 0.02;

bool IsWater(float4 flags) { return flags.r < 0.5 && flags.g > 0.5; }

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

float Schlick(float cosine) { return kWaterF0 + (1.0 - kWaterF0) * pow(1.0 - saturate(cosine), 5.0); }

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
    float f = Schlick(dot(halfway, toEye));
    float visibility = SunVisibility(position + normal * 2.0, g_frame.sunShadowMask);
    return ClampSample(g_frame.sunColor * (d * g * f / (4.0 * nv)) * visibility);
}

struct WaterResult
{
    float3 color;
    float3 specularAlbedo;
    float hitDistance;
};

// The shaded water at a pixel, before the tonemap scale. `previousView` is last frame's shaded colour of the main view.
WaterResult ShadeWater(int2 pixel, float3 position, uint previousView)
{
    Texture2D<float4> albedos = ResourceDescriptorHeap[g_frame.albedoView];
    Texture2D<float4> normals = ResourceDescriptorHeap[g_frame.gbufferNormalView];
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    float4 tangentNormal = normals.Load(int3(pixel, 0));
    float4 marks = flags.Load(int3(pixel, 0));
    float tint = tangentNormal.z;
    float3 toEye = g_frame.eyePosition - position;
    float distance = length(toEye);
    toEye /= max(distance, 1e-4);
    float3 normal = WaterNormal(tangentNormal.xy, tangentNormal.w, toEye, kWaveSlope);
    float3 fresnelNormal = WaterNormal(tangentNormal.xy, tangentNormal.w, toEye, 1.0);
    // The refraction holds the colour the game would have written, which is linear before its tonemap scale.
    float3 refraction = albedos.Load(int3(pixel, 0)).rgb / max(g_frame.tonemapScale, 1e-3);

    float cosine = saturate(dot(fresnelNormal, toEye));
    float fresnel = Schlick(cosine) * marks.a;
    // Below the surface, light from beyond the critical angle is reflected entirely (Snell's window). The shore fade is
    // for the view from above, where the refraction's alpha holds the water's depth.
    bool below = toEye.z < 0.0;
    if (below) {
        float sine = sqrt(saturate(1.0 - cosine * cosine)) * kWaterIor;
        fresnel = sine >= 1.0 ? 1.0 : Schlick(sqrt(saturate(1.0 - sine * sine)));
    }
    WaterResult result;
    result.hitDistance = kSkyHitDistance;
    float3 reflection = 0.0;
    if (g_frame.traceMask != 0 && g_frame.reflectionScale > 0.0) {
        uint seed = PixelSeed(pixel, 0x5Au);
        float3 direction = reflect(-toEye, normal);
        // Ripples can tilt the mirror direction into the water; reflect it back off the plane.
        float side = below ? -1.0 : 1.0;
        if (direction.z * side < 0.02)
            direction.z = side * max(abs(direction.z), 0.02);
        direction = normalize(direction);
        // The 3D skybox's view has no last-frame image of its own.
        uint previous = (g_frame.traceMask & kSkyboxMask) != 0 ? kNoView : previousView;
        reflection = ReflectedRadiance(previous, position + float3(0.0, 0.0, side * 2.0), direction, g_frame.skyColor * g_frame.indirectScale, seed,
            result.hitDistance);
        reflection *= tint * g_frame.reflectionScale;
        // Seen from below, the reflected underwater scene lies behind more water.
        if (below && g_frame.fogEnabled != 0) {
            float fog = saturate(min(g_frame.fogParams.z, result.hitDistance * g_frame.fogParams.w - g_frame.fogParams.x));
            reflection = lerp(reflection, g_frame.fogColor / max(g_frame.tonemapScale, 1e-3), g_frame.fogParams.y > 0.0 ? fog : fog * fog);
        }
    }
    result.color = lerp(refraction, reflection, fresnel);
    if (!below)
        result.color += SunGlint(position, normal, toEye) * (g_frame.reflectionScale * marks.a);
    result.specularAlbedo = saturate(fresnel * tint);
    return result;
}

#endif
