#include "common.hlsli"

// Render-resolution inputs for DLSS Ray Reconstruction, taken from the native-resolution G-buffer: each render
// pixel uses the native pixel under its centre. Also writes camera motion vectors (render pixels, towards the
// previous frame, without jitter).
// input0, input1: render width and height; output0: first of eight consecutive UAVs: colour, depth, normal,
// flags, light, albedo, self-illumination, motion.

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 renderSize = uint2(g_pass.input0, g_pass.input1);
    if (any(id.xy >= renderSize))
        return;
    int2 pixel = int2(id.xy);
    int2 source = min(int2((float2(pixel) + 0.5) * float2(g_frame.size) / float2(renderSize)), int2(g_frame.size) - 1);
    int3 load = int3(source, 0);

    Texture2D<float4> color = ResourceDescriptorHeap[g_frame.colorView];
    Texture2D<float4> normal = ResourceDescriptorHeap[g_frame.gbufferNormalView];
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    Texture2D<float4> light = ResourceDescriptorHeap[g_frame.lightView];
    Texture2D<float4> albedo = ResourceDescriptorHeap[g_frame.albedoView];
    Texture2D<float4> emissive = ResourceDescriptorHeap[g_frame.emissiveView];
    float depth = DepthAt(source);
    float4 coverage = flags.Load(load);

    RWTexture2D<float4> colorOut = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<float> depthOut = ResourceDescriptorHeap[g_pass.output0 + 1];
    RWTexture2D<float4> normalOut = ResourceDescriptorHeap[g_pass.output0 + 2];
    RWTexture2D<float4> flagsOut = ResourceDescriptorHeap[g_pass.output0 + 3];
    RWTexture2D<float4> lightOut = ResourceDescriptorHeap[g_pass.output0 + 4];
    RWTexture2D<float4> albedoOut = ResourceDescriptorHeap[g_pass.output0 + 5];
    RWTexture2D<float4> emissiveOut = ResourceDescriptorHeap[g_pass.output0 + 6];
    RWTexture2D<float2> motionOut = ResourceDescriptorHeap[g_pass.output0 + 7];
    colorOut[pixel] = color.Load(load);
    depthOut[pixel] = depth;
    normalOut[pixel] = normal.Load(load);
    flagsOut[pixel] = coverage;
    lightOut[pixel] = light.Load(load);
    // Ray Reconstruction expects a neutral albedo where there is no surface.
    albedoOut[pixel] = coverage.r > 0.0 ? albedo.Load(load) : float4(0.5, 0.5, 0.5, 1.0);
    emissiveOut[pixel] = emissive.Load(load);

    float2 motion = 0.0;
    if (g_frame.historyValid != 0) {
        float3 position = WorldPositionFromDepth(source, depth);
        float4 current = mul(g_frame.viewProj, float4(position, 1.0));
        float4 previous = mul(g_frame.prevViewProj, float4(position, 1.0));
        if (current.w > 0.0 && previous.w > 0.0) {
            float2 currentUv = float2(current.x / current.w * 0.5 + 0.5, 0.5 - current.y / current.w * 0.5);
            float2 previousUv = float2(previous.x / previous.w * 0.5 + 0.5, 0.5 - previous.y / previous.w * 0.5);
            motion = (previousUv - currentUv) * float2(renderSize);
        }
    }
    motionOut[pixel] = motion;
}
