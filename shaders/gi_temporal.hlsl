#include "common.hlsli"

// Accumulates the raw indirect estimate over frames. Last frame's history is reprojected with bilinear taps
// that are each rejected when the surface there differs (distance or normal).
// input0: raw estimate; input1: last frame's history (rgb irradiance, a sample count); output0: new history.

static const float kMaxSamples = 32.0;

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    Texture2D<float4> raw = ResourceDescriptorHeap[g_pass.input0];
    Texture2D<float4> history = ResourceDescriptorHeap[g_pass.input1];
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    if (surface.w <= 0.0) {
        output[pixel] = 0.0;
        return;
    }
    float3 current = raw.Load(int3(pixel, 0)).rgb;
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));

    float4 accumulated = 0.0;
    float weightSum = 0.0;
    if (g_frame.historyValid != 0) {
        float4 clip = mul(g_frame.prevViewProj, float4(position, 1.0));
        float2 previous = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) * float2(g_frame.size) - 0.5;
        int2 base = int2(floor(previous));
        float2 f = previous - float2(base);
        float distance = length(position - g_frame.prevEyePosition);
        [unroll]
        for (int i = 0; i < 4; ++i) {
            int2 tap = base + int2(i & 1, i >> 1);
            if (clip.w <= 0.0 || any(tap < 0) || any(tap >= int2(g_frame.size)))
                continue;
            float4 previousSurface = SurfaceAt(g_frame.prevSurfaceView, tap);
            if (previousSurface.w <= 0.0 || abs(previousSurface.w - distance) > 2.0 + distance * 0.01 || dot(previousSurface.xyz, surface.xyz) < 0.9)
                continue;
            float weight = (i & 1 ? f.x : 1.0 - f.x) * (i >> 1 ? f.y : 1.0 - f.y);
            accumulated += history.Load(int3(tap, 0)) * weight;
            weightSum += weight;
        }
    }
    if (weightSum < 1e-3) {
        output[pixel] = float4(current, 1.0);
        return;
    }
    accumulated /= weightSum;
    float samples = min(accumulated.a + 1.0, kMaxSamples);
    output[pixel] = float4(lerp(accumulated.rgb, current, 1.0 / samples), samples);
}
