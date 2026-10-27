// Appended to the game's LIGHTING_PREVIEW pixel shaders for draws after the G-buffer capture
// (bytecode::AppendForwardLighting): v0, v1 and v2 stand for the albedo, world normal and world position those
// shaders write to oC0-oC2, and its samplers are moved to ones the shader leaves free.

#include "forward_light.hlsli"

sampler3D ProbeRed : register(s0);
sampler3D ProbeGreen : register(s1);
sampler3D ProbeBlue : register(s2);

float4 main(float4 albedo : TEXCOORD0, float4 normal : TEXCOORD1, float4 position : TEXCOORD2) : COLOR0
{
    return ForwardLight(albedo, normal.xyz, position.xyz, 0.0, ProbeRed, ProbeGreen, ProbeBlue);
}
