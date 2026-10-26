#include "scene.hlsli"

// Light the deferred pass adds on top of diffuse lighting, found from the material of the triangle each pixel
// shows (a ray from the eye): mirror reflections of env_cubemap materials (envmap mask times $envmaptint, as
// LightmappedGeneric applies them) and self-illumination of surfaces whose G-buffer shader wrote none.
// Reflected surfaces visible last frame reuse last frame's shaded colour; others get sun and sky light.
// input0: last frame's shaded colour of the main view (linear when outputLinear); output0: added radiance, before
// the tonemap scale; output1, output2 (or none): specular albedo and roughness guides for Ray Reconstruction.

static const float kMaxDistance = 16384.0;

// Last frame's colour at a screen position, before the tonemap scale; the surface there must be `distance`
// from last frame's eye (0 for the sky).
float3 PreviousColor(float4 clip, float distance)
{
    if (g_frame.historyValid == 0 || clip.w <= 0.0)
        return -1.0;
    float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    if (any(uv < 0.0) || any(uv >= 1.0))
        return -1.0;
    int2 pixel = int2(uv * float2(g_frame.size));
    float4 surface = SurfaceAt(g_frame.prevSurfaceView, pixel);
    bool matches = distance > 0.0 ? surface.w > 0.0 && abs(surface.w - distance) <= 4.0 + distance * 0.02 : surface.w <= 0.0;
    if (!matches)
        return -1.0;
    Texture2D<float4> previous = ResourceDescriptorHeap[g_pass.input0];
    float3 color = previous.Load(int3(pixel, 0)).rgb;
    return (g_frame.outputLinear != 0 ? color : Decode(color)) / g_frame.tonemapScale;
}

float3 ReflectedRadiance(float3 origin, float3 direction)
{
    HitSurface hit;
    if (!TraceClosest(origin, direction, kMaxDistance, g_frame.traceMask, hit)) {
        // A direction at infinity: w = 0 projects it to where last frame showed that part of the sky.
        float3 sky = PreviousColor(mul(g_frame.prevViewProj, float4(direction, 0.0)), 0.0);
        return sky.x >= 0.0 ? sky : g_frame.skyColor;
    }
    float3 previous = PreviousColor(mul(g_frame.prevViewProj, float4(hit.position, 1.0)), length(hit.position - g_frame.prevEyePosition));
    if (previous.x >= 0.0)
        return previous;
    return hit.albedo * (SunIrradiance(hit, hit.position + hit.normal) + g_frame.skyColor);
}

void WriteGuides(int2 pixel, float3 specularAlbedo, float roughness)
{
    if (g_pass.output1 == kNoView)
        return;
    RWTexture2D<float4> specular = ResourceDescriptorHeap[g_pass.output1];
    RWTexture2D<float> rough = ResourceDescriptorHeap[g_pass.output2];
    specular[pixel] = float4(specularAlbedo, 1.0);
    rough[pixel] = roughness;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    output[pixel] = 0.0;
    WriteGuides(pixel, 0.0, 0.0);
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    if (surface.w <= 0.0 || g_frame.traceMask == 0)
        return;
    WriteGuides(pixel, 0.0, 1.0);
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    float3 view = normalize(position - g_frame.eyePosition);
    // The eye is inside the local player's body.
    HitInfo hit;
    if (!TraceHit(g_frame.eyePosition, view, surface.w + 8.0 + surface.w * 0.02, g_frame.traceMask & ~kLocalPlayerMask, hit) ||
        abs(hit.t - surface.w) > 4.0 + surface.w * 0.02)
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
    float mask = MaskValue(material.envmapMask, uv, footprint);
    // Source's env_cubemap reflections are mirrors; everything else is treated as fully rough.
    WriteGuides(pixel, material.envmapTint * mask, mask > 0.0 ? 0.0 : 1.0);
    float reflectivity = mask * g_frame.reflectionScale;
    if (reflectivity > 0.0) {
        float3 normal = surface.xyz;
        float3 origin = position + normal * (g_frame.normalBias * (1.0 + surface.w * 0.002));
        // As LightmappedGeneric and VertexLitGeneric shape their cubemap term.
        float3 specular = ReflectedRadiance(origin, reflect(view, normal)) * material.envmapTint * reflectivity;
        specular = lerp(specular, specular * specular, material.envmapContrast);
        specular = lerp(dot(specular, float3(0.299, 0.587, 0.114)), specular, material.envmapSaturation);
        result += specular;
    }
    float selfIllum = MaskValue(material.selfIllumMask, uv, footprint);
    if (selfIllum > 0.0) {
        Texture2D<float4> emissive = ResourceDescriptorHeap[g_frame.emissiveView];
        Texture2D<float4> albedo = ResourceDescriptorHeap[g_frame.albedoView];
        if (all(emissive.Load(int3(pixel, 0)).rgb == 0.0))
            result += material.selfIllumTint * albedo.Load(int3(pixel, 0)).rgb * selfIllum;
    }
    output[pixel] = float4(result, 1.0);
}
