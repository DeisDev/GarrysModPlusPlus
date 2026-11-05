#include "common.hlsli"

// Accumulates the raw light estimate over frames, with the first two moments of its luminance for the denoiser's
// variance (gi_filter.hlsl). Last frame's history is found with the motion vectors (shaders/primary.hlsl) and read
// with bilinear taps that are each rejected when the surface there differs (distance or normal). Once a few
// samples are in, a sample far brighter than the history's spread is scaled down before it is blended in; the
// moments keep its true luminance, so lasting changes widen the spread and get through.
// The viewmodel moves with the eye, so its history is the same pixel's, updated in place; its record also keeps the
// distance and the tag of the frame that wrote it (step is this frame's), and history at another distance, or that
// last frame did not write, starts again.
// input0: raw estimate; input1: last frame's history (rgb, sample count); input2: last frame's moments;
// output0: new history; output1: new moments.
// Viewmodel: input0: raw estimate; output0: history (rgb, sample count); output1: record (moments, distance, tag).

static const float kMaxSamples = 32.0;
// The viewmodel's light turns with the eye, so its history is shorter.
static const float kViewmodelMaxSamples = 16.0;
// Mirrors kViewmodelTags in src/lighting.cpp: tags run from 1 to this.
static const uint kViewmodelTags = 1u << 24;
static const float kClampSamples = 4.0;
static const float kClampDeviations = 4.0;

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    id.xy = DispatchPixel(id.xy);
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    bool viewmodel = g_frame.viewmodel != 0;
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<float2> momentsOutput = ResourceDescriptorHeap[g_pass.output1];
    RWTexture2D<float4> record = ResourceDescriptorHeap[g_pass.output1];
    Texture2D<float4> raw = ResourceDescriptorHeap[g_pass.input0];
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    if (surface.w <= 0.0) {
        output[pixel] = 0.0;
        if (viewmodel)
            record[pixel] = 0.0;
        else
            momentsOutput[pixel] = 0.0;
        return;
    }
    float3 current = raw.Load(int3(pixel, 0)).rgb;
    float luminance = Luminance(current);

    float4 accumulated = 0.0;
    float2 moments = 0.0;
    float weightSum = 0.0;
    if (viewmodel) {
        float4 previous = record[pixel];
        float previousTag = float(g_pass.step > 1 ? g_pass.step - 1 : kViewmodelTags);
        if (previous.w == previousTag && abs(previous.z - surface.w) <= 0.5 + surface.w * 0.01) {
            accumulated = output[pixel];
            moments = previous.xy;
            weightSum = 1.0;
        }
    } else if (g_frame.historyValid != 0) {
        Texture2D<float4> history = ResourceDescriptorHeap[g_pass.input1];
        Texture2D<float2> historyMoments = ResourceDescriptorHeap[g_pass.input2];
        Texture2D<float2> motion = ResourceDescriptorHeap[g_frame.motionView];
        Texture2D<float> previousDistance = ResourceDescriptorHeap[g_frame.previousDistanceView];
        float2 previous = float2(pixel) + motion.Load(int3(pixel, 0));
        int2 base = int2(floor(previous));
        float2 f = previous - float2(base);
        float distance = previousDistance.Load(int3(pixel, 0));
        [unroll]
        for (int i = 0; i < 4; ++i) {
            int2 tap = base + int2(i & 1, i >> 1);
            if (any(tap < 0) || any(tap >= int2(g_frame.size)))
                continue;
            float4 previousSurface = SurfaceAt(g_frame.prevSurfaceView, tap);
            if (previousSurface.w <= 0.0 || abs(previousSurface.w - distance) > 2.0 + distance * 0.01 || dot(previousSurface.xyz, surface.xyz) < 0.9)
                continue;
            float weight = (i & 1 ? f.x : 1.0 - f.x) * (i >> 1 ? f.y : 1.0 - f.y);
            accumulated += history.Load(int3(tap, 0)) * weight;
            moments += historyMoments.Load(int3(tap, 0)) * weight;
            weightSum += weight;
        }
    }
    float4 result = float4(current, 1.0);
    float2 resultMoments = float2(luminance, luminance * luminance);
    if (weightSum >= 1e-3) {
        accumulated /= weightSum;
        moments /= weightSum;
        float samples = min(accumulated.a + 1.0, viewmodel ? kViewmodelMaxSamples : kMaxSamples);
        if (accumulated.a >= kClampSamples) {
            float limit = moments.x + kClampDeviations * sqrt(max(moments.y - moments.x * moments.x, 0.0));
            if (luminance > limit)
                current *= limit / luminance;
        }
        result = float4(lerp(accumulated.rgb, current, 1.0 / samples), samples);
        resultMoments = lerp(moments, resultMoments, 1.0 / samples);
    }
    output[pixel] = result;
    if (viewmodel)
        record[pixel] = float4(resultMoments, surface.w, float(g_pass.step));
    else
        momentsOutput[pixel] = resultMoments;
}
