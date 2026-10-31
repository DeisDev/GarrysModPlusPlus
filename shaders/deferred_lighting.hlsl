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

// Source's rim light also adds the ambient light seen along the view (vRimAmbientCubeColor in skin_ps20b) times its
// boost where the surface faces up; the indirect light at the pixel stands in for the ambient cube.
float3 AmbientRim(int2 pixel, float3 normal, float3 indirect)
{
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    float4 marks = flags.Load(int3(pixel, 0));
    uint bits = MaterialBits(marks);
    if ((bits & kMaterialRimLight) == 0)
        return 0.0;
    float boost = RimBoost(bits);
    return indirect * (boost * saturate(RimScale(marks) * normal.z));
}

// LightmappedGeneric's light warp: the total diffuse light scaled by the warp texture at half its length, doubled
// (lightmappedgeneric_ps2_3_x.h, where it warps the lightmap).
float3 WarpedTotalLight(int2 pixel, float3 light)
{
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    float4 marks = flags.Load(int3(pixel, 0));
    uint view = WarpsTotalLight(MaterialBits(marks)) ? LightWarpView(marks) : kNoView;
    if (view == kNoView)
        return light;
    Texture2D<float4> warp = ResourceDescriptorHeap[NonUniformResourceIndex(view)];
    return light * (2.0 * warp.SampleLevel(g_linearClamp, float2(0.5 * length(light), 0.5), 0.0).rgb);
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
    float3 indirect = SkyLight(surface.xyz);
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
    case 9: {
        // Reflections, self-illumination from the material table and Phong highlights.
        float3 highlights = AmbientRim(pixel, surface.xyz, indirect);
        if (g_frame.specularView != kNoView) {
            Texture2D<float4> specularTexture = ResourceDescriptorHeap[g_frame.specularView];
            highlights += specularTexture.Load(int3(pixel, 0)).rgb;
        }
        result = Encode((added + highlights) * g_frame.tonemapScale);
        break;
    }
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
        float3 light = unlit ? 1.0 : WarpedTotalLight(pixel, direct + indirect);
        float3 specular = unlit ? 0.0 : AmbientRim(pixel, surface.xyz, indirect);
        if (!unlit && g_frame.specularView != kNoView) {
            Texture2D<float4> highlights = ResourceDescriptorHeap[g_frame.specularView];
            specular += highlights.Load(int3(pixel, 0)).rgb;
        }
        float3 shaded = ApplyFog((albedo * light + emissive + added + specular) * g_frame.tonemapScale, position) + additive + VolumetricLight(pixel);
        result = g_frame.outputLinear != 0 ? shaded : Encode(shaded);
        break;
    }
    }
    output[pixel] = float4(result, color.a);
}
