#include "common.hlsli"

#define ROOT_SIGNATURE "CBV(b0), SRV(t0), DescriptorTable(SRV(t1, numDescriptors = 2), UAV(u0))"

RaytracingAccelerationStructure g_scene : register(t0);

// Instance masks set in src/scene.cpp. Lightmaps only know about the map and static props, so the
// baked-sun estimate ignores dynamic objects while the traced visibility includes them.
static const uint kBakedCasters = 0x01 | 0x02;
static const uint kAllCasters = 0xFF;

// The first four taps are tried before the rest; when they all agree with the centre sample, the
// remaining taps cannot change the estimate and are skipped.
static const float2 kDiskTaps[8] = {
    float2(1.0, 0.0), float2(0.0, 1.0), float2(-1.0, 0.0), float2(0.0, -1.0),
    float2(0.707, 0.707), float2(-0.707, 0.707), float2(-0.707, -0.707), float2(0.707, -0.707),
};

bool Occluded(float3 origin, float3 direction, float tMax, uint mask)
{
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE> query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.5;
    ray.TMax = tMax;
    query.TraceRayInline(g_scene, RAY_FLAG_NONE, mask, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float SunVisibility(float3 origin, uint mask)
{
    return Occluded(origin, g_frame.sunDirection, 65536.0, mask) ? 0.0 : 1.0;
}

// Replaces the sun term baked into lightmaps with a per-pixel ray-traced one. The baked term is
// estimated by averaging visibility over a lightmap-texel-sized disk, so geometry missing from the
// acceleration structure leaves the image unchanged instead of adding false light or shadow.
[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    float4 color = g_input.Load(int3(pixel, 0));
    float depth = g_depth.Load(int3(pixel, 0));
    if (depth >= 1.0 || g_frame.sunEnabled == 0.0 || g_frame.debugView == 5) {
        g_output[pixel] = color;
        return;
    }

    float3 position = WorldPositionAt(pixel);
    float3 normal = SurfaceNormalAt(pixel, position);
    float lambert = saturate(dot(normal, g_frame.sunDirection));
    if (lambert <= 0.0) {
        g_output[pixel] = g_frame.debugView != 0 ? float4(0, 0, 0.25, color.a) : color;
        return;
    }

    float bias = g_frame.normalBias * (1.0 + distance(position, g_frame.eyePosition) * 0.002);
    float3 origin = position + normal * bias;
    float traced = SunVisibility(origin, kAllCasters);

    float3 tangent = normalize(abs(normal.z) < 0.9 ? cross(normal, float3(0, 0, 1)) : cross(normal, float3(1, 0, 0)));
    float3 bitangent = cross(normal, tangent);
    // Taps that would start inside neighbouring geometry (wall bases, corners) are skipped; the lightmap
    // has no texels there either.
    float baked = SunVisibility(origin, kBakedCasters);
    float taps = 1.0;
    [loop]
    for (int i = 0; i < 8; ++i) {
        if (i == 4 && (baked == 0.0 || baked == taps))
            break;
        float3 offset = (tangent * kDiskTaps[i].x + bitangent * kDiskTaps[i].y) * g_frame.bakedRadius;
        if (Occluded(origin, offset / g_frame.bakedRadius, g_frame.bakedRadius, kBakedCasters))
            continue;
        baked += SunVisibility(origin + offset, kBakedCasters);
        taps += 1.0;
    }
    baked /= taps;

    float ratio = (g_frame.ambientRatio + lambert * traced) / max(g_frame.ambientRatio + lambert * baked, 1e-3);
    ratio = lerp(1.0, ratio, g_frame.shadowStrength);
    switch (g_frame.debugView) {
    case 1: g_output[pixel] = float4(traced.xxx, color.a); return;
    case 2: g_output[pixel] = float4(baked.xxx, color.a); return;
    case 3: g_output[pixel] = float4(saturate(ratio * 0.5).xxx, color.a); return;
    case 4: g_output[pixel] = float4(normal * 0.5 + 0.5, color.a); return;
    }
    g_output[pixel] = float4(ToGamma(ToLinear(color.rgb) * ratio), color.a);
}
