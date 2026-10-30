// Replaces the game's textured Water combos ($basetexture: no refraction, the base texture lit by the bumped lightmap
// plus the reflection, water_ps2x_helper.h) during the G-buffer capture: the base texture becomes a surface for the
// real-time lighting. The reflection is left out, and the normal map's ripples are not in the lit normal (a zero normal,
// for which the lighting takes the depth's). Input: t0 the base texture's coordinates.

sampler BaseTextureSampler : register(s1);

struct Input
{
    float2 coord : TEXCOORD0;
};

struct Output
{
    float4 albedo : COLOR0;
    float4 normal : COLOR1;
    float4 emissive : COLOR2;
    float4 flags : COLOR3;
};

Output main(Input input)
{
    Output output;
    output.albedo = float4(tex2D(BaseTextureSampler, input.coord).rgb, 1.0);
    output.normal = 0.0;
    output.emissive = 0.0;
    output.flags = float4(1.0, 1.0, 0.0, 1.0);
    return output;
}
