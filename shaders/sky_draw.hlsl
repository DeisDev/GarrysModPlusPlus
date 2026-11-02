#include "atmosphere.hlsli"

// The game's 2D sky passes drawn with the procedural sky (SkyMain), or the sun's disc added over the game's own sky
// (DiscMain), in place of the game's pixel shader (src/shaderapi_pipelines.cpp, DrawSky). The translated vertex shader
// still places the faces; the constants come through the forward twins' root parameter (register space 1).
struct SkyDrawConstants
{
    // Clip space to world space for the draw: the inverse of its view-projection rows (vertex c8-c11) as drawn.
    row_major float4x4 clipToWorld;
    // The viewport's left, top, width and height in pixels.
    float4 viewport;
    float3 sunDirection;
    // cLightScale.x of the game's sky shader, which Source multiplies its colour by.
    float tonemapScale;
    // env_sun's colour, its largest component 1.
    float3 sunColor;
    uint skyView;
    uint transmittanceView;
    uint sunDisc;
    uint2 padding;
};

ConstantBuffer<SkyDrawConstants> g_sky : register(b0, space1);
SamplerState g_linearClamp : register(s0, space1);

// The translated shaders' interpolants (Interpolants in src/shader_translator.cpp), so this shader links with them.
struct Interpolants
{
    float4 position : SV_Position;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float4 t4 : TEXCOORD4;
    float4 t5 : TEXCOORD5;
    float4 t6 : TEXCOORD6;
    float4 t7 : TEXCOORD7;
    float4 t8 : TEXCOORD8;
    float4 t9 : TEXCOORD9;
    float4 color0 : COLOR0;
    float4 color1 : COLOR1;
    float4 fog : FOG;
};

// The world direction through a pixel centre. Translated vertex shaders move the game's geometry by D3D9's half-pixel
// offset (gmodpp_halfPixel), so a pixel centre shows the game's clip-space point half a pixel up and left of it.
float3 ViewDirection(float2 pixel)
{
    float2 ndc = (pixel - g_sky.viewport.xy) / g_sky.viewport.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
    ndc -= float2(1.0, -1.0) / g_sky.viewport.zw;
    float4 nearPoint = mul(g_sky.clipToWorld, float4(ndc, 0.0, 1.0));
    float4 farPoint = mul(g_sky.clipToWorld, float4(ndc, 1.0, 1.0));
    return normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
}

// The sun's disc in a direction. It is thousands of times brighter than the sky (its irradiance over 6.8e-5 sr), so it
// is shown as bright as the target holds, in the colour the atmosphere leaves it (env_sun's tint times the transmittance
// toward it), with Allen's limb darkening at 550 nm (I / I0 = 0.3 + 0.93 mu - 0.23 mu^2; Cox, Allen's Astrophysical
// Quantities, 2000). Its edge covers the share of the pixel inside it, and the ground hides it below the horizon.
float3 SunDisc(float3 direction)
{
    float3 sun = g_sky.sunDirection;
    float angle = atan2(length(cross(direction, sun)), dot(direction, sun));
    float coverage = saturate((kSunDiscRadius - angle) / max(fwidth(angle), 1e-7) + 0.5);
    float3 eye = float3(0.0, 0.0, kGroundRadius + kEyeHeight);
    if (coverage <= 0.0 || SphereDistance(eye, direction, kGroundRadius) >= 0.0)
        return 0.0;
    Texture2D<float4> transmittance = ResourceDescriptorHeap[g_sky.transmittanceView];
    float3 color = g_sky.sunColor * AtmosphereTransmittance(transmittance, g_linearClamp, eye.z, sun.z);
    color /= max(max(color.r, max(color.g, color.b)), 1e-6);
    float r = saturate(angle / kSunDiscRadius);
    float mu = sqrt(1.0 - r * r);
    return color * ((0.3 + 0.93 * mu - 0.23 * mu * mu) * coverage);
}

float4 SkyMain(Interpolants input) : SV_Target0
{
    float3 direction = ViewDirection(input.position.xy);
    TextureCube<float4> sky = ResourceDescriptorHeap[g_sky.skyView];
    float3 color = sky.SampleLevel(g_linearClamp, direction, 0.0).rgb * g_sky.tonemapScale;
    if (g_sky.sunDisc != 0)
        color += SunDisc(direction);
    return float4(color, 1.0);
}

float4 DiscMain(Interpolants input) : SV_Target0
{
    return float4(SunDisc(ViewDirection(input.position.xy)), 0.0);
}
