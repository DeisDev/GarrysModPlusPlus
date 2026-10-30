#include "reflection.hlsli"

// Light the deferred pass adds on top of diffuse lighting, found from the material of the triangle each pixel
// shows (shaders/primary.hlsl): mirror reflections of env_cubemap materials (envmap mask times $envmaptint, as
// LightmappedGeneric applies them) and self-illumination of surfaces whose G-buffer shader wrote none.
// Reflected surfaces visible last frame reuse last frame's shaded colour. Others get the sun, one local light
// picked by SampleLight and, as their indirect light, the denoised light of the reflecting pixel.
// input0: last frame's shaded colour of the main view (linear when outputLinear); input1: this frame's denoised
// light, or none; input2: the exact direct light (shaders/direct.hlsl); output0: added radiance, before the tonemap scale, and the reflection's hit distance (0 without
// one) for shaders/reflection_temporal.hlsl; output1, output2, output3 (or none):
// specular albedo, roughness and specular hit distance guides for Ray Reconstruction.

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
    output[pixel] = 0.0;
    WriteGuides(pixel, 0.0, 0.0, kSkyHitDistance);
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    if (surface.w <= 0.0 || g_frame.traceMask == 0)
        return;
    WriteGuides(pixel, 0.0, 1.0, 0.0);
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    float3 view = normalize(position - g_frame.eyePosition);
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
    float footprint = max(min(length(right), length(left)), min(length(down), length(up))) * uvPerUnit;

    float3 result = 0.0;
    float reflectionDistance = 0.0;
    float mask = MaskValue(material.envmapMask, uv, footprint);
    float reflectivity = mask * g_frame.reflectionScale;
    if (reflectivity > 0.0) {
        float3 normal = surface.xyz;
        float3 geometricNormal = GeometricNormalAt(pixel);
        float3 origin = SurfaceRayOrigin(position, geometricNormal, surface.w);
        float3 ambient = g_frame.skyColor;
        if (g_pass.input1 != kNoView) {
            Texture2D<float4> denoised = ResourceDescriptorHeap[g_pass.input1];
            ambient = denoised.Load(int3(pixel, 0)).rgb;
        }
        uint seed = PixelSeed(pixel, 0x8Fu);
        float hitDistance;
        // As LightmappedGeneric and VertexLitGeneric shape their cubemap term.
        float3 specular = ReflectedRadiance(g_pass.input0, origin, AboveSurface(reflect(view, normal), geometricNormal), ambient * g_frame.indirectScale, seed, hitDistance) *
            material.envmapTint * reflectivity;
        specular = lerp(specular, specular * specular, material.envmapContrast);
        specular = lerp(dot(specular, float3(0.299, 0.587, 0.114)), specular, material.envmapSaturation);
        if (material.envmapLightScale > 0.0) {
            // GMod's CUBEMAP 2 combos fade the reflection where the lightmap is dark; the pixel's light stands in for it.
            Texture2D<float4> direct = ResourceDescriptorHeap[g_pass.input2];
            float3 light = direct.Load(int3(pixel, 0)).rgb + ambient * g_frame.indirectScale;
            float3 fade = saturate((light - material.envmapLightMin) * material.envmapLightRange);
            specular *= lerp(1.0, fade, material.envmapLightScale);
        }
        result += specular;
        reflectionDistance = hitDistance;
        // Source's env_cubemap reflections are mirrors.
        WriteGuides(pixel, saturate(material.envmapTint * reflectivity), 0.0, hitDistance);
    }
    float selfIllum = MaskValue(material.selfIllumMask, uv, footprint);
    if (selfIllum > 0.0) {
        Texture2D<float4> emissive = ResourceDescriptorHeap[g_frame.emissiveView];
        Texture2D<float4> albedo = ResourceDescriptorHeap[g_frame.albedoView];
        if (all(emissive.Load(int3(pixel, 0)).rgb == 0.0))
            result += material.selfIllumTint * albedo.Load(int3(pixel, 0)).rgb * selfIllum;
    }
    output[pixel] = float4(result, reflectionDistance);
}
