#include "common.hlsli"

// The upscaler's output (Ray Reconstruction's or FSR's) for the composite, with the game's additive passes added
// (AdditiveAfterResolve). Where a pixel and the eight around it show the open 2D sky,
// the sky as the game drew it at the output's resolution stands in: the upscaler takes the sky at its render
// resolution, sampled at pixel centres, and smoothed the clouds' detail away as if it were noisy light (2026-11-03).
// Silhouettes against the sky keep the upscaler's anti-aliased edges. input0: the upscaler's output;
// input1, input2: the jitter's image shift in output pixels (float bits); input3: the 3D skybox's distance per pixel
// (0 where it drew nothing), or kNoView; output0: linear light.

// Whether the main view's depth and the 3D skybox leave the pixels around `pixel` empty.
bool OpenSky(int2 pixel)
{
    Texture2D<float> skybox = ResourceDescriptorHeap[g_pass.input3 != kNoView ? g_pass.input3 : g_frame.depthView];
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            int2 at = clamp(pixel + int2(x, y), int2(0, 0), int2(g_frame.size) - 1);
            if (DepthAt(at) < 1.0 || (g_pass.input3 != kNoView && skybox.Load(int3(at, 0)) > 0.0))
                return false;
        }
    }
    return true;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    float2 shift = float2(asfloat(g_pass.input1), asfloat(g_pass.input2));
    if (OpenSky(int2(id.xy))) {
        // Read where the jitter moved the sky this frame, so it holds still.
        Texture2D<float4> drawn = ResourceDescriptorHeap[g_frame.colorView];
        output[id.xy] = float4(drawn.SampleLevel(g_linearClamp, (float2(id.xy) + 0.5 + shift) * g_frame.invSize, 0.0).rgb, 1.0);
        return;
    }
    Texture2D<float4> source = ResourceDescriptorHeap[g_pass.input0];
    float3 color = source.Load(int3(id.xy, 0)).rgb;
    output[id.xy] = float4(color + AdditiveAfterResolve(int2(id.xy), shift), 1.0);
}
