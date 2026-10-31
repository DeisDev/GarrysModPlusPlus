#include "water.hlsli"

// Light the deferred pass adds on top of diffuse lighting, found from the material of the triangle each pixel
// shows (shaders/primary.hlsl): reflections of env_cubemap materials (envmap mask times $envmaptint, as
// LightmappedGeneric applies them) and self-illumination of surfaces whose G-buffer shader wrote none. Reflections
// are glossy where the twin wrote a Phong exponent (PhongRoughness), mirrors elsewhere; reflected surfaces are lit where
// they are (ReflectedRadiance in shaders/reflection.hlsli).
// input1: the main view's denoised light of last frame, or none; input2: the exact direct light (shaders/direct.hlsl);
// output0: the reflection, before the tonemap scale, and its hit distance (0 without one) for
// shaders/reflection_temporal.hlsl and NRD; output1, output2, output3 (or none): specular albedo, linear roughness and
// specular hit distance guides for Ray Reconstruction and NRD; output4: the material table's self-illumination.

// The roughness a reflection was traced with, linear (the square root of alpha) as Ray Reconstruction takes it.
float GuideRoughness(float alpha)
{
    return g_frame.glossyReflections != 0 && alpha >= kMirrorRoughness ? sqrt(alpha) : 0.0;
}

void WriteGuides(int2 pixel, float3 specularAlbedo, float roughness, float hitDistance)
{
    if (g_pass.output1 == kNoView)
        return;
    RWTexture2D<float4> specular = ResourceDescriptorHeap[g_pass.output1];
    RWTexture2D<float> rough = ResourceDescriptorHeap[g_pass.output2];
    RWTexture2D<float> distance = ResourceDescriptorHeap[g_pass.output3];
    specular[pixel] = float4(specularAlbedo, 1.0);
    rough[pixel] = roughness;
    distance[pixel] = hitDistance;
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
    RWTexture2D<float4> selfIllumOutput = ResourceDescriptorHeap[g_pass.output4];
    output[pixel] = 0.0;
    selfIllumOutput[pixel] = 0.0;
    WriteGuides(pixel, 0.0, 0.0, kSkyHitDistance);
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    if (surface.w <= 0.0 || g_frame.traceMask == 0)
        return;
    WriteGuides(pixel, 0.0, 1.0, 0.0);
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    float3 view = normalize(position - g_frame.eyePosition);
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    if (IsWaterSurface(MaterialBits(flags.Load(int3(pixel, 0))))) {
        // Textured water: a mirror under the water's Fresnel term, scaled by its reflect tint and the base texture's
        // alpha, from just above the plane (shaders/water.hlsli).
        Texture2D<float4> normals = ResourceDescriptorHeap[g_frame.gbufferNormalView];
        float4 water = normals.Load(int3(pixel, 0));
        uint seed = PixelSeed(pixel, 0x8Fu);
        float weight;
        float3 direction = ReflectionDirection(view, WaterNormal(water.xy, water.w, -view, kWaveSlope), float3(0.0, 0.0, 1.0), kGlintRoughness,
            float2(Random(seed), Random(seed)), weight);
        direction.z = max(direction.z, 0.02);
        float strength = Schlick(dot(surface.xyz, -view)) * water.z * g_frame.reflectionScale;
        float hitDistance;
        float lobe = g_frame.glossyReflections != 0 ? LobeSpread(kGlintRoughness) : 0.0;
        float3 reflection = ReflectedRadiance(position + float3(0.0, 0.0, 2.0), normalize(direction), PixelAngle() * surface.w, PixelAngle() + lobe, seed,
            hitDistance);
        output[pixel] = float4(reflection * strength * weight, hitDistance);
        WriteGuides(pixel, saturate(strength), GuideRoughness(kGlintRoughness), hitDistance);
        return;
    }
    HitInfo hit;
    if (!VisibleHit(pixel, hit))
        return;
    Material material;
    float2 uv;
    float uvPerUnit;
    if (!HitMaterial(hit, material, uv, uvPerUnit))
        return;
    // The pixel's size on the surface, from the neighbours on the side that stays on the same surface.
    float3 right = WorldPositionFromDepth(pixel + int2(1, 0), DepthAt(pixel + int2(1, 0))) - position;
    float3 left = position - WorldPositionFromDepth(pixel - int2(1, 0), DepthAt(pixel - int2(1, 0)));
    float3 down = WorldPositionFromDepth(pixel + int2(0, 1), DepthAt(pixel + int2(0, 1))) - position;
    float3 up = position - WorldPositionFromDepth(pixel - int2(0, 1), DepthAt(pixel - int2(0, 1)));
    float pixelWidth = max(min(length(right), length(left)), min(length(down), length(up)));
    float footprint = pixelWidth * uvPerUnit;

    float3 result = 0.0;
    float reflectionDistance = 0.0;
    float mask = MaskValue(material.envmapMask, uv, footprint);
    float reflectivity = mask * g_frame.reflectionScale;
    if (reflectivity > 0.0) {
        float3 normal = surface.xyz;
        float3 geometricNormal = GeometricNormalAt(pixel);
        float3 origin = SurfaceRayOrigin(position, geometricNormal, surface.w);
        uint seed = PixelSeed(pixel, 0x8Fu);
        float hitDistance;
        float alpha = PhongRoughness(pixel);
        float weight;
        float3 direction = ReflectionDirection(view, normal, geometricNormal, alpha, float2(Random(seed), Random(seed)), weight);
        float lobe = g_frame.glossyReflections != 0 && alpha >= kMirrorRoughness ? LobeSpread(alpha) : 0.0;
        // As LightmappedGeneric and VertexLitGeneric shape their cubemap term.
        float3 specular = ReflectedRadiance(origin, direction, pixelWidth, PixelAngle() + lobe, seed, hitDistance) * material.envmapTint * (reflectivity * weight);
        specular = lerp(specular, specular * specular, material.envmapContrast);
        specular = lerp(dot(specular, float3(0.299, 0.587, 0.114)), specular, material.envmapSaturation);
        if (material.envmapLightScale > 0.0) {
            // GMod's CUBEMAP 2 combos fade the reflection where the lightmap is dark; the pixel's light stands in for it.
            float3 ambient = g_frame.skyColor;
            if (g_pass.input1 != kNoView) {
                Texture2D<float4> denoised = ResourceDescriptorHeap[g_pass.input1];
                ambient = denoised.Load(int3(pixel, 0)).rgb;
            }
            Texture2D<float4> direct = ResourceDescriptorHeap[g_pass.input2];
            float3 light = direct.Load(int3(pixel, 0)).rgb + ambient * g_frame.indirectScale;
            float3 fade = saturate((light - material.envmapLightMin) * material.envmapLightRange);
            specular *= lerp(1.0, fade, material.envmapLightScale);
        }
        result += specular;
        reflectionDistance = hitDistance;
        float roughness = GuideRoughness(alpha);
        WriteGuides(pixel, SpecularAlbedo(saturate(material.envmapTint * reflectivity), roughness * roughness, dot(normal, -view)), roughness, hitDistance);
    }
    float selfIllum = MaskValue(material.selfIllumMask, uv, footprint);
    if (selfIllum > 0.0) {
        Texture2D<float4> emissive = ResourceDescriptorHeap[g_frame.emissiveView];
        Texture2D<float4> albedo = ResourceDescriptorHeap[g_frame.albedoView];
        if (all(emissive.Load(int3(pixel, 0)).rgb == 0.0))
            selfIllumOutput[pixel] = float4(material.selfIllumTint * albedo.Load(int3(pixel, 0)).rgb * selfIllum, 0.0);
    }
    output[pixel] = float4(result, reflectionDistance);
}
