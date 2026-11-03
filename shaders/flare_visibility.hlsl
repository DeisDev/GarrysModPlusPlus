#include "common.hlsli"
#include "scene.hlsli"

// How much of the sun's or the Moon's disc the eye sees through the scene, per colour, for the lens flare
// (src/post_effects.cpp): rays from the eye to a spiral of points over the disc (input0: its angular radius, input1-3:
// the direction to its centre, as the bits of floats), bent by the eye's water as sunlight is, through translucent
// surfaces and the 3D skybox. The local player does not block them. output0: a 1x1 texture.

static const uint kRays = 64;

groupshared float3 g_visibility[kRays];

[RootSignature(ROOT_SIGNATURE)]
[numthreads(kRays, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    float3 sun = normalize(float3(asfloat(g_pass.input1), asfloat(g_pass.input2), asfloat(g_pass.input3)));
    float3 tangent = normalize(cross(abs(sun.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0), sun));
    float3 bitangent = cross(sun, tangent);
    float radius = tan(asfloat(g_pass.input0)) * sqrt((float(index) + 0.5) / float(kRays));
    float angle = float(index) * 2.399963;
    float3 direction = normalize(sun + radius * (cos(angle) * tangent + sin(angle) * bitangent));
    g_visibility[index] = SunVisibilityFrom(g_frame.eyePosition, g_frame.eyeWater, direction, g_frame.sunShadowMask & ~kLocalPlayerMask);
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = kRays / 2; stride > 0; stride >>= 1) {
        if (index < stride)
            g_visibility[index] += g_visibility[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
        output[uint2(0, 0)] = float4(g_visibility[0] / float(kRays), 1.0);
    }
}
