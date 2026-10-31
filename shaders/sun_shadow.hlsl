#include "nvidia/nrd_common.hlsli"
#include "scene.hlsli"

// The main view's sun shadows, each pixel's shadow ray aimed at a random point of the sun's disc, for NVIDIA NRD's
// SIGMA_SHADOW_TRANSLUCENCY (or Ray Reconstruction, which takes them noisy). Pixels are set up as in shaders/direct.hlsl,
// which reads the result in place of its own ray to the sun's centre. output0: SIGMA's penumbra; output1: its
// translucency (gba: the light that gets through, per colour); output2, output3: its view depth and normal guides,
// NRD's shared inputs, which shaders/nrd.hlsl writes again later in the frame.

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float> penumbra = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<float4> translucency = ResourceDescriptorHeap[g_pass.output1];
    RWTexture2D<float> viewDepth = ResourceDescriptorHeap[g_pass.output2];
    RWTexture2D<float4> normalRoughness = ResourceDescriptorHeap[g_pass.output3];
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    // The sky and water (a negative distance; shaders/water.hlsli) are not denoised.
    if (surface.w <= 0.0) {
        penumbra[pixel] = 0.0;
        translucency[pixel] = 0.0;
        viewDepth[pixel] = kNrdSkipViewZ;
        normalRoughness[pixel] = 0.0;
        return;
    }
    float3 normal = surface.xyz;
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    float3 faceNormal = GeometricNormalAt(pixel);
    float3 origin = SurfaceRayOrigin(position, faceNormal, surface.w);
    Texture2D<uint> pixelFlags = ResourceDescriptorHeap[g_frame.pixelFlagsView];
    Texture2D<uint4> visibility = ResourceDescriptorHeap[g_frame.visibilityView];
    Texture2D<float4> gbufferFlags = ResourceDescriptorHeap[g_frame.flagsView];
    g_twoSided = (pixelFlags.Load(int3(pixel, 0)) & kPixelIgnoreNormals) != 0;
    g_toEye = normalize(g_frame.eyePosition - position);
    g_halfLambert = (MaterialBits(gbufferFlags.Load(int3(pixel, 0))) & kMaterialHalfLambert) != 0;
    uint instance = visibility.Load(int3(pixel, 0)).x;
    bool onModel = instance != kNoView && (LoadInstance(instance).flags & kInstanceModel) != 0;
    float3 lightNormal = g_twoSided && dot(normal, faceNormal) < 0.0 ? -normal : normal;
    float3 behind = g_twoSided ? position - faceNormal * SurfaceBias(surface.w) : origin;
    uint seed = PixelSeed(pixel, 0x5Bu);
    float3 direction = SunDiscDirection(seed);

    // SIGMA's encoding: no distance where the surface faces away from the light, NRD_FP16_MAX where nothing blocks it.
    float occluder = 0.0;
    float3 light = 0.0;
    if (g_frame.sunEnabled != 0.0 && Lambert(lightNormal, direction) > 0.0) {
        light = SunVisibilityToNearest(ShadowOrigin(origin, behind, faceNormal, direction), direction, g_frame.sunShadowMask,
            SelfShadowFlags(onModel, faceNormal, direction), occluder);
        if (occluder >= 65536.0)
            occluder = NRD_FP16_MAX;
    }
    penumbra[pixel] = SIGMA_FrontEnd_PackPenumbra(occluder, kSunAngularRadius);
    translucency[pixel] = SIGMA_FrontEnd_PackTranslucency(occluder, light);
    viewDepth[pixel] = dot(position - g_frame.eyePosition, normalize(g_frame.viewProj[3].xyz));
    normalRoughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(normal, 1.0, 0.0);
}
