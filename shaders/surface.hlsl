#include "water.hlsli"

// Resolves the G-buffer into one surface texture the lighting passes share: the world normal (with a depth
// fallback) turned toward the camera and the distance to the eye. Water (shaders/water.hlsli) has no G-buffer surface;
// it gets its distance negated, which every pass takes for no surface, and the rippled normal its reflection leaves at,
// which only Ray Reconstruction reads: with the plane's normal it took the ripples' detail for noise.

// The step to the neighbour on the side of `pixel` (along `axis`) that lies in the pixel's own plane. Depth is linear
// across a plane in screen space, so that side's second depth difference is zero; picking it keeps a pixel next
// to a crease from taking the other plane's slope, whose normal would start its rays inside the wall.
float3 PlaneStep(int2 pixel, int2 axis, float depth, float3 position)
{
    float after = DepthAt(pixel + axis);
    float before = DepthAt(pixel - axis);
    bool forward = abs(DepthAt(pixel + axis * 2) - 2.0 * after + depth) < abs(DepthAt(pixel - axis * 2) - 2.0 * before + depth);
    return forward ? WorldPositionFromDepth(pixel + axis, after) - position : position - WorldPositionFromDepth(pixel - axis, before);
}

// World brushes whose materials need no normals are drawn without them, so the G-buffer holds zero there.
// Brushes are planar, so the normal of the depth surface is exact.
float3 DepthNormal(int2 pixel, float depth, float3 position)
{
    return normalize(cross(PlaneStep(pixel, int2(0, 1), depth, position), PlaneStep(pixel, int2(1, 0), depth, position)));
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    id.xy = DispatchPixel(id.xy);
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    Texture2D<float4> gbufferNormal = ResourceDescriptorHeap[g_frame.gbufferNormalView];
    float depth = DepthAt(pixel);
    float4 coverage = flags.Load(int3(pixel, 0));
    if (coverage.r < 0.5 || depth >= 1.0) {
        float3 toWater = g_frame.eyePosition - WorldPositionFromDepth(pixel, depth);
        output[pixel] = 0.0;
        if (IsWater(coverage) && depth < 1.0) {
            float4 slope = gbufferNormal.Load(int3(pixel, 0));
            float3 position = g_frame.eyePosition - toWater;
            output[pixel] = float4(DrawnWaterNormal(slope.xy, slope.w, normalize(toWater), kWaveSlope, DrawnWaterBody(position)), -length(toWater));
        }
        // The developer probe on water: the normal map's tangent-space slope the game's draw sampled (and its eye angle)
        // and the one shaders sample at the same point (WaterSurfaceSlope), which should agree (Lighting::RequestProbe).
        if (g_frame.probeView != kNoView && IsWater(coverage) && depth < 1.0 && all(uint2(pixel) == uint2(g_frame.probePixel & 0xFFFF, g_frame.probePixel >> 16))) {
            float3 position = WorldPositionFromDepth(pixel, depth);
            uint water = WaterBodyAt(position - float3(0.0, 0.0, 1.0));
            float4 stored = gbufferNormal.Load(int3(pixel, 0));
            float3 drawn = float3(stored.xy, stored.w);
            float3 rebuilt = water != kNoWater ? WaterSurfaceSlope(water, LoadWaterBody(water), position) : 0.0;
            // The eye vector's angle in the tangent plane if tangent x runs along the texture's s axis and y along t.
            float2 axisS, axisT;
            if (water != kNoWater && WaterTextureAxes(water, LoadWaterBody(water), axisS, axisT))
                rebuilt.z = atan2(dot(toWater.xy, axisT), dot(toWater.xy, axisS));
            RWByteAddressBuffer probe = ResourceDescriptorHeap[g_frame.probeView];
            probe.Store4(32, uint4(asuint(drawn), water));
            probe.Store4(48, uint4(asuint(rebuilt), 1));
        }
        return;
    }
    if (g_frame.probeView != kNoView && all(uint2(pixel) == uint2(g_frame.probePixel & 0xFFFF, g_frame.probePixel >> 16))) {
        RWByteAddressBuffer probe = ResourceDescriptorHeap[g_frame.probeView];
        probe.Store(60, 0);
    }
    float3 position = WorldPositionFromDepth(pixel, depth);
    float3 toEye = g_frame.eyePosition - position;
    float4 stored = gbufferNormal.Load(int3(pixel, 0));
    float3 normal;
    if (IsWaterSurface(MaterialBits(coverage)))
        normal = WaterNormal(stored.xy, stored.w, normalize(toEye), 1.0);
    else
        normal = dot(stored.xyz, stored.xyz) > 0.25 ? normalize(stored.xyz) : DepthNormal(pixel, depth, position);
    // Two-sided materials report the front-face normal on back faces.
    if (dot(normal, toEye) < 0.0)
        normal = -normal;
    output[pixel] = float4(normal, length(toEye));
}
