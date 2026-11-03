#include "scene.hlsli"

// The main view's depth for the translucent surfaces the game draws after the lighting (Lighting::Run). The G-buffer
// was drawn with a sub-pixel jitter for anti-aliasing, so at an edge its depth flips between the near and the far
// surface from frame to frame, and so would translucent surfaces seen past the edge (glass between stair steps).
// Edge pixels take the depth of the scene seen through their unjittered centre instead, unless the ray meets something
// nearer than the G-buffer shows within g_pass.step pixels (geometry the scene adds). The far side of a thin gap can lie
// beyond every sample of this frame's jittered neighbourhood, so a farther hit is kept; where the scene lacks geometry
// the G-buffer has, that misplaces only edge pixels. Other pixels take the farthest G-buffer depth around their
// unjittered centre. Every depth is pushed back a little (DepthBehind): the game redraws some surfaces unjittered with a
// depth test (GMod's halos, such as the physics gun's on a held prop), and those draws must pass it.
// input0, input1: the image's jitter shift in output pixels (x right, y down, as floats); output0: the depth.

float EyeDistance(int2 pixel, float depth)
{
    return depth >= 1.0 ? 1e30 : length(WorldPositionFromDepth(pixel, depth) - g_frame.eyePosition);
}

float Tolerance(float distance) { return 1.0 + 0.01 * distance; }

// The depth at `centre` (in pixels of the jittered image) a little farther along its ray: well past the rounding of
// a 24-bit depth buffer, yet too little to show translucent surfaces through what lies in front of them. Edge pixels
// (`edge`) go farther: the ray-traced scene's triangles can differ slightly from the ones the game draws.
float DepthBehind(float2 centre, float depth, bool edge = false)
{
    if (depth >= 1.0)
        return depth;
    float2 uv = centre * g_frame.invSize;
    float4 offset = mul(g_frame.clipToEye, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, (depth - g_frame.depthMin) / g_frame.depthScale, 1.0));
    float3 toPosition = offset.xyz / offset.w;
    float3 position = g_frame.eyePosition + toPosition;
    float distance = length(toPosition);
    position += toPosition / max(distance, 1e-4) * (edge ? 0.25 + distance * 2e-3 : 0.02 + distance * 2e-4);
    float4 clip = mul(g_frame.viewProj, float4(position, 1.0));
    return min(g_frame.depthMin + clip.z / clip.w * g_frame.depthScale, 1.0);
}

// The farthest G-buffer depth around a point between pixel centres. Within a surface it is at most a pixel's depth
// slope farther than the point itself, and at small steps (a prop's ribs) it is the farther side, which the game's
// unjittered redraw may show there.
float DepthAround(float2 centre)
{
    int2 base = int2(floor(centre - 0.5));
    return max(max(DepthAt(base), DepthAt(base + int2(1, 0))), max(DepthAt(base + int2(0, 1)), DepthAt(base + int2(1, 1))));
}

// The nearest surface the G-buffer can hold along a ray: translucent ones are drawn after it.
bool TraceGBufferSurface(float3 origin, float3 direction, out float t)
{
    SCENE_RAY_QUERY(RAY_FLAG_NONE) query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.01;
    ray.TMax = 65536.0;
    query.TraceRayInline(g_scene, RAY_FLAG_NONE, g_frame.traceMask & ~kLocalPlayerMask, ray);
    while (query.Proceed()) {
        bool translucent;
        float3 through = CandidateTransmission(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), translucent);
        if (!translucent && through.x < 1.0)
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
    float2 centre = float2(pixel) + 0.5 + float2(asfloat(g_pass.input0), asfloat(g_pass.input1));
    float nearDistance = EyeDistance(pixel, nearest);
    float farDistance = EyeDistance(pixel, farthest);
    if (farDistance - nearDistance <= Tolerance(nearDistance) || g_frame.traceMask == 0) {
        output[pixel] = DepthBehind(centre, nearest >= 1.0 ? depth : DepthAround(centre));
        return;
    }
    float2 uv = centre * g_frame.invSize;
    float4 offset = mul(g_frame.clipToEye, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
    float3 direction = normalize(offset.xyz / offset.w);
    float distance = 1e30;
    float traced = 1.0;
    float t;
    if (TraceGBufferSurface(g_frame.eyePosition, direction, t)) {
        distance = t;
        float4 clip = mul(g_frame.viewProj, float4(g_frame.eyePosition + direction * t, 1.0));
        traced = g_frame.depthMin + clip.z / clip.w * g_frame.depthScale;
    }
    output[pixel] = DepthBehind(centre, distance >= nearDistance - Tolerance(nearDistance) ? traced : depth, true);
}
