#include "common.hlsli"

// Restores detail the temporal resolve softens: an unsharp mask over the four direct neighbours, limited to
// their colour range so edges do not ring, then adds the game's additive passes (AdditiveAfterResolve).
// input0: resolved history (linear); input1, input2: the jitter's image shift in pixels (float bits); output0: final
// colour (encoded).

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    Texture2D<float4> resolved = ResourceDescriptorHeap[g_pass.input0];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    int2 last = int2(g_frame.size) - 1;
    float3 center = resolved.Load(int3(pixel, 0)).rgb;
    float3 up = resolved.Load(int3(clamp(pixel + int2(0, -1), 0, last), 0)).rgb;
    float3 down = resolved.Load(int3(clamp(pixel + int2(0, 1), 0, last), 0)).rgb;
    float3 left = resolved.Load(int3(clamp(pixel + int2(-1, 0), 0, last), 0)).rgb;
    float3 right = resolved.Load(int3(clamp(pixel + int2(1, 0), 0, last), 0)).rgb;
    float3 low = min(center, min(min(up, down), min(left, right)));
    float3 high = max(center, max(max(up, down), max(left, right)));
    float3 sharpened = center + (center - (up + down + left + right) * 0.25) * (g_frame.sharpening * 2.0);
    float3 additive = AdditiveAfterResolve(pixel, float2(asfloat(g_pass.input1), asfloat(g_pass.input2)));
    output[pixel] = float4(Encode(clamp(sharpened, low, high) + additive), 1.0);
}
