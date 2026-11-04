#include "common.hlsli"

// Temporal anti-aliasing resolve. The scene is rendered with a sub-pixel jitter that changes every frame; the
// history is reprojected with the motion vectors (shaders/primary.hlsl) and clipped to the colour range of the current 3x3
// neighbourhood (in YCoCg) before blending, which rejects most ghosting from motion and disocclusion.
// input0: shaded linear light; input1: last frame's history; output0: new history; step: 1 when the history may be
// used.

float3 RgbToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 YCoCgToRgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    Texture2D<float4> current = ResourceDescriptorHeap[g_pass.input0];
    Texture2D<float4> history = ResourceDescriptorHeap[g_pass.input1];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];

    float3 center = 0.0;
    float3 moment1 = 0.0;
    float3 moment2 = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            int2 tap = clamp(pixel + int2(x, y), int2(0, 0), int2(g_frame.size) - 1);
            float3 color = RgbToYCoCg(current.Load(int3(tap, 0)).rgb);
            if (x == 0 && y == 0)
                center = color;
            moment1 += color;
            moment2 += color * color;
        }
    }
    if (g_pass.step == 0) {
        output[pixel] = float4(YCoCgToRgb(center), 1.0);
        return;
    }

    Texture2D<float2> motion = ResourceDescriptorHeap[g_frame.motionView];
    float2 uv = (float2(pixel) + 0.5 + motion.Load(int3(pixel, 0))) * g_frame.invSize;
    if (any(uv < 0.0) || any(uv > 1.0)) {
        output[pixel] = float4(YCoCgToRgb(center), 1.0);
        return;
    }
    float3 mean = moment1 / 9.0;
    float3 deviation = sqrt(max(moment2 / 9.0 - mean * mean, 0.0));
    float3 previous = RgbToYCoCg(history.SampleLevel(g_linearClamp, uv, 0.0).rgb);
    previous = clamp(previous, mean - deviation, mean + deviation);
    output[pixel] = float4(YCoCgToRgb(lerp(center, previous, g_frame.taaHistoryWeight)), 1.0);
}
