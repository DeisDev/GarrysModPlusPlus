#include "nvidia/nrd_common.hlsli"

// The main view's indirect light and reflections through NVIDIA NRD (src/nrd_denoiser.h), without Ray Reconstruction.
// InputsMain: NRD's inputs. input0: the raw indirect estimate with the sampled direct remainder (shaders/gi_trace.hlsl;
// its alpha the indirect rays' hit distance); input1: the raw reflections (shaders/reflections.hlsl; alpha their hit
// distance, 0 without one); input2, input3: the reflections' linear roughness and specular albedo. output0: view depth;
// output1: normal and roughness; output2, output3: diffuse and specular radiance with hit distance. The light is
// demodulated: the indirect estimate is irradiance already, and the reflections are divided by their specular albedo.
// ResolveMain: input0, input1: NRD's diffuse and specular outputs; input2: the specular albedo. output0: the denoised
// irradiance; output1: the denoised reflections times their specular albedo.

// Specular albedo below this leaves the reflection as it is.
static const float kMinSpecularAlbedo = 0.01;

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void InputsMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float> viewDepth = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<float4> normalRoughness = ResourceDescriptorHeap[g_pass.output1];
    RWTexture2D<float4> diffuse = ResourceDescriptorHeap[g_pass.output2];
    RWTexture2D<float4> specular = ResourceDescriptorHeap[g_pass.output3];
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    // The sky and water (a negative distance; shaders/water.hlsli) are not denoised.
    if (surface.w <= 0.0) {
        viewDepth[pixel] = kNrdSkipViewZ;
        normalRoughness[pixel] = 0.0;
        diffuse[pixel] = 0.0;
        specular[pixel] = 0.0;
        return;
    }
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    float viewZ = dot(position - g_frame.eyePosition, normalize(g_frame.viewProj[3].xyz));
    Texture2D<float4> indirect = ResourceDescriptorHeap[g_pass.input0];
    Texture2D<float4> reflections = ResourceDescriptorHeap[g_pass.input1];
    Texture2D<float> roughnessGuide = ResourceDescriptorHeap[g_pass.input2];
    Texture2D<float4> specularAlbedo = ResourceDescriptorHeap[g_pass.input3];
    float roughness = roughnessGuide.Load(int3(pixel, 0));
    float4 light = indirect.Load(int3(pixel, 0));
    float4 reflection = reflections.Load(int3(pixel, 0));
    float3 albedo = specularAlbedo.Load(int3(pixel, 0)).rgb;
    viewDepth[pixel] = viewZ;
    normalRoughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(surface.xyz, roughness, 0.0);
    diffuse[pixel] = NrdPackRadiance(light.rgb, light.a, viewZ, 1.0);
    specular[pixel] = NrdPackRadiance(reflection.rgb / max(albedo, kMinSpecularAlbedo), reflection.a, viewZ, roughness);
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
    irradiance[pixel] = float4(max(NrdUnpackRadiance(diffuse.Load(int3(pixel, 0))), 0.0), 1.0);
    float3 albedo = specularAlbedo.Load(int3(pixel, 0)).rgb;
    reflections[pixel] = float4(max(NrdUnpackRadiance(specular.Load(int3(pixel, 0))), 0.0) * max(albedo, kMinSpecularAlbedo), 0.0);
}
