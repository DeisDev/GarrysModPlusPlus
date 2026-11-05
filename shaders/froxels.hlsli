#ifndef GMODPP_FROXELS_HLSLI
#define GMODPP_FROXELS_HLSLI

#include "common.hlsli"

// The volumetric light's froxel volume (shaders/volumetrics.hlsl): a grid fitted to the main view's frustum,
// g_frame.froxelDims cells, each column a tile of the screen (a screen position uv lies at uv * froxelUvScale in the
// volume), its slices in view depth from froxelStart over froxelRange, thinner near the eye. The integrated volume
// (volumetricView) holds, per froxel, the light the medium between the eye and the froxel's far side scatters towards
// the eye, before the tonemap scale (rgb), and the share of what lies beyond that it lets through (a).

// The haze reaches this far over the sky, which already shows the haze beyond. Ground mist lies over the ground to the
// horizon, so the sky shows it as far as it goes (MistDepth).
static const float kHazeSkyDistance = 1024.0;
static const float kMistSkyDistance = 1e6;

float SkyMediumDistance()
{
    return g_frame.mistEnabled != 0.0 ? kMistSkyDistance : kHazeSkyDistance;
}

// The eye ray through screen position `uv`: the step along it per unit of view depth.
float3 FroxelRay(float2 uv)
{
    float4 toPoint = mul(g_frame.clipToEye, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
    float3 offset = toPoint.xyz / toPoint.w;
    return offset / max(dot(g_frame.viewProj[3].xyz, offset), 1e-6);
}

// Ground mist's optical depth along `length` units of a ray from `start` along unit `direction`, without its noise: an
// exponential height fog's closed form (Wenzel 2006), its density e times less every 1 / mistInvFalloff units up.
float MistDepth(float3 start, float3 direction, float length)
{
    float above = (start.z - g_frame.mistBase) * g_frame.mistInvFalloff;
    float rise = clamp(direction.z * length * g_frame.mistInvFalloff, -60.0, 60.0);
    float shape = abs(rise) > 1e-4 ? (1.0 - exp(-rise)) / rise : 1.0 - 0.5 * rise;
    return g_frame.mistDensity * exp(-clamp(above, -60.0, 60.0)) * length * shape;
}

// Slices are linear in view depth near the eye and grow exponentially beyond about a 30th of the range.
static const float kFroxelCurve = 5.0;

float FroxelDepth(float slice, float start, float range)
{
    return start + range * (exp2(kFroxelCurve * slice) - 1.0) / (exp2(kFroxelCurve) - 1.0);
}

float FroxelSlice(float viewDepth, float start, float range)
{
    return log2(1.0 + max(viewDepth - start, 0.0) / max(range, 1e-3) * (exp2(kFroxelCurve) - 1.0)) / kFroxelCurve;
}

// The light scattered towards the eye along the screen position `uv` up to view depth `viewDepth` (rgb, before the
// tonemap scale) and the share of the light from that depth that reaches the eye (a).
float4 FroxelLight(float2 uv, float viewDepth)
{
    if (g_frame.volumetricView == kNoView)
        return float4(0.0, 0.0, 0.0, 1.0);
    float slices = float(g_frame.froxelDims.z);
    float boundary = min(FroxelSlice(viewDepth, g_frame.froxelStart, g_frame.froxelRange), 1.0) * slices;
    if (boundary <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    Texture3D<float4> volume = ResourceDescriptorHeap[g_frame.volumetricView];
    // Slice k holds what has gathered by its far side, so a depth `boundary` slices in reads slice boundary - 1;
    // within the first slice the light grows from none.
    float4 light = volume.SampleLevel(g_linearClamp, float3(uv * g_frame.froxelUvScale, (boundary - 0.5) / slices), 0.0);
    light = boundary < 1.0 ? lerp(float4(0.0, 0.0, 0.0, 1.0), light, boundary) : light;
    // Ground mist goes on past the volume: its closed form, lit as the volume's far side is on average (mistFarColor).
    float end = g_frame.froxelStart + g_frame.froxelRange;
    if (g_frame.mistEnabled != 0.0 && viewDepth > end) {
        float3 ray = FroxelRay(uv);
        float rayScale = length(ray);
        float through = exp(-MistDepth(g_frame.eyePosition + ray * end, ray / max(rayScale, 1e-6), (viewDepth - end) * rayScale));
        light.rgb += light.a * (1.0 - through) * g_frame.mistFarColor;
        light.a *= through;
    }
    return light;
}

// FroxelLight for a point in the world, seen from the main view's eye.
float4 FroxelLightAt(float3 position)
{
    float4 clip = mul(g_frame.viewProj, float4(position, 1.0));
    if (clip.w <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float2 ndc = clip.xy / clip.w;
    return FroxelLight(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5), clip.w);
}

// FroxelLight in front of the open 2D sky at screen position `uv`, `direction` from the eye: as far as SkyMediumDistance.
float4 FroxelLightOverSky(float2 uv, float3 direction)
{
    float3 position = g_frame.eyePosition + direction * SkyMediumDistance();
    return FroxelLight(uv, mul(g_frame.viewProj, float4(position, 1.0)).w);
}

#endif
