#include "common.hlsli"
#include "cube.hlsli"

// The sky's light for indirect rays (SkyLight in shaders/common.hlsli): the 2D sky (input0, the faces of
// src/shaderapi_capture.cpp's cube of what the game drew, or of the procedural sky that replaces it: radiance before
// the tonemap scale, -1 where nothing was drawn) averaged down to
// kSize x kSize texels a face, then scaled so that a surface facing up under open sky gets the map's sky light
// (g_frame.skyColor), as it did from a uniform sky. Directions the game has not drawn take the sky light itself. With
// clouds (input1, the faces of their cube in the procedural sky's size, or kNoView) the sky is seen through them; the
// map's sky light already holds how much they change its brightness (Lighting::FillConstants).
// output0: the kSize x kSize x 6 cube.
// Step 0: a group per output texel averages its block of the drawn sky. Step 1: one group scales the cube.

static const uint kSize = 8;
static const uint kThreads = 64;
static const uint kTexelsPerThread = kSize * kSize * 6 / kThreads;

groupshared float4 g_sums[kThreads];

float4 GroupSum(float4 value, uint index)
{
    g_sums[index] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = kThreads / 2; stride > 0; stride /= 2) {
        if (index < stride)
            g_sums[index] += g_sums[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    return g_sums[0];
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    RWTexture2DArray<float4> output = ResourceDescriptorHeap[g_pass.output0];
    if (g_pass.step == 0) {
        Texture2DArray<float4> sky = ResourceDescriptorHeap[g_pass.input0];
        uint width, height, faces;
        sky.GetDimensions(width, height, faces);
        uint block = max(width / kSize, 1u);
        uint span = (block + 7) / 8;
        uint2 first = thread.xy * span;
        float4 sum = 0.0;
        for (uint y = first.y; y < min(first.y + span, block); ++y) {
            for (uint x = first.x; x < min(first.x + span, block); ++x) {
                int4 texel = int4(group.xy * block + uint2(x, y), group.z, 0);
                float3 radiance = sky.Load(texel).rgb;
                if (!all(radiance >= 0.0))
                    continue;
                if (g_pass.input1 != kNoView) {
                    Texture2DArray<float4> clouds = ResourceDescriptorHeap[g_pass.input1];
                    float4 seen = clouds.Load(texel);
                    radiance = radiance * seen.a + seen.rgb;
                }
                sum += float4(radiance, 1.0);
            }
        }
        sum = GroupSum(sum, index);
        if (index == 0)
            output[group] = float4(sum.w > 0.0 ? sum.rgb / sum.w : -1.0, 1.0);
        return;
    }

    // The mean radiance over the upper hemisphere, weighted by the cosine to straight up and each texel's solid angle:
    // the uniform sky that would light a surface facing up as much.
    float3 values[kTexelsPerThread];
    float4 weighted = 0.0;
    for (uint i = 0; i < kTexelsPerThread; ++i) {
        uint texel = index + i * kThreads;
        uint3 position = uint3(texel % kSize, (texel / kSize) % kSize, texel / (kSize * kSize));
        values[i] = output[position].rgb;
        if (values[i].x < 0.0)
            continue;
        float3 direction = FaceDirection(position.z, (float2(position.xy) + 0.5) * (2.0 / kSize) - 1.0);
        float squared = dot(direction, direction);
        float weight = max(direction.z, 0.0) / (squared * squared);
        weighted += float4(values[i] * weight, weight);
    }
    weighted = GroupSum(weighted, index);
    float3 mean = weighted.w > 0.0 ? weighted.rgb / weighted.w : 0.0;
    float scale = Luminance(mean) > 1e-6 ? Luminance(g_frame.skyColor) / Luminance(mean) : -1.0;
    for (uint j = 0; j < kTexelsPerThread; ++j) {
        uint texel = index + j * kThreads;
        uint3 position = uint3(texel % kSize, (texel / kSize) % kSize, texel / (kSize * kSize));
        output[position] = float4(scale >= 0.0 && values[j].x >= 0.0 ? values[j] * scale : g_frame.skyColor, 1.0);
    }
}
