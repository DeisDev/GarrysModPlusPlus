#include "common.hlsli"

// Accumulates reflections over frames. A mirror image moves with the reflected point, not with the mirror, so
// the history is reprojected from the virtual point behind the mirror (the eye ray continued by the hit
// distance) and clipped to the spread of the current 3x3 neighbourhood in YCoCg, which rejects stale history.
// input0: this frame's reflections (rgb, hit distance; 0 for pixels without a reflection); input1: last
// frame's accumulated reflections (rgb, sample count); output0: new accumulation; step: 1 when the history may
// be used.

static const float kMaxSamples = 16.0;

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
    float4 center = current.Load(int3(pixel, 0));
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    if (surface.w <= 0.0 || g_pass.step == 0 || g_frame.historyValid == 0) {
        output[pixel] = float4(center.rgb, 1.0);
        return;
    }
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    float3 view = normalize(position - g_frame.eyePosition);
    float3 reprojected = center.a > 0.0 ? g_frame.eyePosition + view * (surface.w + center.a) : position;
    float4 clip = mul(g_frame.prevViewProj, float4(reprojected, 1.0));
    float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    // The mirror itself must have been on screen at a matching distance.
    Texture2D<float2> motion = ResourceDescriptorHeap[g_frame.motionView];
    Texture2D<float> previousDistance = ResourceDescriptorHeap[g_frame.previousDistanceView];
    int2 surfacePixel = int2(floor(float2(pixel) + 0.5 + motion.Load(int3(pixel, 0))));
    bool valid = clip.w > 0.0 && all(uv >= 0.0) && all(uv <= 1.0) && all(surfacePixel >= 0) && all(surfacePixel < int2(g_frame.size));
    if (valid) {
        float4 previousSurface = SurfaceAt(g_frame.prevSurfaceView, surfacePixel);
        float distance = previousDistance.Load(int3(pixel, 0));
        valid = previousSurface.w > 0.0 && abs(previousSurface.w - distance) <= 4.0 + distance * 0.02;
    }
    if (!valid) {
        output[pixel] = float4(center.rgb, 1.0);
        return;
    }
    float3 moment1 = 0.0;
    float3 moment2 = 0.0;
    int2 last = int2(g_frame.size) - 1;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float3 color = RgbToYCoCg(current.Load(int3(clamp(pixel + int2(x, y), 0, last), 0)).rgb);
            moment1 += color;
            moment2 += color * color;
        }
    }
    float3 mean = moment1 / 9.0;
    float3 deviation = sqrt(max(moment2 / 9.0 - mean * mean, 0.0));
    float4 previous = history.SampleLevel(g_linearClamp, uv, 0.0);
    float3 clipped = YCoCgToRgb(clamp(RgbToYCoCg(previous.rgb), mean - 1.5 * deviation, mean + 1.5 * deviation));
    float samples = min(previous.a + 1.0, kMaxSamples);
    output[pixel] = float4(lerp(clipped, center.rgb, 1.0 / samples), samples);
}
