// Replaces the game's textured Water combos ($basetexture: no refraction, the base texture lit by the bumped lightmap
// plus the reflection, water_ps2x_helper.h) during the G-buffer capture: the base texture becomes a surface for the
// real-time lighting. The normal target holds what shaders/water_surface.hlsl writes there (the normal map's slope, the
// reflection's strength and the eye vector's angle), marked by kMaterialWaterSurface (shaders/common.hlsli), from which
// shaders/surface.hlsl rebuilds the rippled normal and shaders/reflections.hlsl adds the reflection. Inputs: t0 the
// base and normal maps' coordinates, t1 the eye vector in the surface's tangent space.

sampler BaseTextureSampler : register(s1);
sampler NormalSampler : register(s4);

float4 g_ReflectTint : register(c4);

struct Input
{
    float2 coord : TEXCOORD0;
    float3 tangentEye : TEXCOORD1;
};

struct Output
{
    float4 albedo : COLOR0;
    float4 normal : COLOR1;
    float4 emissive : COLOR2;
    float4 flags : COLOR3;
};

static const float kNoEyeAngle = 10.0;
static const float kMaterialWaterSurface = 32.0;

Output main(Input input)
{
    float4 base = tex2D(BaseTextureSampler, input.coord);
    float2 slope = tex2D(NormalSampler, input.coord).xy * 2.0 - 1.0;
    float eyeAngle = dot(input.tangentEye.xy, input.tangentEye.xy) > 1e-6 ? atan2(input.tangentEye.y, input.tangentEye.x) : kNoEyeAngle;
    Output output;
    output.albedo = float4(base.rgb, 1.0);
    output.normal = float4(slope, dot(g_ReflectTint.rgb, 1.0 / 3.0) * base.a, eyeAngle);
    output.emissive = 0.0;
    output.flags = float4(1.0, 1.0 - kMaterialWaterSurface / 255.0, 0.0, 1.0);
    return output;
}
