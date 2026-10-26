#include "common.hlsli"

// Resolves the G-buffer into one surface texture the lighting passes share: the world normal (with a depth
// fallback) turned toward the camera and the distance to the eye.

float3 PositionAt(int2 pixel)
{
    return WorldPositionFromDepth(pixel, DepthAt(pixel));
}

// World brushes whose materials need no normals are drawn without them, so the G-buffer holds zero there.
// Brushes are planar, so the normal of the depth surface is exact away from edges; the smaller neighbour
// difference on each axis keeps edges from blending two planes.
float3 DepthNormal(int2 pixel, float3 position)
{
    float3 right = PositionAt(pixel + int2(1, 0)) - position;
    float3 left = position - PositionAt(pixel - int2(1, 0));
    float3 down = PositionAt(pixel + int2(0, 1)) - position;
    float3 up = position - PositionAt(pixel - int2(0, 1));
    float3 dx = dot(right, right) < dot(left, left) ? right : left;
    float3 dy = dot(down, down) < dot(up, up) ? down : up;
    return normalize(cross(dy, dx));
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    Texture2D<float4> gbufferNormal = ResourceDescriptorHeap[g_frame.gbufferNormalView];
    float depth = DepthAt(pixel);
    if (flags.Load(int3(pixel, 0)).r < 0.5 || depth >= 1.0) {
        output[pixel] = 0.0;
        return;
    }
    float3 position = WorldPositionFromDepth(pixel, depth);
    float3 toEye = g_frame.eyePosition - position;
    float3 stored = gbufferNormal.Load(int3(pixel, 0)).xyz;
    float3 normal = dot(stored, stored) > 0.25 ? normalize(stored) : DepthNormal(pixel, position);
    // Two-sided materials report the front-face normal on back faces.
    if (dot(normal, toEye) < 0.0)
        normal = -normal;
    output[pixel] = float4(normal, length(toEye));
}
