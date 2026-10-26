#include "common.hlsli"

// One à-trous iteration (5x5 B3 spline kernel, taps `step` pixels apart) that only mixes samples lying on
// the same surface: weights fall off with the distance from this pixel's plane and with normal difference.
// input0: source; output0: filtered result.

static const float kKernel[3] = {3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0};

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    Texture2D<float4> source = ResourceDescriptorHeap[g_pass.input0];
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    float4 center = source.Load(int3(pixel, 0));
    if (surface.w <= 0.0) {
        output[pixel] = center;
        return;
    }
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    // Allowed distance from the plane grows with the footprint of the taps.
    float planeTolerance = 1.0 + surface.w * 0.004 * g_pass.step;
    float3 sum = 0.0;
    float weightSum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            int2 tap = pixel + int2(x, y) * int(g_pass.step);
            if (any(tap < 0) || any(tap >= int2(g_frame.size)))
                continue;
            float4 tapSurface = SurfaceAt(g_frame.surfaceView, tap);
            if (tapSurface.w <= 0.0)
                continue;
            float3 tapPosition = WorldPositionFromDepth(tap, DepthAt(tap));
            float plane = abs(dot(surface.xyz, tapPosition - position)) / planeTolerance;
            float weight = kKernel[abs(x)] * kKernel[abs(y)] * exp(-plane * plane) * pow(saturate(dot(surface.xyz, tapSurface.xyz)), 32.0);
            sum += source.Load(int3(tap, 0)).rgb * weight;
            weightSum += weight;
        }
    }
    output[pixel] = float4(weightSum > 0.0 ? sum / weightSum : center.rgb, center.a);
}
