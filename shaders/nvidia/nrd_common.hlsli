#ifndef GMODPP_NRD_COMMON_HLSLI
#define GMODPP_NRD_COMMON_HLSLI

// NVIDIA NRD's front-end and back-end packing for the denoiser's inputs and outputs (shaders/nrd.hlsl). Built at compile
// time with NRD's shader headers (CMakeLists.txt); nothing here is compiled at runtime.

#include "../common.hlsli"
#include "NRD.hlsli"

// g_pass.step: which denoiser runs (NrdDenoiser::Method).
static const uint kNrdRelax = 0;
static const uint kNrdReblur = 1;

// REBLUR's hit distance normalisation, as src/nrd_denoiser.cpp sets it: NRD's defaults with A in inches.
static const float3 kReblurHitDistance = float3(3.0 * 39.37, 0.1, 20.0);

// The view depth NRD skips: beyond CommonSettings::denoisingRange (its default, 500000).
static const float kNrdSkipViewZ = 1e6;

float4 NrdPackRadiance(float3 radiance, float hitDistance, float viewZ, float roughness)
{
    if (g_pass.step == kNrdReblur)
        return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, kReblurHitDistance, roughness), true);
    return RELAX_FrontEnd_PackRadianceAndHitDist(radiance, hitDistance, true);
}

float3 NrdUnpackRadiance(float4 packed)
{
    if (g_pass.step == kNrdReblur)
        return REBLUR_BackEnd_UnpackRadianceAndNormHitDist(packed).rgb;
    return RELAX_BackEnd_UnpackRadiance(packed).rgb;
}

#endif
