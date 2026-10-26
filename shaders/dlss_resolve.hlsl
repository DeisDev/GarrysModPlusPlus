#include "common.hlsli"

// Encodes Ray Reconstruction's linear output the way the game encodes lit colour, for the composite.
// input0: Ray Reconstruction output; output0: encoded colour.

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    Texture2D<float4> source = ResourceDescriptorHeap[g_pass.input0];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    output[id.xy] = float4(Encode(source.Load(int3(id.xy, 0)).rgb), 1.0);
}
