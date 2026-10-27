#include "scene.hlsli"

// The main view's depth for the translucent surfaces the game draws after the lighting (Lighting::Run). The G-buffer
// was drawn with a sub-pixel jitter for anti-aliasing, so at an edge its depth flips between the near and the far
// surface from frame to frame, and so would translucent surfaces seen past the edge (glass between stair steps).
// Edge pixels take the depth of the scene seen through their unjittered centre instead, unless the ray meets something
// nearer than the G-buffer shows within g_pass.step pixels (geometry the scene adds). The far side of a thin gap can lie
// beyond every sample of this frame's jittered neighbourhood, so a farther hit is kept; where the scene lacks geometry
// the G-buffer has, that misplaces only edge pixels.
// input0, input1: the image's jitter shift in output pixels (x right, y down, as floats); output0: the depth.

float EyeDistance(int2 pixel, float depth)
{
    return depth >= 1.0 ? 1e30 : length(WorldPositionFromDepth(pixel, depth) - g_frame.eyePosition);
}

float Tolerance(float distance) { return 1.0 + 0.01 * distance; }

// The nearest surface the G-buffer can hold along a ray: translucent ones are drawn after it.
bool TraceGBufferSurface(float3 origin, float3 direction, out float t)
{
    RayQuery<RAY_FLAG_NONE> query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.01;
    ray.TMax = 65536.0;
    query.TraceRayInline(g_scene, RAY_FLAG_NONE, g_frame.traceMask & ~kLocalPlayerMask, ray);
    while (query.Proceed()) {
        bool translucent;
        float opacity = CandidateOpacity(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), translucent);
        if (!translucent && opacity > 0.0)
            query.CommitNonOpaqueTriangleHit();
    }
    t = query.CommittedRayT();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    int radius = int(g_pass.step);
    float depth = DepthAt(pixel);
    float nearest = depth;
    float farthest = depth;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            float d = DepthAt(pixel + int2(x, y));
            nearest = min(nearest, d);
            farthest = max(farthest, d);
        }
    }
    RWTexture2D<float> output = ResourceDescriptorHeap[g_pass.output0];
    float nearDistance = EyeDistance(pixel, nearest);
    float farDistance = EyeDistance(pixel, farthest);
    if (farDistance - nearDistance <= Tolerance(nearDistance) || g_frame.traceMask == 0) {
        output[pixel] = depth;
        return;
    }
    float2 centre = float2(pixel) + 0.5 + float2(asfloat(g_pass.input0), asfloat(g_pass.input1));
    float2 uv = centre * g_frame.invSize;
    float4 world = mul(g_frame.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
    float3 direction = normalize(world.xyz / world.w - g_frame.eyePosition);
    float distance = 1e30;
    float traced = 1.0;
    float t;
    if (TraceGBufferSurface(g_frame.eyePosition, direction, t)) {
        distance = t;
        float4 clip = mul(g_frame.viewProj, float4(g_frame.eyePosition + direction * t, 1.0));
        traced = g_frame.depthMin + clip.z / clip.w * g_frame.depthScale;
    }
    output[pixel] = distance >= nearDistance - Tolerance(nearDistance) ? traced : depth;
}
