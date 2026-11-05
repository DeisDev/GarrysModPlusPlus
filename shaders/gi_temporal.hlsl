#include "common.hlsli"

// Accumulates the raw light estimate over frames, with the first two moments of its luminance for the denoiser's
// variance (gi_filter.hlsl). Last frame's history is found with the motion vectors (shaders/primary.hlsl) and read
// with bilinear taps that are each rejected when the surface there differs (distance or normal). Once a few
// samples are in, a sample far brighter than the history's spread is scaled down before it is blended in; the
// moments keep its true luminance, so lasting changes widen the spread and get through.
// The viewmodel's run keeps its own history, found along its motion (its vertices' positions last frame, shaders/
// primary.hlsl); its record also keeps the distance and the tag of the frame that wrote it (step is this frame's), and
// each tap at another distance or face normal (the normal map's normal differs between neighbouring texels on a bumpy
// weapon), or that last frame did not write (its passes cover a rectangle around it), is left out. The light around
// the viewmodel changes as the eye turns, so its history is also clipped to the spread of this frame's estimates around
// the pixel (Salvi 2016's variance clipping).
// input0: raw estimate; input1: last frame's history (rgb, sample count); input2: last frame's moments;
// output0: new history; output1: new moments.
// Viewmodel: input0: raw estimate; input1, input2, input3: last frame's history (rgb, sample count), record (moments,
// distance, tag) and face normals, taken while g_frame.historyValid; output0, output1, output2: this frame's, written
// only where this run's viewmodel is (elsewhere the tags keep older texels from being taken).

static const float kMaxSamples = 32.0;
// The viewmodel's light turns with the eye, so its history is shorter.
static const float kViewmodelMaxSamples = 16.0;
// How far the viewmodel's history may stray from the mean of this frame's estimates around it, in their deviations, and
// how many of its pixels the 3x3 around it must hold for that: fewer leave no spread, and the history collapsed to the
// frame's estimate along the edges of a detailed weapon.
static const float kViewmodelClipDeviations = 2.0;
static const float kViewmodelClipCount = 4.0;
// Mirrors kViewmodelTags in src/lighting.cpp: tags run from 1 to this.
static const uint kViewmodelTags = 1u << 24;
static const float kClampSamples = 4.0;
static const float kClampDeviations = 4.0;

// The face normal under a pixel (shaders/primary.hlsl).
float3 FaceNormalAt(int2 pixel)
{
    Texture2D<float4> normals = ResourceDescriptorHeap[g_frame.geometricNormalView];
    return normals.Load(int3(pixel, 0)).xyz;
}

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
    RWTexture2D<float4> normalOutput = ResourceDescriptorHeap[g_pass.output2];
    Texture2D<float4> raw = ResourceDescriptorHeap[g_pass.input0];
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    // A viewmodel captured in parts is lit once per part, each writing this frame's textures at its own pixels.
    if (surface.w <= 0.0) {
        if (!viewmodel) {
            output[pixel] = 0.0;
            momentsOutput[pixel] = 0.0;
        }
        return;
    }
    float3 current = raw.Load(int3(pixel, 0)).rgb;
    float luminance = Luminance(current);
    float3 faceNormal = viewmodel ? FaceNormalAt(pixel) : 0.0;

    float4 accumulated = 0.0;
    float2 moments = 0.0;
    float weightSum = 0.0;
    if (viewmodel && g_frame.historyValid != 0) {
        Texture2D<float4> history = ResourceDescriptorHeap[g_pass.input1];
        Texture2D<float4> records = ResourceDescriptorHeap[g_pass.input2];
        Texture2D<float4> normals = ResourceDescriptorHeap[g_pass.input3];
        Texture2D<float2> motion = ResourceDescriptorHeap[g_frame.motionView];
        Texture2D<float> previousDistance = ResourceDescriptorHeap[g_frame.previousDistanceView];
        float distance = previousDistance.Load(int3(pixel, 0));
        float previousTag = float(g_pass.step > 1 ? g_pass.step - 1 : kViewmodelTags);
        float2 previous = float2(pixel) + motion.Load(int3(pixel, 0));
        int2 base = int2(floor(previous));
        float2 f = previous - float2(base);
        [unroll]
        for (int i = 0; i < 4; ++i) {
            int2 tap = base + int2(i & 1, i >> 1);
            if (distance <= 0.0 || any(tap < 0) || any(tap >= int2(g_frame.size)))
                continue;
            float4 previousRecord = records.Load(int3(tap, 0));
            if (previousRecord.w != previousTag || abs(previousRecord.z - distance) > 0.5 + distance * 0.01 ||
                dot(normals.Load(int3(tap, 0)).xyz, faceNormal) < 0.9)
                continue;
            float weight = (i & 1 ? f.x : 1.0 - f.x) * (i >> 1 ? f.y : 1.0 - f.y);
            accumulated += history.Load(int3(tap, 0)) * weight;
            moments += previousRecord.xy * weight;
            weightSum += weight;
        }
        if (weightSum >= 1e-3) {
            // This frame's estimates on the viewmodel around the pixel (its parts at other distances, a rail or a sight,
            // share its light): their mean and deviation per colour.
            float3 sum = 0.0;
            float3 squares = 0.0;
            float count = 0.0;
            [unroll]
            for (int y = -1; y <= 1; ++y) {
                [unroll]
                for (int x = -1; x <= 1; ++x) {
                    int2 near = clamp(pixel + int2(x, y), int2(0, 0), int2(g_frame.size) - 1);
                    float4 nearSurface = SurfaceAt(g_frame.surfaceView, near);
                    if (nearSurface.w <= 0.0)
                        continue;
                    float3 value = raw.Load(int3(near, 0)).rgb;
                    sum += value;
                    squares += value * value;
                    count += 1.0;
                }
            }
            if (count >= kViewmodelClipCount) {
                float3 mean = sum / count;
                float3 deviation = sqrt(max(squares / count - mean * mean, 0.0));
                accumulated /= weightSum;
                accumulated.rgb = clamp(accumulated.rgb, mean - kViewmodelClipDeviations * deviation, mean + kViewmodelClipDeviations * deviation);
                accumulated *= weightSum;
            }
        }
    } else if (!viewmodel && g_frame.historyValid != 0) {
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
    if (viewmodel) {
        record[pixel] = float4(resultMoments, surface.w, float(g_pass.step));
        normalOutput[pixel] = float4(faceNormal, 0.0);
    } else {
        momentsOutput[pixel] = resultMoments;
    }
}
