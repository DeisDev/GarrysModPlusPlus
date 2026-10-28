#include "common.hlsli"

// Encodes Ray Reconstruction's linear output the way the game encodes lit colour, for the composite.
// The game's additive passes are added here (AdditiveAfterResolve). input0: Ray Reconstruction output; input1, input2:
// the jitter's image shift in output pixels (float bits); output0: encoded colour.

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    Texture2D<float4> source = ResourceDescriptorHeap[g_pass.input0];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    float3 additive = AdditiveAfterResolve(int2(id.xy), float2(asfloat(g_pass.input1), asfloat(g_pass.input2)));
    output[id.xy] = float4(Encode(source.Load(int3(id.xy, 0)).rgb + additive), 1.0);
}
