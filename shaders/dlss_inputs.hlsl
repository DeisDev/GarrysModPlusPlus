#include "common.hlsli"

// Render-resolution inputs for DLSS Ray Reconstruction, taken from the native-resolution G-buffer: each render
// pixel uses the native pixel under its centre, and the depth at the centre itself.
// input0, input1: render width and height; output0: first of seven consecutive UAVs: colour, depth, normal,
// flags, light, albedo, self-illumination.

// Depth at a point between native pixel centres (`centre` in native pixels). The native pixel's own depth would
// put the point on its surface's plane up to half a pixel away, which by a crease lies behind the other surface.
// Depth is linear across a plane in screen space, so blending the four native pixels around the point is exact on
// one surface and stays in front of both at a crease; across a silhouette the nearest pixel is kept.
float DepthAtPoint(float2 centre, int2 nearest)
{
    int2 base = int2(floor(centre - 0.5));
    float2 blend = centre - 0.5 - float2(base);
    float4 depths = float4(DepthAt(base), DepthAt(base + int2(1, 0)), DepthAt(base + int2(0, 1)), DepthAt(base + int2(1, 1)));
    float4 distances = float4(length(WorldPositionFromDepth(base, depths.x) - g_frame.eyePosition),
        length(WorldPositionFromDepth(base + int2(1, 0), depths.y) - g_frame.eyePosition),
        length(WorldPositionFromDepth(base + int2(0, 1), depths.z) - g_frame.eyePosition),
        length(WorldPositionFromDepth(base + int2(1, 1), depths.w) - g_frame.eyePosition));
    float nearestDistance = min(min(distances.x, distances.y), min(distances.z, distances.w));
    float farthestDistance = max(max(distances.x, distances.y), max(distances.z, distances.w));
    if (any(depths >= 1.0) || farthestDistance > nearestDistance * 1.05 + 1.0)
        return DepthAt(nearest);
    return lerp(lerp(depths.x, depths.y, blend.x), lerp(depths.z, depths.w, blend.x), blend.y);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 renderSize = uint2(g_pass.input0, g_pass.input1);
    if (any(id.xy >= renderSize))
        return;
    int2 pixel = int2(id.xy);
    float2 centre = (float2(pixel) + 0.5) * float2(g_frame.size) / float2(renderSize);
    int2 source = min(int2(centre), int2(g_frame.size) - 1);
    int3 load = int3(source, 0);

    Texture2D<float4> color = ResourceDescriptorHeap[g_frame.colorView];
    Texture2D<float4> normal = ResourceDescriptorHeap[g_frame.gbufferNormalView];
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    Texture2D<float4> light = ResourceDescriptorHeap[g_frame.lightView];
    Texture2D<float4> albedo = ResourceDescriptorHeap[g_frame.albedoView];
    Texture2D<float4> emissive = ResourceDescriptorHeap[g_frame.emissiveView];
    float depth = DepthAtPoint(centre, source);
    float4 coverage = flags.Load(load);

    RWTexture2D<float4> colorOut = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<float> depthOut = ResourceDescriptorHeap[g_pass.output0 + 1];
    RWTexture2D<float4> normalOut = ResourceDescriptorHeap[g_pass.output0 + 2];
    RWTexture2D<float4> flagsOut = ResourceDescriptorHeap[g_pass.output0 + 3];
    RWTexture2D<float4> lightOut = ResourceDescriptorHeap[g_pass.output0 + 4];
    RWTexture2D<float4> albedoOut = ResourceDescriptorHeap[g_pass.output0 + 5];
    RWTexture2D<float4> emissiveOut = ResourceDescriptorHeap[g_pass.output0 + 6];
    float4 colour = color.Load(load);
    colorOut[pixel] = colour;
    depthOut[pixel] = depth;
    normalOut[pixel] = normal.Load(load);
    flagsOut[pixel] = coverage;
    lightOut[pixel] = light.Load(load);
    // Water (flags g without r) keeps its refraction (shaders/water.hlsli), which is also the colour Ray Reconstruction
    // should keep sharp. Mirrors (flags b alone) keep a neutral albedo under their specular guides. Other pixels without
    // a surface (the sky, the 3D skybox, the game's own shading) are drawn lit and noise-free: their colour is their
    // albedo, so Ray Reconstruction keeps their texture instead of smoothing it as noisy light.
    float4 guide = float4(saturate(Decode(colour.rgb)), 1.0);
    if (coverage.r > 0.0 || coverage.g > 0.5)
        guide = albedo.Load(load);
    else if (coverage.b > 0.5)
        guide = float4(0.5, 0.5, 0.5, 1.0);
    albedoOut[pixel] = guide;
    emissiveOut[pixel] = emissive.Load(load);
}
