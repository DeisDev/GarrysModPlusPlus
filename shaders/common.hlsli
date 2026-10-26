#ifndef GMODPP_COMMON_HLSLI
#define GMODPP_COMMON_HLSLI

// Mirrors gmodpp::FrameConstants in src/effects.h.
struct FrameConstants
{
    row_major float4x4 invViewProj;
    float3 eyePosition;
    float sunEnabled;
    float3 sunDirection;
    float ambientRatio;
    float2 invSize;
    uint2 size;
    float shadowStrength;
    float bakedRadius;
    float normalBias;
    uint debugView;
};

ConstantBuffer<FrameConstants> g_frame : register(b0);
Texture2D<float> g_depth : register(t1);
Texture2D<float4> g_input : register(t2);
RWTexture2D<float4> g_output : register(u0);

float3 WorldPositionAt(int2 pixel)
{
    pixel = clamp(pixel, int2(0, 0), int2(g_frame.size) - 1);
    float depth = g_depth.Load(int3(pixel, 0));
    float2 uv = (float2(pixel) + 0.5) * g_frame.invSize;
    float4 world = mul(g_frame.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return world.xyz / world.w;
}

float3 SurfaceNormalAt(int2 pixel, float3 position)
{
    float3 right = WorldPositionAt(pixel + int2(1, 0)) - position;
    float3 left = position - WorldPositionAt(pixel - int2(1, 0));
    float3 down = WorldPositionAt(pixel + int2(0, 1)) - position;
    float3 up = position - WorldPositionAt(pixel - int2(0, 1));
    float3 dx = dot(right, right) < dot(left, left) ? right : left;
    float3 dy = dot(down, down) < dot(up, up) ? down : up;
    float3 normal = normalize(cross(dx, dy));
    return dot(normal, g_frame.eyePosition - position) < 0.0 ? -normal : normal;
}

float3 ToLinear(float3 color) { return pow(max(color, 0.0), 2.2); }
float3 ToGamma(float3 color) { return pow(max(color, 0.0), 1.0 / 2.2); }

#endif
