#include "dither.hlsli"

// Colour grading of the 3D view after the game's post-processing (native::GradeView): the view, copied to `source`, is
// decoded from sRGB, graded in linear light and encoded again over itself.

// Mirrors GradeConstants in src/shaderapi_pipelines.cpp.
struct GradeConstants
{
    // Exposure times white balance, per channel.
    float3 gain;
    // The power applied around middle grey.
    float contrast;
    float saturation;
    uint source;
    uint2 padding;
};

ConstantBuffer<GradeConstants> g_grade : register(b0);

static const float kMiddleGrey = 0.18;

float3 SrgbToLinear(float3 color)
{
    return select(color <= 0.04045, color / 12.92, pow((color + 0.055) / 1.055, 2.4));
}

float3 LinearToSrgb(float3 color)
{
    return select(color <= 0.0031308, color * 12.92, 1.055 * pow(color, 1.0 / 2.4) - 0.055);
}

float4 main(float4 position : SV_Position) : SV_Target
{
    Texture2D<float4> source = ResourceDescriptorHeap[g_grade.source];
    float4 color = source.Load(int3(position.xy, 0));
    float3 graded = SrgbToLinear(color.rgb) * g_grade.gain;
    graded = kMiddleGrey * pow(max(graded, 0.0) / kMiddleGrey, g_grade.contrast);
    float luminance = dot(graded, float3(0.2126, 0.7152, 0.0722));
    graded = max(lerp(luminance, graded, g_grade.saturation), 0.0);
    return float4(LinearToSrgb(saturate(graded)) + DitherNoise(uint2(position.xy)) / 255.0, color.a);
}
