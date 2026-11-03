#include "common.hlsli"

// The main view's indirect light and reflections through AMD FSR Ray Regeneration (src/fsr.h), as shaders/nrd.hlsl
// prepares them for NRD.
// InputsMain: input0: the raw indirect estimate with the sampled direct remainder (shaders/gi_trace.hlsl; its alpha the
// indirect rays' hit distance); input1: the raw reflections (alpha their hit distance); input2, input3: the reflections'
// linear roughness and specular albedo; input4: last frame's distance from the eye (shaders/primary.hlsl). output0:
// signed linear view depth, 0 (outside the depth bounds, passed through) on the sky and water; output1: the world normal
// (octahedral), roughness and material type 0; output2, output3: diffuse and specular light with hit distance, without
// their first albedo (the indirect estimate is irradiance already; the reflections are divided by their specular
// albedo); output4: motion as last frame's texture coordinates minus this frame's, and last frame's view depth minus
// this frame's.
// ResolveMain: input0, input1: the denoised diffuse and specular light; input2: the specular albedo. output0: the
// denoised irradiance; output1: the denoised reflections times their specular albedo.

// Specular albedo below this leaves the reflection as it is (kMinSpecularAlbedo in shaders/nrd.hlsl).
static const float kMinSpecularAlbedo = 0.01;

// The octahedral encoding the denoiser takes (denoising.md, 5.3.1).
float2 NormalToOctahedronUv(float3 n)
{
    n.xy /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 k = select(n.xy >= 0.0, 1.0, -1.0);
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * k;
    return n.xy * 0.5 + 0.5;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void InputsMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float> linearDepth = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<float4> normals = ResourceDescriptorHeap[g_pass.output1];
    RWTexture2D<float4> diffuse = ResourceDescriptorHeap[g_pass.output2];
    RWTexture2D<float4> specular = ResourceDescriptorHeap[g_pass.output3];
    RWTexture2D<float4> motionOut = ResourceDescriptorHeap[g_pass.output4];
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    if (surface.w <= 0.0) {
        linearDepth[pixel] = 0.0;
        normals[pixel] = 0.0;
        diffuse[pixel] = 0.0;
        specular[pixel] = 0.0;
        motionOut[pixel] = 0.0;
        return;
    }
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    float3 toPosition = position - g_frame.eyePosition;
    float viewZ = dot(toPosition, normalize(g_frame.viewProj[3].xyz));
    Texture2D<float4> indirect = ResourceDescriptorHeap[g_pass.input0];
    Texture2D<float4> reflections = ResourceDescriptorHeap[g_pass.input1];
    Texture2D<float> roughnessGuide = ResourceDescriptorHeap[g_pass.input2];
    Texture2D<float4> specularAlbedo = ResourceDescriptorHeap[g_pass.input3];
    Texture2D<float> previousDistance = ResourceDescriptorHeap[g_pass.input4];
    Texture2D<float2> motion = ResourceDescriptorHeap[g_frame.motionView];
    float roughness = roughnessGuide.Load(int3(pixel, 0));
    float4 light = indirect.Load(int3(pixel, 0));
    float4 reflection = reflections.Load(int3(pixel, 0));
    float3 albedo = specularAlbedo.Load(int3(pixel, 0)).rgb;
    // Last frame's view depth along the same ray from the eye, from last frame's distance to the point.
    float distance = length(toPosition);
    float previousViewZ = previousDistance.Load(int3(pixel, 0)) * viewZ / max(distance, 1e-4);
    linearDepth[pixel] = viewZ;
    normals[pixel] = float4(NormalToOctahedronUv(surface.xyz), roughness, 0.0);
    diffuse[pixel] = float4(light.rgb, light.a);
    specular[pixel] = float4(reflection.rgb / max(albedo, kMinSpecularAlbedo), reflection.a);
    motionOut[pixel] = float4(motion.Load(int3(pixel, 0)) * g_frame.invSize, previousViewZ - viewZ, 0.0);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void ResolveMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    Texture2D<float4> diffuse = ResourceDescriptorHeap[g_pass.input0];
    Texture2D<float4> specular = ResourceDescriptorHeap[g_pass.input1];
    Texture2D<float4> specularAlbedo = ResourceDescriptorHeap[g_pass.input2];
    RWTexture2D<float4> irradiance = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<float4> reflections = ResourceDescriptorHeap[g_pass.output1];
    if (SurfaceAt(g_frame.surfaceView, pixel).w <= 0.0) {
        irradiance[pixel] = 0.0;
        reflections[pixel] = 0.0;
        return;
    }
    irradiance[pixel] = float4(max(diffuse.Load(int3(pixel, 0)).rgb, 0.0), 1.0);
    float3 albedo = specularAlbedo.Load(int3(pixel, 0)).rgb;
    reflections[pixel] = float4(max(specular.Load(int3(pixel, 0)).rgb, 0.0) * max(albedo, kMinSpecularAlbedo), 0.0);
}
