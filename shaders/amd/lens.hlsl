// The lens and film stages of the camera effects (src/post_effects.cpp) with AMD FidelityFX Lens 1.1's models (FidelityFX
// SDK 1.1.4, MIT): its chromatic aberration's dispersion and film grain. FfxLens itself is not used: it samples blue at
// pixel corners, half a pixel off red and green, and its effects do not follow the field of view.

#include "../post/flare.hlsli"
#include "../tone_curve.hlsli"
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

// input0: the view (linear); output0: the view through the lens, by each pixel's field angle (its tangent from the
// centre over the focal length): radially distorted, each colour magnified by its refraction relative to blue (lateral
// chromatic aberration, which grows with the angle like the distortion), with the lens flare and the cos^4 law's
// vignetting.
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
    float2 field = (position - centre) / g_post.focalPixels;
    float fieldSquared = dot(field, field);
    float2 seen = centre + (position - centre) * ((1.0 + g_post.distortion * fieldSquared) * g_post.distortionScale);
    float4 base = source.SampleLevel(g_postLinear, seen * g_post.invSize, 0);
    float3 color = base.rgb;
    if (g_post.chromaticAberration > 0.0) {
        float2 magnification = 1.0 + (FfxLensGetRGMag(g_post.chromaticAberration) - 1.0) * fieldSquared;
        color.r = source.SampleLevel(g_postLinear, (centre + (seen - centre) * magnification.x) * g_post.invSize, 0).r;
        color.g = source.SampleLevel(g_postLinear, (centre + (seen - centre) * magnification.y) * g_post.invSize, 0).g;
    }
    float3 light = FlareLight();
    if (any(light > 0.0))
        color += LensFlare(position, light);
    // cos^4 of the field angle is (1 / (1 + tan^2))^2.
    if (g_post.vignette > 0.0)
        color *= pow(1.0 / (1.0 + fieldSquared), 2.0 * g_post.vignette);
    output[pixel] = float4(color, base.a);
}

static const float kMiddleGrey = 0.18;

// The engine's colour correction over sRGB-encoded colour, as engine_post blends its look-ups (Engine_Post_ps2x.fxc's
// PerformColorCorrection).
float3 ColorCorrected(float3 encoded)
{
    float3 lookup = encoded * (31.0 / 32.0) + 0.5 / 32.0;
    float3 result = encoded * g_post.ccDefaultWeight;
    [loop]
    for (uint i = 0; i < g_post.ccCount; ++i) {
        Texture3D<float4> volume = ResourceDescriptorHeap[NonUniformResourceIndex(g_post.ccViews[i])];
        result += volume.SampleLevel(g_postLinear, lookup, 0.0).rgb * g_post.ccWeights[i];
    }
    return result;
}

float3 Grade(float3 color)
{
    float3 graded = color * g_post.gradeGain;
    graded = kMiddleGrey * pow(max(graded, 0.0) / kMiddleGrey, g_post.gradeContrast);
    return max(lerp(Luminance(graded), graded, g_post.gradeSaturation), 0.0);
}

// input0: the view. The colour grading, the tone curve up to the display's peak, the engine's colour correction below
// paper white and film grain, back into the frame as display-referred linear light: the HUD, drawn next, stays at paper
// white. The grain is added to the sRGB-encoded values up to paper white, as FidelityFX Lens grains the displayed image
// (its amount fades to nothing at 0 and 1); light above paper white keeps its value.
[RootSignature(POST_ROOT_SIGNATURE)]
float4 FinalMain(float4 position : SV_Position) : SV_Target
{
    Texture2D<float4> source = ResourceDescriptorHeap[g_postPass.input0];
    int2 pixel = int2(position.xy) - g_post.origin;
    float4 color = source.Load(int3(pixel, 0));
    float3 graded = max(g_post.gradeEnabled != 0 ? Grade(color.rgb) : color.rgb, 0.0);
    graded = ToneCurve(graded, g_post.toneCurve, g_post.highlightRolloff, g_post.tonePeak);
    if (g_post.ccCount != 0) {
        // Below paper white as the game corrected its 8-bit image; above it unchanged.
        float3 low = saturate(graded);
        graded = SrgbToLinear(ColorCorrected(LinearToSrgb(low))) + (graded - low);
    }
    if (g_post.grainAmount > 0.0) {
        float3 encoded = LinearToSrgb(graded);
        float3 grained = saturate(encoded);
        FfxLensApplyFilmGrain(FfxInt32x2(pixel), grained, g_post.grainScale, g_post.grainAmount, g_post.grainSeed);
        graded = SrgbToLinear(encoded + grained - saturate(encoded));
    }
    return float4(graded, color.a);
}
