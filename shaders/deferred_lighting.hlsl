#include "water.hlsli"

// Final shading: albedo times (direct light + denoised light) plus self-illumination, reflections and the light
// the game's additive passes added, written in the encoding the game uses for lit colours. Pixels without
// G-buffer data keep the game's forward colour.
// input0: denoised light (indirect plus the sampled direct remainder), or none for the sky's light; input1: raw
// estimate (debug view); input2: reflections and self-illumination (shaders/reflections.hlsl), or none; input3:
// exact direct light (rgb) and sun visibility (a) from shaders/direct.hlsl; input4: sampled direct remainder
// (debug view); output0: shaded colour, encoded or (outputLinear) linear; output1, output2, output3 (or none): Ray
// Reconstruction's specular albedo, roughness and hit distance guides, which water pixels overwrite.

// Light scattered by the haze between the eye and the pixel (shaders/volumetrics.hlsl), after the tonemap scale.
float3 VolumetricLight(int2 pixel)
{
    if (g_frame.volumetricView == kNoView)
        return 0.0;
    Texture2D<float4> volumetric = ResourceDescriptorHeap[g_frame.volumetricView];
    // The half-resolution grid fills only part of the texture when Ray Reconstruction renders below the output size.
    float2 textureSize;
    volumetric.GetDimensions(textureSize.x, textureSize.y);
    float2 position = min((float2(pixel) + 0.5) * 0.5, float2((g_frame.size + 1) / 2) - 0.5);
    return volumetric.SampleLevel(g_linearClamp, position / textureSize, 0.0).rgb * g_frame.tonemapScale;
}

// Source's range fog (CalcRangeFog and BlendPixelFog in common_ps_fxc.h), applied after the tonemap scale.
float3 ApplyFog(float3 color, float3 position)
{
    if (g_frame.fogEnabled == 0)
        return color;
    float viewDepth = mul(g_frame.viewProj, float4(position, 1.0)).w;
    float fog = saturate(min(g_frame.fogParams.z, viewDepth * g_frame.fogParams.w - g_frame.fogParams.x));
    // fogParams.y: 1 under water, whose fog Source blends in linearly.
    return lerp(color, g_frame.fogColor, g_frame.fogParams.y > 0.0 ? fog : fog * fog);
}

// The sun's Phong highlight. Brushes (shaders/twins/lightmappedgeneric_ps2_3_x.h): GMod's lightmappedgeneric_ps30
// computes pow(|N.H|, exponent) * sqrt(N.L) * strength * L * saturate(L) toward g_LightDir, with L the lightmap; here L
// is the traced direct sunlight. Models (skin, a negative exponent): Source's SpecularAndRimTerms,
// pow(R.L, exponent) * N.L * light * strength. The exponent is in the emissive target's alpha, the strength in the
// flags' blue.
float3 PhongHighlight(int2 pixel, float3 normal, float3 position, float sunVisibility, bool unlit)
{
    Texture2D<float4> emissives = ResourceDescriptorHeap[g_frame.emissiveView];
    float exponent = emissives.Load(int3(pixel, 0)).a;
    if (unlit || exponent == 0.0 || g_frame.sunEnabled == 0.0 || sunVisibility <= 0.0)
        return 0.0;
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    float strength = flags.Load(int3(pixel, 0)).b;
    float3 toEye = normalize(g_frame.eyePosition - position);
    float lambert = saturate(dot(normal, g_frame.sunDirection));
    float3 sunlight = g_frame.sunColor * (sunVisibility * lambert);
    if (exponent < 0.0) {
        float3 reflected = 2.0 * normal * dot(normal, toEye) - toEye;
        return sunlight * (strength * pow(saturate(dot(reflected, g_frame.sunDirection)), -exponent));
    }
    float3 halfway = normalize(toEye + g_frame.sunDirection);
    return sunlight * saturate(sunlight) * (strength * sqrt(lambert) * pow(saturate(abs(dot(normal, halfway))), exponent));
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    id.xy = DispatchPixel(id.xy);
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
    Texture2D<float4> lightTexture = ResourceDescriptorHeap[g_frame.lightView];
    float3 additive = g_frame.additiveAfterResolve != 0 ? 0.0 : Decode(lightTexture.Load(int3(pixel, 0)).rgb);
    if (!covered && surface.w < 0.0) {
        float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
        WaterResult water = ShadeWater(pixel, position, g_frame.previousLitView);
        float3 shaded = ApplyFog(water.color * g_frame.tonemapScale, position) + VolumetricLight(pixel);
        output[pixel] = float4(g_frame.outputLinear != 0 ? shaded : Encode(shaded), color.a);
        if (g_pass.output1 != kNoView) {
            RWTexture2D<float4> specular = ResourceDescriptorHeap[g_pass.output1];
            RWTexture2D<float> roughness = ResourceDescriptorHeap[g_pass.output2];
            RWTexture2D<float> hitDistance = ResourceDescriptorHeap[g_pass.output3];
            specular[pixel] = float4(water.specularAlbedo, 1.0);
            roughness[pixel] = 0.02;
            hitDistance[pixel] = water.hitDistance;
        }
        return;
    }
    if (!covered) {
        float3 sky = Decode(color.rgb) + VolumetricLight(pixel);
        output[pixel] = float4(g_frame.outputLinear != 0 ? sky : Encode(sky), color.a);
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
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    Texture2D<float4> directTexture = ResourceDescriptorHeap[g_pass.input3];
    float4 directLight = directTexture.Load(int3(pixel, 0));
    float3 direct = directLight.rgb;
    float3 indirect = g_frame.skyColor;
    if (g_pass.input0 != kNoView) {
        Texture2D<float4> indirectTexture = ResourceDescriptorHeap[g_pass.input0];
        indirect = indirectTexture.Load(int3(pixel, 0)).rgb;
    }
    indirect *= g_frame.indirectScale;
    float ambientOcclusion = 1.0;
    if (g_frame.aoView != kNoView) {
        Texture2D<float> ao = ResourceDescriptorHeap[g_frame.aoView];
        ambientOcclusion = ao.Load(int3(pixel, 0));
        indirect *= lerp(1.0, ambientOcclusion, g_frame.aoIntensity);
    }

    float3 result;
    switch (g_frame.debugView) {
    case 4: {
        Texture2D<float4> remainder = ResourceDescriptorHeap[g_pass.input4];
        result = Encode((direct + remainder.Load(int3(pixel, 0)).rgb) * g_frame.tonemapScale * 0.5);
        break;
    }
    case 5: result = Encode(indirect * g_frame.tonemapScale); break;
    case 6: {
        Texture2D<float4> raw = ResourceDescriptorHeap[g_pass.input1];
        result = Encode(raw.Load(int3(pixel, 0)).rgb * g_frame.tonemapScale);
        break;
    }
    case 7: result = directLight.a; break;
    case 8: result = Encode(Decode(lightTexture.Load(int3(pixel, 0)).rgb)); break;
    case 9: result = Encode(added * g_frame.tonemapScale); break;
    case 11: result = ambientOcclusion; break;
    case 10: {
        // Red and green: motion towards last frame (a tenth of a pixel per step); blue: unlit material.
        Texture2D<float2> motion = ResourceDescriptorHeap[g_frame.motionView];
        Texture2D<uint> pixelFlags = ResourceDescriptorHeap[g_frame.pixelFlagsView];
        result = float3(saturate(0.5 + motion.Load(int3(pixel, 0)) * 0.05), (pixelFlags.Load(int3(pixel, 0)) & kPixelUnlit) != 0 ? 1.0 : 0.0);
        break;
    }
    default: {
        Texture2D<uint> pixelFlags = ResourceDescriptorHeap[g_frame.pixelFlagsView];
        bool unlit = (pixelFlags.Load(int3(pixel, 0)) & kPixelUnlit) != 0;
        float3 light = unlit ? 1.0 : direct + indirect;
        float3 shaded = ApplyFog((albedo * light + emissive + added + PhongHighlight(pixel, surface.xyz, position, directLight.a, unlit)) * g_frame.tonemapScale,
            position) + additive + VolumetricLight(pixel);
        result = g_frame.outputLinear != 0 ? shaded : Encode(shaded);
        break;
    }
    }
    output[pixel] = float4(result, color.a);
}
