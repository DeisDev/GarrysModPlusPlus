#include "post.hlsli"

// Bloom (src/post_effects.cpp): light scattered in the lens and the eye, as Jimenez's "Next Generation Post Processing
// in Call of Duty: Advanced Warfare" (SIGGRAPH 2014) builds it: a chain of half-size levels, each from the one above by a
// 13-tap filter, then from the smallest up, each level the weighted sum of its own and the tent-filtered level below.
// Every filter keeps its input's sum and the levels' weights add up to 1, so the bloom is the view convolved with a
// kernel that integrates to 1: what the lens pass mixes in (bloomScatter of it) only moves light. No Karis average:
// it would take light away from bright pixels.

float LevelWeight(uint level)
{
    return level < 4 ? g_post.bloomWeights[level] : g_post.bloomWeightsHigh[level - 4];
}

// input0: the level above (the view for level 0), output0: this level, half its size. step: the level.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void DownMain(uint2 pixel : SV_DispatchThreadID)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    uint2 size;
    output.GetDimensions(size.x, size.y);
    if (any(pixel >= size))
        return;
    Texture2D<float4> source = ResourceDescriptorHeap[g_postPass.input0];
    uint2 sourceSize;
    source.GetDimensions(sourceSize.x, sourceSize.y);
    float2 texel = 1.0 / float2(sourceSize);
    float2 uv = (float2(pixel) + 0.5) / float2(size);
    // Bilinear taps two source texels apart around the centre and one apart inside: the inner box weighs half, the
    // centre, the edge middles and the corners of the outer box an eighth, a quarter and an eighth.
    float3 a = source.SampleLevel(g_postLinear, uv + texel * float2(-2.0, -2.0), 0).rgb;
    float3 b = source.SampleLevel(g_postLinear, uv + texel * float2(0.0, -2.0), 0).rgb;
    float3 c = source.SampleLevel(g_postLinear, uv + texel * float2(2.0, -2.0), 0).rgb;
    float3 d = source.SampleLevel(g_postLinear, uv + texel * float2(-1.0, -1.0), 0).rgb;
    float3 e = source.SampleLevel(g_postLinear, uv + texel * float2(1.0, -1.0), 0).rgb;
    float3 f = source.SampleLevel(g_postLinear, uv + texel * float2(-2.0, 0.0), 0).rgb;
    float3 g = source.SampleLevel(g_postLinear, uv, 0).rgb;
    float3 h = source.SampleLevel(g_postLinear, uv + texel * float2(2.0, 0.0), 0).rgb;
    float3 i = source.SampleLevel(g_postLinear, uv + texel * float2(-1.0, 1.0), 0).rgb;
    float3 j = source.SampleLevel(g_postLinear, uv + texel * float2(1.0, 1.0), 0).rgb;
    float3 k = source.SampleLevel(g_postLinear, uv + texel * float2(-2.0, 2.0), 0).rgb;
    float3 l = source.SampleLevel(g_postLinear, uv + texel * float2(0.0, 2.0), 0).rgb;
    float3 m = source.SampleLevel(g_postLinear, uv + texel * float2(2.0, 2.0), 0).rgb;
    float3 sum = (d + e + i + j) * 0.125 + g * 0.125 + (b + f + h + l) * 0.0625 + (a + c + k + m) * 0.03125;
    output[pixel] = float4(max(sum, 0.0), 1.0);
}

// input0: this level of the down chain, input1: the level below's sum (kNoView for the smallest), output0: this level's
// sum. step: the level.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void UpMain(uint2 pixel : SV_DispatchThreadID)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    uint2 size;
    output.GetDimensions(size.x, size.y);
    if (any(pixel >= size))
        return;
    Texture2D<float4> level = ResourceDescriptorHeap[g_postPass.input0];
    float3 sum = level.Load(int3(pixel, 0)).rgb * LevelWeight(g_postPass.step);
    if (g_postPass.input1 != kNoView) {
        Texture2D<float4> below = ResourceDescriptorHeap[g_postPass.input1];
        uint2 belowSize;
        below.GetDimensions(belowSize.x, belowSize.y);
        float2 texel = 1.0 / float2(belowSize);
        float2 uv = (float2(pixel) + 0.5) / float2(size);
        // A 3x3 tent (1 2 1) of bilinear taps a texel of the level below apart.
        float3 tent = below.SampleLevel(g_postLinear, uv, 0).rgb * 4.0;
        tent += (below.SampleLevel(g_postLinear, uv + float2(texel.x, 0.0), 0).rgb + below.SampleLevel(g_postLinear, uv - float2(texel.x, 0.0), 0).rgb +
                    below.SampleLevel(g_postLinear, uv + float2(0.0, texel.y), 0).rgb + below.SampleLevel(g_postLinear, uv - float2(0.0, texel.y), 0).rgb) *
            2.0;
        tent += below.SampleLevel(g_postLinear, uv + texel, 0).rgb + below.SampleLevel(g_postLinear, uv - texel, 0).rgb +
            below.SampleLevel(g_postLinear, uv + float2(texel.x, -texel.y), 0).rgb + below.SampleLevel(g_postLinear, uv + float2(-texel.x, texel.y), 0).rgb;
        sum += tent * (1.0 / 16.0);
    }
    output[pixel] = float4(sum, 1.0);
}
