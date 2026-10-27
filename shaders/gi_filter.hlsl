#include "common.hlsli"

// One à-trous iteration (5x5 B3 spline kernel, taps `step` pixels apart) of the light denoiser, guided by the
// variance of the light's luminance as in SVGF (Schied et al., 2017): weights fall off with the distance from
// this pixel's plane, with normal difference and with luminance difference measured against the expected noise.
// Each iteration passes on the variance of its own result.
// input0: source (rgb; a: luminance variance, or the accumulated sample count on the first iteration);
// input1: luminance moments of the accumulated history on the first iteration, or none; input2: 1 on the first
// iteration; output0: filtered rgb and luminance variance.

static const float kKernel[3] = {3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0};
static const float kLuminanceSigma = 4.0;
// Fewer accumulated samples than this give no usable moments; the neighbourhood's spread stands in for them.
static const float kMomentSamples = 4.0;

float SourceVariance(Texture2D<float4> source, int2 pixel, float spatialVariance)
{
    float4 value = source.Load(int3(pixel, 0));
    if (g_pass.input2 == 0)
        return value.a;
    if (g_pass.input1 == kNoView || value.a < kMomentSamples)
        return spatialVariance;
    Texture2D<float2> moments = ResourceDescriptorHeap[g_pass.input1];
    float2 m = moments.Load(int3(pixel, 0));
    return max(m.y - m.x * m.x, 0.0);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    int2 last = int2(g_frame.size) - 1;
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    Texture2D<float4> source = ResourceDescriptorHeap[g_pass.input0];
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    float4 center = source.Load(int3(pixel, 0));
    if (surface.w <= 0.0) {
        output[pixel] = float4(center.rgb, 0.0);
        return;
    }

    float spatialVariance = 0.0;
    if (g_pass.input2 != 0) {
        float2 m = 0.0;
        float count = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                int2 tap = clamp(pixel + int2(x, y), 0, last);
                if (SurfaceAt(g_frame.surfaceView, tap).w <= 0.0)
                    continue;
                float l = Luminance(source.Load(int3(tap, 0)).rgb);
                m += float2(l, l * l);
                count += 1.0;
            }
        }
        m /= max(count, 1.0);
        spatialVariance = max(m.y - m.x * m.x, 0.0);
    }
    // The edge-stopping variance is blurred over 3x3 so single noisy pixels do not decide it.
    float variance = 0.0;
    float varianceWeight = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            int2 tap = clamp(pixel + int2(x, y), 0, last);
            float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            variance += SourceVariance(source, tap, spatialVariance) * w;
            varianceWeight += w;
        }
    }
    variance /= varianceWeight;
    float luminanceScale = 1.0 / (kLuminanceSigma * sqrt(variance) + 1e-4);
    float centerLuminance = Luminance(center.rgb);

    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    // Allowed distance from the plane grows with the footprint of the taps.
    float planeTolerance = 1.0 + surface.w * 0.004 * g_pass.step;
    float3 sum = 0.0;
    float weightSum = 0.0;
    float varianceSum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            int2 tap = pixel + int2(x, y) * int(g_pass.step);
            if (any(tap < 0) || any(tap > last))
                continue;
            float4 tapSurface = SurfaceAt(g_frame.surfaceView, tap);
            if (tapSurface.w <= 0.0)
                continue;
            float3 tapPosition = WorldPositionFromDepth(tap, DepthAt(tap));
            float3 color = source.Load(int3(tap, 0)).rgb;
            float plane = abs(dot(surface.xyz, tapPosition - position)) / planeTolerance;
            float weight = kKernel[abs(x)] * kKernel[abs(y)] * exp(-plane * plane) * pow(saturate(dot(surface.xyz, tapSurface.xyz)), 32.0) *
                exp(-abs(Luminance(color) - centerLuminance) * luminanceScale);
            sum += color * weight;
            weightSum += weight;
            varianceSum += weight * weight * SourceVariance(source, tap, spatialVariance);
        }
    }
    if (weightSum <= 0.0) {
        output[pixel] = float4(center.rgb, variance);
        return;
    }
    output[pixel] = float4(sum / weightSum, varianceSum / (weightSum * weightSum));
}
