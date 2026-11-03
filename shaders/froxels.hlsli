#ifndef GMODPP_FROXELS_HLSLI
#define GMODPP_FROXELS_HLSLI

#include "common.hlsli"

// The volumetric light's froxel volume (shaders/volumetrics.hlsl): a grid fitted to the main view's frustum,
// g_frame.froxelDims cells, each column a tile of the screen (a screen position uv lies at uv * froxelUvScale in the
// volume), its slices in view depth from froxelStart over froxelRange, thinner near the eye. The integrated volume
// (volumetricView) holds, per froxel, the light the medium between the eye and the froxel's far side scatters towards
// the eye, before the tonemap scale (rgb), and the share of what lies beyond that it lets through (a).

// The haze reaches this far over the sky, which already shows the haze beyond.
static const float kHazeSkyDistance = 1024.0;

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
    return boundary < 1.0 ? lerp(float4(0.0, 0.0, 0.0, 1.0), light, boundary) : light;
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

#endif
