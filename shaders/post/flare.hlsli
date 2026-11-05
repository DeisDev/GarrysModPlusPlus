#ifndef POST_FLARE_HLSLI
#define POST_FLARE_HLSLI

#include "post.hlsli"

// The light reaching the lens from the sun or the Moon per colour, as bright as a white surface facing it (CameraMain's
// visibility times its brightness), faded out as it leaves the frame by a quarter of the height. The lens flare it makes
// is drawn by shaders/post/flare.hlsl.
float3 FlareLight()
{
    if (g_post.flareInFront == 0.0 || g_post.flareVisibilityView == kNoView)
        return 0.0;
    RWStructuredBuffer<float4> camera = ResourceDescriptorHeap[g_post.cameraView];
    float3 visibility = float3(camera[0].zw, camera[1].x);
    float2 outside = max(max(-g_post.flarePosition, g_post.flarePosition - float2(g_post.size)), 0.0);
    float fade = saturate(1.0 - length(outside) / (0.25 * float(g_post.size.y)));
    return g_post.flareColor * visibility * fade;
}

#endif
