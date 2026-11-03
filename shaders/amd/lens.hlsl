// The lens and film stages of the camera effects (src/post_effects.cpp) with AMD FidelityFX Lens 1.1's models (FidelityFX
// SDK 1.1.4, MIT): its chromatic aberration's dispersion, vignetting and film grain. FfxLens itself is not used: it
// samples blue at pixel corners, half a pixel off red and green.

#include "../post/flare.hlsli"
#include "../dither.hlsli"
#include "ffx_core.h"

// The callbacks ffx_lens.h's FfxLens calls, which this file does not use.
FfxFloat32 FfxLensSampleR(FfxFloat32x2 coord)
{
    Texture2D<float4> source = ResourceDescriptorHeap[g_postPass.input0];
    return source.SampleLevel(g_postLinear, coord, 0).r;
}

FfxFloat32 FfxLensSampleG(FfxFloat32x2 coord)
{
    Texture2D<float4> source = ResourceDescriptorHeap[g_postPass.input0];
    return source.SampleLevel(g_postLinear, coord, 0).g;
}

FfxFloat32 FfxLensSampleB(FfxFloat32x2 coord)
{
    Texture2D<float4> source = ResourceDescriptorHeap[g_postPass.input0];
    return source.SampleLevel(g_postLinear, coord, 0).b;
}

void StoreLensOutput(FfxInt32x2 coord, FfxFloat32x3 color)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    output[coord] = float4(color, 1.0);
}

FfxFloat32 ChromAb()
{
    return g_post.chromaticAberration;
}

FfxFloat32x2 Center()
{
    return 0.5 * FfxFloat32x2(g_post.size);
}

FfxFloat32 Vignette()
{
    return g_post.vignette;
}

FfxFloat32 GrainScale()
{
    return g_post.grainScale;
}

FfxFloat32 GrainAmount()
{
    return g_post.grainAmount;
}

FfxUInt32 GrainSeed()
{
    return g_post.grainSeed;
}

#include "lens/ffx_lens.h"

// input0: the view (linear); output0: the view through the lens: radially distorted, each colour magnified by its
// refraction (lateral chromatic aberration, relative to blue), with the lens flare and vignetting.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void LensMain(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= g_post.size))
        return;
    Texture2D<float4> source = ResourceDescriptorHeap[g_postPass.input0];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    float2 size = float2(g_post.size);
    float2 centre = 0.5 * size;
    float2 position = float2(pixel) + 0.5;
    float2 fromCentre = (position - centre) / size.y;
    float2 seen = centre + (position - centre) * ((1.0 + g_post.distortion * dot(fromCentre, fromCentre)) * g_post.distortionScale);
    float4 base = source.SampleLevel(g_postLinear, seen * g_post.invSize, 0);
    float3 color = base.rgb;
    if (g_post.chromaticAberration > 0.0) {
        float2 magnification = FfxLensGetRGMag(g_post.chromaticAberration);
        color.r = source.SampleLevel(g_postLinear, (centre + (seen - centre) * magnification.x) * g_post.invSize, 0).r;
        color.g = source.SampleLevel(g_postLinear, (centre + (seen - centre) * magnification.y) * g_post.invSize, 0).g;
    }
    float3 light = FlareLight();
    if (any(light > 0.0))
        color += LensFlare(position, light);
    if (g_post.vignette > 0.0)
        FfxLensApplyVignette(FfxInt32x2(pixel), FfxInt32x2(centre), color, g_post.vignette);
    output[pixel] = float4(color, base.a);
}

static const float kMiddleGrey = 0.18;

float3 Grade(float3 color)
{
    float3 graded = color * g_post.gradeGain;
    graded = kMiddleGrey * pow(max(graded, 0.0) / kMiddleGrey, g_post.gradeContrast);
    return max(lerp(Luminance(graded), graded, g_post.gradeSaturation), 0.0);
}

// input0: the view (linear). The colour grading, sRGB encoding and film grain (on the encoded values, as FidelityFX Lens
// grains the displayed image), dithered into the back buffer.
[RootSignature(POST_ROOT_SIGNATURE)]
float4 FinalMain(float4 position : SV_Position) : SV_Target
{
    Texture2D<float4> source = ResourceDescriptorHeap[g_postPass.input0];
    int2 pixel = int2(position.xy) - g_post.origin;
    float4 color = source.Load(int3(pixel, 0));
    float3 encoded = LinearToSrgb(saturate(g_post.gradeEnabled != 0 ? Grade(color.rgb) : color.rgb));
    if (g_post.grainAmount > 0.0)
        FfxLensApplyFilmGrain(FfxInt32x2(pixel), encoded, g_post.grainScale, g_post.grainAmount, g_post.grainSeed);
    return float4(encoded + DitherNoise(uint2(position.xy)) / 255.0, color.a);
}
