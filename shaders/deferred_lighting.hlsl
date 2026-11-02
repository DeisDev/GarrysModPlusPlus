#include "baked.hlsli"
#include "water.hlsli"

// Final shading: albedo times (direct light + denoised light) plus self-illumination, reflections and the light
// the game's additive passes added, written in the encoding the game uses for lit colours. Pixels without
// G-buffer data keep the game's forward colour.
// input0: denoised light (indirect plus the sampled direct remainder), or none for the sky's light; input1: raw
// estimate (debug view); input2: reflections and self-illumination (shaders/reflections.hlsl), or none; input3:
// exact direct light (rgb) and sun visibility (a) from shaders/direct.hlsl; input4: sampled direct remainder
// (debug view); output0: shaded colour, encoded or (outputLinear) linear; output1, output2, output3, output4 (or none):
// Ray Reconstruction's specular albedo, roughness, hit distance and diffuse albedo guides, which water pixels overwrite.

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

// Under water the eye looks through the water it is in: its light gathers at this many points when the volumetric pass
// does not gather it.
static const uint kUnderwaterSteps = 2;

// Source's range fog, applied after the tonemap scale. Lit fog (volumetricFog) only hides the surface here: the fog's
// own light comes with the volumetric light. Under water the water the eye looks through takes Source's water fog's
// place (ThroughWater), its light with the volumetric light when that pass runs.
float3 ApplyFog(float3 color, float3 position, int2 pixel)
{
    if (g_frame.eyeWater != kNoView) {
        float3 toPosition = position - g_frame.eyePosition;
        float distance = length(toPosition);
        float3 transmitted = color * exp(-LoadWaterBody(g_frame.eyeWater).extinction * distance);
        if (g_frame.volumetricView != kNoView)
            return transmitted;
        uint seed = PixelSeed(pixel, 0x77u);
        return transmitted + ThroughWater(0.0, g_frame.eyePosition, toPosition / max(distance, 1e-4), distance, g_frame.eyeWater, kUnderwaterSteps, seed) *
            g_frame.tonemapScale;
    }
    if (g_frame.fogEnabled == 0)
        return color;
    float fog = FogOpacity(mul(g_frame.viewProj, float4(position, 1.0)).w);
    return g_frame.volumetricFog != 0 ? color * (1.0 - fog) : lerp(color, g_frame.fogColor, fog);
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

// Skin: $halflambert materials on character models (kInstanceCharacter).
bool IsSkin(int2 pixel)
{
    if (g_frame.traceMask == 0 || g_frame.visibilityView == kNoView)
        return false;
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    if ((MaterialBits(flags.Load(int3(pixel, 0))) & kMaterialHalfLambert) == 0)
        return false;
    Texture2D<uint4> visibility = ResourceDescriptorHeap[g_frame.visibilityView];
    uint instance = visibility.Load(int3(pixel, 0)).x;
    return instance != kNoView && (LoadInstance(instance).flags & kInstanceCharacter) != 0;
}

// Light scatters under skin before it leaves: its scatter distance per colour (Jensen et al. 2001, skin1's mean free
// paths of 3.67, 1.37 and 0.68 mm, in inch units), as the shape of Burley's normalized diffusion profile
// R(r) = (e^(-r/d) + e^(-r/3d)) / (8 pi d r), which reaches about 3 d.
static const float3 kSkinScatter = float3(3.67, 1.37, 0.68) / 25.4;
static const uint kSkinTaps = 16;

// A skin pixel's exact direct light spread by the profile over the skin around it, which softens and reddens shadow
// edges. The taps cover a disc evenly, each standing for the same area; taps off the skin or off the pixel's plane are
// left out, and each colour's weights are normalised, so no light is gained or lost. Under a pixel's width it changes
// nothing. The rest of the light was denoised and is soft already.
float3 SkinDirect(int2 pixel, float3 position, float3 normal, float3 direct, Texture2D<float4> directTexture)
{
    float reach = 3.0 * kSkinScatter.r;
    float depth = mul(g_frame.viewProj, float4(position, 1.0)).w;
    float radius = reach * 0.5 * float(g_frame.size.y) * length(g_frame.viewProj[1].xyz) / max(depth, 1e-3);
    if (radius < 1.0)
        return direct;
    radius = min(radius, 48.0);
    // The pixel itself stands for the disc's middle, half a tap's spacing out.
    float centre = 0.5 * reach / sqrt(float(kSkinTaps));
    float3 weights = (exp(-centre / kSkinScatter) + exp(-centre / (3.0 * kSkinScatter))) / (25.1327412 * kSkinScatter * centre);
    float3 sum = direct * weights;
    for (uint k = 0; k < kSkinTaps; ++k) {
        float angle = 2.39996323 * k;
        int2 tap = pixel + int2(round(float2(cos(angle), sin(angle)) * (radius * sqrt((k + 0.5) / kSkinTaps))));
        if (any(tap < 0) || any(tap >= int2(g_frame.size)) || all(tap == pixel) || SurfaceAt(g_frame.surfaceView, tap).w <= 0.0 || !IsSkin(tap))
            continue;
        float3 offset = WorldPositionFromDepth(tap, DepthAt(tap)) - position;
        if (abs(dot(normal, offset)) > reach)
            continue;
        float r = max(length(offset), centre);
        float3 w = (exp(-r / kSkinScatter) + exp(-r / (3.0 * kSkinScatter))) / (25.1327412 * kSkinScatter * r);
        sum += directTexture.Load(int3(tap, 0)).rgb * w;
        weights += w;
    }
    return sum / weights;
}

// Ray Reconstruction's guides for a mirror's pixels (IsMirror), which show the game's reflection view: a perfect
// mirror whose reflection lies as far away as a traced ray along the mirrored view finds, so it follows the reflection
// as the camera moves rather than smearing it over the glass.
void MirrorGuides(int2 pixel)
{
    if (g_pass.output1 == kNoView || g_frame.traceMask == 0 || any(pixel + 1 >= int2(g_frame.size)))
        return;
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    float depth = DepthAt(pixel);
    if (depth >= 1.0 || !IsMirror(flags.Load(int3(pixel, 0))))
        return;
    float3 position = WorldPositionFromDepth(pixel, depth);
    float3 normal = cross(WorldPositionFromDepth(pixel + int2(1, 0), DepthAt(pixel + int2(1, 0))) - position,
        WorldPositionFromDepth(pixel + int2(0, 1), DepthAt(pixel + int2(0, 1))) - position);
    float3 toEye = g_frame.eyePosition - position;
    float eyeDistance = length(toEye);
    normal = normalize(normal);
    if (dot(normal, toEye) < 0.0)
        normal = -normal;
    HitInfo hit;
    float3 direction = reflect(-toEye / max(eyeDistance, 1e-4), normal);
    float hitDistance = TraceHit(position + normal * SurfaceBias(eyeDistance), direction, 65536.0, g_frame.traceMask, hit) ? hit.t : kSkyHitDistance;
    RWTexture2D<float4> specular = ResourceDescriptorHeap[g_pass.output1];
    RWTexture2D<float> roughness = ResourceDescriptorHeap[g_pass.output2];
    RWTexture2D<float> distanceGuide = ResourceDescriptorHeap[g_pass.output3];
    specular[pixel] = float4(1.0, 1.0, 1.0, 1.0);
    roughness[pixel] = 0.0;
    distanceGuide[pixel] = hitDistance;
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
    // Water belongs to the main view's run; the viewmodel's run, over a rectangle of the same image, leaves it as lit.
    if (!covered && surface.w < 0.0 && g_frame.viewmodel == 0) {
        float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
        WaterResult water = ShadeWater(pixel, position);
        float3 shaded = ApplyFog(water.color * g_frame.tonemapScale, position, pixel) + VolumetricLight(pixel);
        output[pixel] = float4(g_frame.outputLinear != 0 ? shaded : Encode(shaded), color.a);
        if (g_pass.output1 != kNoView) {
            RWTexture2D<float4> specular = ResourceDescriptorHeap[g_pass.output1];
            RWTexture2D<float> roughness = ResourceDescriptorHeap[g_pass.output2];
            RWTexture2D<float> hitDistance = ResourceDescriptorHeap[g_pass.output3];
            specular[pixel] = float4(water.specularAlbedo, 1.0);
            roughness[pixel] = 0.0;
            hitDistance[pixel] = water.hitDistance;
            if (water.refractedGuide) {
                RWTexture2D<float4> albedoGuide = ResourceDescriptorHeap[g_pass.output4];
                albedoGuide[pixel] = float4(water.diffuseAlbedo, 1.0);
            }
        }
        return;
    }
    if (!covered) {
        float3 beyond = Decode(color.rgb);
        // Under water, a pixel this view left undrawn looks through the water to nothing it meets; the back buffer
        // still holds Source's view into the air there (its refraction view, which is not lit).
        if (g_frame.eyeWater != kNoView && DepthAt(pixel) >= 1.0) {
            float3 direction = normalize(WorldPositionFromDepth(pixel, 0.5) - g_frame.eyePosition);
            beyond = ApplyFog(0.0, g_frame.eyePosition + direction * 65536.0, pixel);
        }
        float3 sky = beyond + VolumetricLight(pixel);
        output[pixel] = float4(g_frame.outputLinear != 0 ? sky : Encode(sky), color.a);
        MirrorGuides(pixel);
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
    if (g_frame.selfIllumView != kNoView) {
        Texture2D<float4> selfIllum = ResourceDescriptorHeap[g_frame.selfIllumView];
        added += selfIllum.Load(int3(pixel, 0)).rgb;
    }
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    Texture2D<float4> directTexture = ResourceDescriptorHeap[g_pass.input3];
    float4 directLight = directTexture.Load(int3(pixel, 0));
    float3 direct = directLight.rgb;
    if (g_frame.debugView == 0 && IsSkin(pixel))
        direct = SkinDirect(pixel, position, surface.xyz, direct, directTexture);
    float3 indirect = SkyLight(surface.xyz);
    if (g_pass.input0 != kNoView) {
        Texture2D<float4> indirectTexture = ResourceDescriptorHeap[g_pass.input0];
        indirect = indirectTexture.Load(int3(pixel, 0)).rgb;
    }
    indirect *= g_frame.indirectScale;
    // Ambient occlusion (set only with baked indirect light) darkens the baked light alone: the denoised light also
    // holds the traced remainder of the direct light, which already has its shadows. The baked light is taken again
    // here, unfiltered, to keep the occlusion sharp.
    float ambientOcclusion = 1.0;
    if (g_frame.aoView != kNoView) {
        Texture2D<float> ao = ResourceDescriptorHeap[g_frame.aoView];
        ambientOcclusion = ao.Load(int3(pixel, 0));
        bool leaf;
        float3 baked = BakedIndirect(pixel, position, LitNormal(pixel, surface.xyz, leaf), leaf) * g_frame.indirectScale;
        indirect = max(indirect - baked * (g_frame.aoIntensity * (1.0 - ambientOcclusion)), 0.0);
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
    // The radiance cache's voxels, each its own colour (black without the cache).
    case 12: result = RadianceCacheEnabled() ? CacheDebugColor(position, GeometricNormalAt(pixel)) : 0.0; break;
    // RTXDI's sample per pixel: its light as a colour, or its reservoir's sample count (shaders/restir.hlsl); black without.
    case 13:
    case 14: {
        result = 0.0;
        if (g_frame.restirDebugView != kNoView) {
            Texture2D<float4> restir = ResourceDescriptorHeap[g_frame.restirDebugView];
            result = restir.Load(int3(pixel, 0)).rgb;
        }
        break;
    }
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
        float3 shaded = ApplyFog((albedo * light + emissive + added + specular) * g_frame.tonemapScale, position, pixel) + additive + VolumetricLight(pixel);
        result = g_frame.outputLinear != 0 ? shaded : Encode(shaded);
        // NRD's validation overlay (debug view 15) over the image.
        if (g_frame.debugView == 15 && g_frame.nrdValidationView != kNoView) {
            Texture2D<float4> validation = ResourceDescriptorHeap[g_frame.nrdValidationView];
            float4 overlay = validation.Load(int3(pixel, 0));
            result = lerp(result, overlay.rgb, overlay.a);
        }
        break;
    }
    }
    output[pixel] = float4(result, color.a);
}
