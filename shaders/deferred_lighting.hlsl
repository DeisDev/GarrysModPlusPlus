#include "scene.hlsli"

// Final shading: albedo times (traced sun + traced local lights + denoised indirect light) plus
// self-illumination, plus the light the game's flashlight passes added, written in the encoding the game
// uses for lit colours. Pixels without
// G-buffer data keep the game's forward colour.
// input0: denoised indirect irradiance; input1: raw indirect estimate (debug view); input2: reflections and
// self-illumination (shaders/reflections.hlsl), or none; output0: shaded colour, encoded or (outputLinear) linear.

// Source's range fog (CalcRangeFog and BlendPixelFog in common_ps_fxc.h), applied after the tonemap scale.
float3 ApplyFog(float3 color, float3 position)
{
    if (g_frame.fogEnabled == 0)
        return color;
    float viewDepth = mul(g_frame.viewProj, float4(position, 1.0)).w;
    float fog = saturate(min(g_frame.fogParams.z, viewDepth * g_frame.fogParams.w - g_frame.fogParams.x));
    return lerp(color, g_frame.fogColor, fog * fog);
}

// Shadow rays per pixel for map and dynamic lights when more lights than this matter there.
static const uint kShadowSamples = 4;

float3 PointLight(LightRanges ranges, uint mapLights, uint i, float3 position, float3 normal, out float3 toLight, out float distance)
{
    uint index = i >= mapLights ? g_frame.lightCount + i - mapLights : LightRangeIndex(ranges, i);
    return LightIrradiance(LoadLight(index), position, normal, toLight, distance);
}

float3 DirectIrradiance(float3 position, float3 normal, float distanceToEye, inout uint seed, out float sunVisibility)
{
    float3 origin = position + normal * (g_frame.normalBias * (1.0 + distanceToEye * 0.002));
    float3 irradiance = 0.0;
    sunVisibility = 0.0;
    float lambert = saturate(dot(normal, g_frame.sunDirection));
    if (g_frame.sunEnabled != 0.0 && lambert > 0.0) {
        sunVisibility = SunVisibility(origin);
        irradiance += g_frame.sunColor * lambert * sunVisibility;
    }
    // Shadow rays go to each light that matters or, when more than kShadowSamples do, to that many picked in
    // proportion to their light in one pass (weighted reservoir sampling) and weighted to keep the sum unbiased.
    LightRanges ranges = LightsAt(position, normal);
    uint mapLights = LightRangeCount(ranges);
    uint lights = mapLights + g_frame.dynamicLightCount;
    float total = 0.0;
    uint visible = 0;
    uint first[kShadowSamples];
    uint picked[kShadowSamples];
    float pickedWeight[kShadowSamples];
    [unroll]
    for (uint s = 0; s < kShadowSamples; ++s) {
        first[s] = 0;
        picked[s] = 0;
        pickedWeight[s] = 1.0;
    }
    for (uint i = 0; i < lights; ++i) {
        float3 toLight;
        float distance;
        float weight = Luminance(PointLight(ranges, mapLights, i, position, normal, toLight, distance));
        if (weight <= VisibleIrradiance())
            continue;
        [unroll]
        for (uint s = 0; s < kShadowSamples; ++s) {
            if (s == visible)
                first[s] = i;
        }
        ++visible;
        total += weight;
        [unroll]
        for (uint s = 0; s < kShadowSamples; ++s) {
            if (Random(seed) * total < weight) {
                picked[s] = i;
                pickedWeight[s] = weight;
            }
        }
    }
    bool exact = visible <= kShadowSamples;
    [unroll]
    for (uint s = 0; s < kShadowSamples; ++s) {
        if (s >= visible)
            break;
        uint i = exact ? first[s] : picked[s];
        float3 toLight;
        float distance;
        float3 light = PointLight(ranges, mapLights, i, position, normal, toLight, distance);
        bool attached = i >= mapLights;
        if (!Occluded(origin, toLight, ShadowRayLength(attached, distance), ShadowMask(attached)))
            irradiance += exact ? light : light * (total / (pickedWeight[s] * kShadowSamples));
    }
    for (uint p = 0; p < g_frame.projectorCount; ++p) {
        float3 toLight;
        float distance;
        float3 light = ProjectorIrradiance(LoadProjector(p), position, normal, toLight, distance);
        if (Luminance(light) > VisibleIrradiance() && !Occluded(origin, toLight, ShadowRayLength(true, distance), ShadowMask(true)))
            irradiance += light;
    }
    return irradiance;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    Texture2D<float4> colors = ResourceDescriptorHeap[g_frame.colorView];
    float4 color = colors.Load(int3(pixel, 0));
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    bool covered = surface.w > 0.0;
    if (g_frame.debugView == 3) {
        output[pixel] = float4(covered ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0), color.a);
        return;
    }
    if (!covered) {
        output[pixel] = g_frame.outputLinear != 0 ? float4(Decode(color.rgb), color.a) : color;
        return;
    }
    Texture2D<float4> albedos = ResourceDescriptorHeap[g_frame.albedoView];
    float3 albedo = albedos.Load(int3(pixel, 0)).rgb;
    if (g_frame.debugView == 1) {
        output[pixel] = float4(Encode(albedo), color.a);
        return;
    }
    if (g_frame.debugView == 2) {
        output[pixel] = float4(surface.xyz * 0.5 + 0.5, color.a);
        return;
    }

    Texture2D<float4> emissives = ResourceDescriptorHeap[g_frame.emissiveView];
    float3 emissive = emissives.Load(int3(pixel, 0)).rgb;
    float3 added = 0.0;
    if (g_pass.input2 != kNoView) {
        Texture2D<float4> reflections = ResourceDescriptorHeap[g_pass.input2];
        added = reflections.Load(int3(pixel, 0)).rgb;
    }
    Texture2D<float4> lightTexture = ResourceDescriptorHeap[g_frame.lightView];
    float3 flashlight = Decode(lightTexture.Load(int3(pixel, 0)).rgb);
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    float sunVisibility;
    uint seed = PixelSeed(pixel, 0x2Du);
    float3 direct = DirectIrradiance(position, surface.xyz, surface.w, seed, sunVisibility);
    Texture2D<float4> indirectTexture = ResourceDescriptorHeap[g_pass.input0];
    float3 indirect = (g_frame.traceMask != 0 ? indirectTexture.Load(int3(pixel, 0)).rgb : g_frame.skyColor) * g_frame.indirectScale;

    float3 result;
    switch (g_frame.debugView) {
    case 4: result = Encode(direct * g_frame.tonemapScale * 0.5); break;
    case 5: result = Encode(indirect * g_frame.tonemapScale); break;
    case 6: {
        Texture2D<float4> raw = ResourceDescriptorHeap[g_pass.input1];
        result = Encode(raw.Load(int3(pixel, 0)).rgb * g_frame.tonemapScale);
        break;
    }
    case 7: result = sunVisibility; break;
    case 8: result = Encode(flashlight); break;
    case 9: result = Encode(added * g_frame.tonemapScale); break;
    default: {
        float3 shaded = ApplyFog((albedo * (direct + indirect) + emissive + added) * g_frame.tonemapScale, position) + flashlight;
        result = g_frame.outputLinear != 0 ? shaded : Encode(shaded);
        break;
    }
    }
    output[pixel] = float4(result, color.a);
}
