// AMD FidelityFX Contrast Adaptive Sharpening (FidelityFX SDK 1.1.4, MIT), sharpening only, on the camera effects' linear
// colour (src/post_effects.cpp), which CAS needs. CAS limits its sharpening by the distance to a signal that ends at 1;
// the view ends at the display's peak (tonePeak paper whites, 1 in SDR), so it is sharpened in units of the peak, or
// nothing brighter than paper white would be sharpened in HDR.

#include "../post/post.hlsli"
#include "ffx_core.h"

FfxFloat32x3 casLoad(FfxInt32x2 position)
{
    Texture2D<float4> source = ResourceDescriptorHeap[g_postPass.input0];
    return source.Load(int3(clamp(position, 0, int2(g_post.size) - 1), 0)).rgb / g_post.tonePeak;
}

// The colour is linear already.
void casInput(inout FfxFloat32 red, inout FfxFloat32 green, inout FfxFloat32 blue)
{
}

#include "cas/ffx_cas.h"

// input0: the view (linear); output0: the sharpened view.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= g_post.size))
        return;
    FfxUInt32x4 const0;
    FfxUInt32x4 const1;
    const float2 size = float2(g_post.size);
    ffxCasSetup(const0, const1, g_post.sharpness, size.x, size.y, size.x, size.y);
    float3 color;
    ffxCasFilter(color.r, color.g, color.b, pixel, const0, const1, true);
    Texture2D<float4> source = ResourceDescriptorHeap[g_postPass.input0];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    output[pixel] = float4(color * g_post.tonePeak, source.Load(int3(pixel, 0)).a);
}
