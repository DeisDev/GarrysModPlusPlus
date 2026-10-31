#ifndef GMODPP_PHONG_HLSLI
#define GMODPP_PHONG_HLSLI

#include "scene.hlsli"

// A pixel's Phong highlight from its twin (shaders/twins): the exponent in the emissive target's alpha (negative for
// Source's model specular, positive for GMod's brush Phong, 0 for none) and the strength in the flags' blue.
struct Phong
{
    float exponent;
    float strength;
    float3 normal;
    float3 toEye;
    // Rim light (Source's SpecularAndRimTerms), or exponent 0.
    float rimExponent;
    float rimScale;
};

Phong PhongAt(int2 pixel, bool lit, float3 position, float3 normal)
{
    Phong phong = (Phong)0;
    if (!lit)
        return phong;
    Texture2D<float4> emissive = ResourceDescriptorHeap[g_frame.emissiveView];
    phong.exponent = emissive.Load(int3(pixel, 0)).a;
    if (phong.exponent == 0.0)
        return phong;
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    float4 marks = flags.Load(int3(pixel, 0));
    phong.strength = marks.b;
    phong.normal = normal;
    phong.toEye = normalize(g_frame.eyePosition - position);
    if (phong.exponent < 0.0 && (MaterialBits(marks) & kMaterialRimLight) != 0) {
        Texture2D<float4> normals = ResourceDescriptorHeap[g_frame.gbufferNormalView];
        phong.rimExponent = normals.Load(int3(pixel, 0)).a;
        phong.rimScale = RimScale(marks);
    }
    return phong;
}

// Rim light from one light: pow(R.L, rim exponent) times the light reaching the surface, scaled by the rim's mask,
// Fresnel and tint. The skin shader takes the larger of the summed highlights and the summed rim.
float3 ModelRim(Phong phong, float3 light, float3 toLight)
{
    if (phong.rimExponent <= 0.0)
        return 0.0;
    float3 reflected = 2.0 * phong.normal * dot(phong.normal, phong.toEye) - phong.toEye;
    return light * (phong.rimScale * pow(saturate(dot(reflected, toLight)), phong.rimExponent));
}

// Models: Source's SpecularAndRimTerms (common_vertexlitgeneric_dx9.h), pow(R.L, exponent) times the light reaching
// the surface (with its N.L), as the skin shader sums it over its lights.
float3 ModelHighlight(Phong phong, float3 light, float3 toLight)
{
    if (phong.exponent >= 0.0)
        return 0.0;
    float3 reflected = 2.0 * phong.normal * dot(phong.normal, phong.toEye) - phong.toEye;
    return light * (phong.strength * pow(saturate(dot(reflected, toLight)), -phong.exponent));
}

// The sun's highlight. Brushes: GMod's lightmappedgeneric_ps30 PHONG combos compute pow(|N.H|, exponent) * sqrt(N.L) *
// strength * L * saturate(L) toward the sun (g_LightDir), with L the lightmap; here L is the traced direct sunlight.
float3 SunHighlight(Phong phong, float3 sunlight)
{
    if (phong.exponent < 0.0)
        return ModelHighlight(phong, sunlight, g_frame.sunDirection);
    if (phong.exponent == 0.0)
        return 0.0;
    float lambert = saturate(dot(phong.normal, g_frame.sunDirection));
    float3 halfway = normalize(phong.toEye + g_frame.sunDirection);
    return sunlight * saturate(sunlight) * (phong.strength * sqrt(lambert) * pow(saturate(abs(dot(phong.normal, halfway))), phong.exponent));
}

#endif
