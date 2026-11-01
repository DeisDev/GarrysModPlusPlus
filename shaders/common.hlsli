#ifndef GMODPP_COMMON_HLSLI
#define GMODPP_COMMON_HLSLI

// Every compute pass shares this layout. Textures and buffers are reached through descriptor heap indices
// in g_frame and g_pass. The radiance cache's constants (b2) and hash entries (u0, space2: a root descriptor, on which
// Shader Model 6.6 guarantees 64-bit atomics), and RTXDI's constants (b3), RIS buffer (u1), reservoirs (u2) and neighbour
// offsets (t1), are bound for every pass and read by those built with them (shaders/nvidia).
#define ROOT_SIGNATURE "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), CBV(b0), SRV(t0), RootConstants(num32BitConstants = 12, b1), " \
    "CBV(b2), UAV(u0, space = 2), CBV(b3), UAV(u1, space = 2), UAV(u2, space = 2), SRV(t1, space = 2), " \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP), "     "StaticSampler(s2, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP)"

// Mirrors gmodpp::FrameConstants in src/lighting.h.
struct FrameConstants
{
    row_major float4x4 invViewProj;
    row_major float4x4 viewProj;
    row_major float4x4 prevViewProj;
    float3 eyePosition;
    float tonemapScale;
    float3 sunDirection;
    float sunEnabled;
    float3 sunColor;
    uint outputSrgb;
    float3 skyColor;
    uint debugView;
    float2 invSize;
    uint2 size;
    float normalBias;
    uint traceMask;
    uint frameIndex;
    uint historyValid;
    float3 prevEyePosition;
    uint lightCount;
    uint instancesView;
    uint lightsView;
    uint depthView;
    uint colorView;
    uint gbufferNormalView;
    uint flagsView;
    uint surfaceView;
    uint prevSurfaceView;
    float4 fogParams;
    float3 fogColor;
    uint fogEnabled;
    uint lightView;
    uint dynamicLightsView;
    uint dynamicLightCount;
    uint projectorsView;
    uint projectorCount;
    float lightScale;
    float indirectScale;
    float taaHistoryWeight;
    float sharpening;
    float depthMin;
    float depthScale;
    uint albedoView;
    uint emissiveView;
    uint materialsView;
    float reflectionScale;
    // 1 when the lighting pass writes linear colour (before the tonemap's encoding) for Ray Reconstruction.
    uint outputLinear;
    // Scene::LightLists.
    uint lightRangesView;
    uint lightIndicesView;
    uint bspNodesView;
    uint leafClustersView;
    uint clusterCount;
    // 1 for the first-person viewmodel's run.
    uint viewmodel;
    // Written by shaders/primary.hlsl.
    uint visibilityView;
    uint motionView;
    uint previousDistanceView;
    uint pixelFlagsView;
    // Developer probe (Lighting::RequestProbe): the pixel (x | y << 16) and the buffer it reports to, or kNoView.
    uint probePixel;
    uint probeView;
    // The irradiance volume's first cell and probe spacing (shaders/probes.hlsl).
    int3 probeOrigin;
    float probeSpacing;
    // Ray-traced ambient occlusion (shaders/ao.hlsl), or kNoView, and how much of it darkens indirect light.
    uint aoView;
    float aoIntensity;
    // Half-resolution in-scattered light (shaders/volumetrics.hlsl), or kNoView, and the haze's density per unit.
    uint volumetricView;
    float volumetricDensity;
    // Written by shaders/primary.hlsl: the face normal under each pixel, facing the eye.
    uint geometricNormalView;
    // Ray tracing quality (Settings in src/settings.h).
    uint exactLights;
    uint sampledLightRays;
    uint indirectBounces;
    uint aoRays;
    uint volumeRays;
    uint volumetricSteps;
    // traceMask without what may not block each kind of ray: sun shadows, the first indirect bounce (and ambient
    // occlusion), later bounces and local lights' shadows (shadowless props, by FoliageShadows). Indirect rays never
    // meet the local player, whose hidden first-person body would darken the ground under it.
    uint sunShadowMask;
    uint indirectMask;
    uint bounceMask;
    uint lightShadowMask;
    // Ray flags added to indirect light and ambient occlusion rays.
    uint indirectRayFlags;
    // The corner of the rectangle a view's passes are dispatched over (shaders/view_rect.hlsl), or kNoView.
    uint dispatchOriginView;
    // The main view's filled irradiance volume (red, green, blue coefficients; shaders/probes.hlsl) for the viewmodel's
    // run, or kNoView.
    uint probeRedView;
    uint probeGreenView;
    uint probeBlueView;
    // Light style values (Scene::SetLightStyles) that scale map lights by their style, or kNoView.
    uint lightStylesView;
    // 1 when the game's additive passes (lightView) are added after the temporal resolve (AdditiveAfterResolve), so
    // glows are neither jittered nor denoised.
    uint additiveAfterResolve;
    // 1 when reflections follow the GGX lobe of their roughness (shaders/reflection.hlsli), 0 for mirrors everywhere.
    uint glossyReflections;
    // indirect_lighting = 1 once the map's baked light is ready (shaders/baked.hlsli): the world's BakedTriangles, the
    // faces' lightmaps, the baked bounce light, and the leaf ambient ranges and samples; kNoView otherwise.
    uint bakedTrianglesView;
    uint bakedFacesView;
    uint bakedLightView;
    uint leafAmbientView;
    uint ambientSamplesView;
    // The 3D skybox's camera and 1 / its scale, through which sun rays that reach the sky continue (SunVisibility);
    // worldToSky is 0 without one.
    float skyCameraX;
    float skyCameraY;
    float skyCameraZ;
    float worldToSky;
    // Phong highlights (shaders/direct.hlsl), added unlit by shaders/deferred_lighting.hlsl.
    uint specularView;
    // Each water surface triangle's body of water (WaterBodyOfHit in shaders/water_body.hlsli), or kNoView.
    uint waterTrianglesView;
    // The light warp textures the G-buffer's flags refer to (LightWarpView).
    uint lightWarpCount;
    // The 2D sky as the game draws it and its blurred light for indirect rays (SkyRadiance, SkyLight), or kNoView.
    uint skyCubeView;
    uint skyLightView;
    // Scene::LightLists' light grid (LightsAt in shaders/scene.hlsli).
    float3 gridOrigin;
    float gridCellSize;
    uint3 gridDims;
    uint gridCellsView;
    uint gridIndicesView;
    uint gridTailView;
    // Scene::EmittersView and EmitterCount (0 without EmissiveLighting::Full), and the EmissiveLighting setting.
    uint emittersView;
    uint emitterCount;
    uint emissiveLighting;
    // 1 when volumetricView holds the map's fog, lit (shaders/volumetrics.hlsl), in place of Source's flat fog.
    uint volumetricFog;
    // RTXDI's debug image (shaders/restir.hlsl), or kNoView.
    uint restirDebugView;
    // The 3D skybox's fog, for reflected rays that continue into it (shaders/reflection.hlsli).
    float4 skyboxFogParams;
    float3 skyboxFogColor;
    uint skyboxFogEnabled;
    // Self-illumination from the material table for surfaces whose G-buffer shader wrote none (shaders/reflections.hlsl),
    // and NRD's validation overlay (debug view 15), or kNoView.
    uint selfIllumView;
    uint nrdValidationView;
    // Shadow rays over the sun's disc for the viewmodel's pixels (shaders/direct.hlsl).
    uint viewmodelShadowRays;
    uint padding;
    // The map's bodies of water (shaders/water_body.hlsli), each BSP leaf's body, their normal maps' frames and the
    // highest surface; the body holding the eye (kNoView in air), the clock their normal maps animate by, and the main
    // view's caustics (shaders/caustics.hlsl) over the square from causticsOrigin, 1 / causticsInvSize across.
    uint waterBodiesView;
    uint waterBodyCount;
    uint leafWaterView;
    uint waterFramesView;
    float waterTop;
    uint eyeWater;
    float waterTime;
    uint causticsView;
    float2 causticsOrigin;
    float causticsInvSize;
    // Each body of water's surface as the game last drew it (WaterDraw in shaders/water_body.hlsli), or kNoView.
    uint waterDrawsView;
    // The water's sun maps (shaders/caustics.hlsl): the fine one over the caustics' square, and the coarse one over the
    // square from coarseCausticsOrigin, 1 / coarseCausticsInvSize across; kNoView without sunlight on water.
    uint waterSunView;
    uint coarseWaterSunView;
    float coarseCausticsInvSize;
    // The first brush entity body of water (Scene::PlaceBrushWater), found by its bounds alone.
    uint brushWaterFirst;
    float2 coarseCausticsOrigin;
    uint2 padding5;
    uint4 lightWarpViews[16];
};

// Per-pass descriptor indices and parameters; mirrors Lighting::PassArgs in src/lighting.h.
struct PassConstants
{
    uint input0;
    uint input1;
    uint input2;
    uint input3;
    uint input4;
    uint output0;
    uint output1;
    uint output2;
    uint output3;
    uint step;
    uint output4;
    uint padding;
};

static const uint kNoView = 0xFFFFFFFFu;

// Pixel flags from shaders/primary.hlsl: a scene triangle matches the G-buffer there, its material is unlit, and its
// instance is lit without regard to its normals.
static const uint kPixelHit = 1;
static const uint kPixelUnlit = 2;
static const uint kPixelIgnoreNormals = 4;

// Material bits the twins (shaders/twins) store in the G-buffer flags' green as 1 - bits / 255 on covered pixels.
// Rim light (skin twin) also stores its boost in quarters from bit kMaterialRimBoostShift, its mask, Fresnel and tint
// in the flags' red from 128/255 and its exponent in the normal's alpha. Light warp ($lightwarptexture) stores its
// texture's slot in the flags' alpha; with kMaterialWarpTotal (LightmappedGeneric's $lightwarptexture, without rim
// light) the warp scales the total diffuse light by its brightness instead of each light (WarpsTotalLight).
static const uint kMaterialHalfLambert = 1;
static const uint kMaterialRimLight = 2;
static const uint kMaterialRimBoostShift = 2;
static const uint kMaterialRimBoostMask = 31;
static const uint kMaterialLightWarp = 128;
static const uint kMaterialWarpTotal = 64;
// Textured water (shaders/water_textured_surface.hlsl, without rim light): the normal target holds the water's slope,
// reflection strength and eye angle (shaders/water.hlsli) instead of a normal.
static const uint kMaterialWaterSurface = 32;

float RimScale(float4 flags)
{
    return saturate((flags.r * 255.0 - 128.0) / 127.0);
}

bool IsWaterSurface(uint bits)
{
    return (bits & (kMaterialRimLight | kMaterialWaterSurface)) == kMaterialWaterSurface;
}

bool WarpsTotalLight(uint bits)
{
    return (bits & (kMaterialRimLight | kMaterialWarpTotal)) == kMaterialWarpTotal;
}

float RimBoost(uint bits)
{
    return float((bits >> kMaterialRimBoostShift) & kMaterialRimBoostMask) / 4.0;
}

uint MaterialBits(float4 flags)
{
    return 255u - uint(round(saturate(flags.g) * 255.0));
}

// Game draws that light themselves (forward twins, shaders/forward_twin.hlsli) keep their own constants in b0 and b1
// and reach these in register space 1 (the translated shaders' root signature, shaderapi_pipelines.cpp).
#ifdef GMODPP_GAME_DRAW
#define GMODPP_REGISTER(r) register(r, space1)
#else
#define GMODPP_REGISTER(r) register(r)
#endif
ConstantBuffer<FrameConstants> g_frame : GMODPP_REGISTER(b0);
ConstantBuffer<PassConstants> g_pass : GMODPP_REGISTER(b1);
RaytracingAccelerationStructure g_scene : GMODPP_REGISTER(t0);
SamplerState g_linearClamp : GMODPP_REGISTER(s0);
SamplerState g_linearWrap : GMODPP_REGISTER(s2);

// The light warp texture of a covered pixel, or kNoView.
uint LightWarpView(float4 flags)
{
    if ((MaterialBits(flags) & kMaterialLightWarp) == 0)
        return kNoView;
    uint slot = uint(round(flags.a * 255.0));
    if (slot == 0 || slot > g_frame.lightWarpCount)
        return kNoView;
    return g_frame.lightWarpViews[(slot - 1) / 4][(slot - 1) % 4];
}

// The pixel of a pass's thread: views whose G-buffer covers part of the screen (the viewmodel) dispatch their passes
// over a rectangle around it only.
uint2 DispatchPixel(uint2 id)
{
    if (g_frame.dispatchOriginView == kNoView)
        return id;
    ByteAddressBuffer origin = ResourceDescriptorHeap[g_frame.dispatchOriginView];
    return id + origin.Load2(32);
}

float3 WorldPositionFromDepth(int2 pixel, float depth)
{
    depth = (depth - g_frame.depthMin) / g_frame.depthScale;
    float2 uv = (float2(pixel) + 0.5) * g_frame.invSize;
    float4 world = mul(g_frame.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return world.xyz / world.w;
}

float DepthAt(int2 pixel)
{
    Texture2D<float> depth = ResourceDescriptorHeap[g_frame.depthView];
    return depth.Load(int3(clamp(pixel, int2(0, 0), int2(g_frame.size) - 1), 0));
}

// Resolved surface (written by the surface pass): xyz world normal facing the camera, w distance to the eye,
// or w = 0 where the pixel holds no G-buffer data.
float4 SurfaceAt(uint view, int2 pixel)
{
    Texture2D<float4> surface = ResourceDescriptorHeap[view];
    return surface.Load(int3(clamp(pixel, int2(0, 0), int2(g_frame.size) - 1), 0));
}

float3 SrgbToLinear(float3 color)
{
    return select(color <= 0.04045, color / 12.92, pow((max(color, 0.0) + 0.055) / 1.055, 2.4));
}

float3 LinearToSrgb(float3 color)
{
    color = max(color, 0.0);
    return select(color <= 0.0031308, color * 12.92, 1.055 * pow(color, 1.0 / 2.4) - 0.055);
}

// The game's colour targets hold lit colour (times the tonemap scale) sRGB-encoded when it renders with sRGB writes.
float3 Encode(float3 color)
{
    return g_frame.outputSrgb != 0 ? LinearToSrgb(color) : saturate(color);
}

float3 Decode(float3 color)
{
    return g_frame.outputSrgb != 0 ? SrgbToLinear(color) : color;
}

float Luminance(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }

// Rays that reach surfaces right next to a light see its falloff blow up. One sample brighter than this many
// times white on screen is scaled down to it, so it cannot leave a firefly for the denoisers.
static const float kMaxSampleBrightness = 8.0;

float3 ClampSample(float3 color)
{
    float limit = kMaxSampleBrightness / max(g_frame.tonemapScale, 1e-3);
    float luminance = Luminance(color);
    return luminance > limit ? color * (limit / luminance) : color;
}

// The 2D sky in a direction as the game draws it (src/shaderapi_capture.cpp draws it again into a cube), before the
// tonemap scale; the map's sky light where it has not been drawn.
float3 SkyRadiance(float3 direction)
{
    if (g_frame.skyCubeView == kNoView)
        return g_frame.skyColor;
    TextureCube<float4> sky = ResourceDescriptorHeap[g_frame.skyCubeView];
    float3 radiance = sky.SampleLevel(g_linearClamp, direction, 0.0).rgb;
    return all(radiance >= 0.0) ? radiance : g_frame.skyColor;
}

// Source's range fog (CalcRangeFog and BlendPixelFog in common_ps_fxc.h): the share of the fog colour over a surface at
// a view depth. fogParams.y: 1 under water, whose fog Source blends in linearly.
float FogOpacity(float viewDepth)
{
    float fog = saturate(min(g_frame.fogParams.z, viewDepth * g_frame.fogParams.w - g_frame.fogParams.x));
    return g_frame.fogParams.y > 0.0 ? fog : fog * fog;
}

// The sky's light arriving from a direction, for indirect light: the drawn sky blurred, as bright overall as the
// map's sky light (shaders/sky.hlsl).
float3 SkyLight(float3 direction)
{
    if (g_frame.skyLightView == kNoView)
        return g_frame.skyColor;
    TextureCube<float4> sky = ResourceDescriptorHeap[g_frame.skyLightView];
    return sky.SampleLevel(g_linearClamp, direction, 0.0).rgb;
}

// The game's additive passes at a surface pixel of the resolved image, linear. They were drawn with the frame's jitter, which
// shifts the image by `shift` output pixels (x right, y down); sampling there puts them back in place.
float3 AdditiveAfterResolve(int2 pixel, float2 shift)
{
    if (g_frame.additiveAfterResolve == 0)
        return 0.0;
    // Only surfaces take the additive passes; over the sky and water the capture's additive draws are not the game's.
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    if (flags.Load(int3(pixel, 0)).r < 0.5)
        return 0.0;
    Texture2D<float4> light = ResourceDescriptorHeap[g_frame.lightView];
    return Decode(light.SampleLevel(g_linearClamp, (float2(pixel) + 0.5 + shift) * g_frame.invSize, 0.0).rgb);
}

// Roughness (GGX alpha) below which a reflection is a mirror.
static const float kMirrorRoughness = 0.02;

// The GGX roughness (alpha) of a pixel's reflection: from the Phong exponent its twin wrote to the self-illumination
// target's alpha (negative for models, positive for GMod's brush Phong, 0 for none), by the Blinn-Phong to Beckmann
// relation alpha = sqrt(2 / (n + 2)) (Walter et al. 2007) carried over to GGX. Source gives no roughness; without an
// exponent, or without glossy reflections, its env_cubemap reflections are mirrors (0).
float PhongRoughness(int2 pixel)
{
    Texture2D<float4> emissive = ResourceDescriptorHeap[g_frame.emissiveView];
    float exponent = abs(emissive.Load(int3(pixel, 0)).a);
    return exponent > 0.0 && g_frame.glossyReflections != 0 ? clamp(sqrt(2.0 / (exponent + 2.0)), kMirrorRoughness, 1.0) : 0.0;
}

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float Random(inout uint state)
{
    state = Hash(state);
    return float(state >> 8) * (1.0 / 16777216.0);
}

uint PixelSeed(uint2 pixel, uint salt)
{
    return Hash(pixel.x + pixel.y * 65536u + Hash(g_frame.frameIndex * 0x9E3779B9u + salt));
}

void BuildBasis(float3 n, out float3 t, out float3 b)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

float3 CosineSample(float3 n, float u1, float u2)
{
    float3 t, b;
    BuildBasis(n, t, b);
    float r = sqrt(u1);
    float phi = 6.28318530718 * u2;
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(1.0 - u1, 0.0)));
}

float3 UniformSphere(float u1, float u2)
{
    float z = 1.0 - 2.0 * u1;
    float r = sqrt(max(1.0 - z * z, 0.0));
    float phi = 6.28318530718 * u2;
    return float3(r * cos(phi), r * sin(phi), z);
}

#endif
