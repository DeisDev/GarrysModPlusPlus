#include "post.hlsli"

// Local exposure (src/post_effects.cpp): the view's log2 luminance split into a base (large areas) and detail by a
// bilateral grid (Durand and Dorsey 2002; the grid of Chen, Paris and Durand 2007), as Unreal Engine 5's local exposure
// does: FinalMain (shaders/amd/lens.hlsl) slices it, scales the base's contrast around middle grey and adds the detail.
// Cells are 32 pixels square, with kBins bins of luminance each over the range localGridMinLog to localGridMinLog +
// 1 / localGridInvRangeLog stops.

static const uint kCell = 32;
static const uint kBins = 32;
// Fixed point of the luminance sums in groupshared memory: 1/1024 of a stop.
static const float kFixedPoint = 1024.0;

groupshared uint g_binSum[kBins];
groupshared uint g_binCount[kBins];
groupshared uint g_cellSum;
groupshared uint g_cellCount;

// The view's light as the tone curve will see it before local exposure: with the HDR brightness boost and grading's
// exposure.
float ExposedLuminance(float3 color)
{
    float gain = 1.0;
    if (g_post.exposureView != kNoView) {
        RWStructuredBuffer<float4> exposure = ResourceDescriptorHeap[g_post.exposureView];
        float4 state = exposure[0];
        gain = state.y != 0.0 ? state.x : 1.0;
    }
    return Luminance(max(color, 0.0)) * gain * g_post.gradeExposure;
}

// input0: the view; output0: the grid (per cell and bin: the sum of log2 luminance above the range's start and the
// count, both over the cell's 1024 pixels); output1: each cell's mean log2 luminance. One group per cell.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void GridMain(uint3 cell : SV_GroupID, uint2 thread : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    if (index < kBins) {
        g_binSum[index] = 0;
        g_binCount[index] = 0;
    }
    if (index == 0) {
        g_cellSum = 0;
        g_cellCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    Texture2D<float4> source = ResourceDescriptorHeap[g_postPass.input0];
    float range = 1.0 / g_post.localGridInvRangeLog;
    [unroll]
    for (uint i = 0; i < 4; ++i) {
        uint2 pixel = cell.xy * kCell + thread * 2 + uint2(i & 1, i >> 1);
        if (any(pixel >= g_post.size))
            continue;
        float stops = log2(max(ExposedLuminance(source.Load(int3(pixel, 0)).rgb), 1e-6)) - g_post.localGridMinLog;
        stops = clamp(stops, 0.0, range);
        uint bin = min(uint(stops * g_post.localGridInvRangeLog * float(kBins)), kBins - 1);
        uint fixedStops = uint(stops * kFixedPoint + 0.5);
        InterlockedAdd(g_binSum[bin], fixedStops);
        InterlockedAdd(g_binCount[bin], 1);
        InterlockedAdd(g_cellSum, fixedStops);
        InterlockedAdd(g_cellCount, 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < kBins) {
        RWTexture3D<float4> grid = ResourceDescriptorHeap[g_postPass.output0];
        float pixels = float(kCell * kCell);
        grid[uint3(cell.xy, index)] = float4(float(g_binSum[index]) / (kFixedPoint * pixels), float(g_binCount[index]) / pixels, 0.0, 0.0);
    }
    if (index == 0) {
        RWTexture2D<float4> mean = ResourceDescriptorHeap[g_postPass.output1];
        float stops = g_cellCount != 0 ? float(g_cellSum) / (kFixedPoint * float(g_cellCount)) : log2(0.18) - g_post.localGridMinLog;
        mean[cell.xy] = float4(stops + g_post.localGridMinLog, 0.0, 0.0, 0.0);
    }
}

// input0: the grid; output0: the grid blurred by a Gaussian of a cell across and a bin of luminance (5 taps each way).
// Sums and counts blur alike, so their ratio stays a weighted mean wherever the kernel leaves the grid.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(4, 4, 4)]
void BlurGridMain(uint3 cell : SV_DispatchThreadID)
{
    Texture3D<float4> grid = ResourceDescriptorHeap[g_postPass.input0];
    RWTexture3D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    uint3 size;
    output.GetDimensions(size.x, size.y, size.z);
    if (any(cell >= size))
        return;
    float2 sum = 0.0;
    [loop]
    for (int z = -2; z <= 2; ++z) {
        [loop]
        for (int y = -2; y <= 2; ++y) {
            [loop]
            for (int x = -2; x <= 2; ++x) {
                int3 tap = int3(cell) + int3(x, y, z);
                if (any(tap < 0) || any(tap >= int3(size)))
                    continue;
                sum += grid.Load(int4(tap, 0)).xy * exp(-0.5 * float(x * x + y * y + z * z));
            }
        }
    }
    output[cell] = float4(sum, 0.0, 0.0);
}

// input0: the cells' mean log2 luminance; output0: it blurred along x (step 1) or y (step 2) by a Gaussian of
// localBlurSigma cells, normalised over the taps inside the grid.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void BlurMeanMain(uint2 cell : SV_DispatchThreadID)
{
    Texture2D<float4> mean = ResourceDescriptorHeap[g_postPass.input0];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    uint2 size;
    output.GetDimensions(size.x, size.y);
    if (any(cell >= size))
        return;
    float sigma = g_post.localBlurSigma;
    if (sigma < 0.05) {
        output[cell] = mean.Load(int3(cell, 0));
        return;
    }
    int2 axis = g_postPass.step == 1 ? int2(1, 0) : int2(0, 1);
    int extent = int(dot(float2(size), float2(axis)));
    int radius = min(int(ceil(3.0 * sigma)), extent);
    float sum = 0.0;
    float weights = 0.0;
    [loop]
    for (int i = -radius; i <= radius; ++i) {
        int2 tap = int2(cell) + axis * i;
        if (any(tap < 0) || any(tap >= int2(size)))
            continue;
        float weight = exp(-0.5 * float(i * i) / (sigma * sigma));
        sum += mean.Load(int3(tap, 0)).x * weight;
        weights += weight;
    }
    output[cell] = float4(sum / max(weights, 1e-6), 0.0, 0.0, 0.0);
}
