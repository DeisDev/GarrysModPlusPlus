#ifndef GMODPP_COMMON_HLSLI
#define GMODPP_COMMON_HLSLI

// Every compute pass shares this layout. Textures and buffers are reached through descriptor heap indices
// in g_frame and g_pass. The radiance cache's constants (b2) and hash entries (u0, space2: a root descriptor, on which
// Shader Model 6.6 guarantees 64-bit atomics), and RTXDI's constants (b3), RIS buffer (u1), reservoirs (u2) and neighbour
// offsets (t1), are bound for every pass and read by those built with them (shaders/nvidia).
#define ROOT_SIGNATURE "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), CBV(b0), SRV(t0), RootConstants(num32BitConstants = 12, b1), " \
    "CBV(b2), UAV(u0, space = 2), CBV(b3), UAV(u1, space = 2), UAV(u2, space = 2), SRV(t1, space = 2), " \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, addressW = TEXTURE_ADDRESS_CLAMP), "     "StaticSampler(s2, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP)"

// Mirrors gmodpp::FrameConstants in src/lighting.h.
struct FrameConstants
{
    // The jittered view-projection's inverse relative to the eye: clip space to the point less eyePosition, over w.
    row_major float4x4 clipToEye;
    row_major float4x4 viewProj;
    row_major float4x4 prevViewProj;
    float3 eyePosition;
    float tonemapScale;
    float3 sunDirection;
    float sunEnabled;
    float3 sunColor;
    // The display's peak in paper whites (1 in SDR), for ClampSample.
    float displayPeak;
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
    // Specular anti-aliasing's strength (Settings::specularAntialiasingStrength), 0 when off (KernelRoughness2At).
    float specularAntialiasing;
    // Scene::LightLists.
    uint lightRangesView;
    uint lightIndicesView;
    uint bspNodesView;
    uint leafClustersView;
    uint clusterCount;
    // 1 for the first-person viewmodel's run, 2 when it traces its own scene with its own triangles (kLocalPlayerMask):
    // its pixels find them, with their motion, and with viewmodel_self_shadows the masks keep them, so it shadows itself
    // (src/viewmodel_casters.h).
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
    // The integrated froxel volume of volumetric light (shaders/froxels.hlsli), or kNoView, and the haze's density per
    // unit.
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
    // The map's fog lit by the map's own sun and sky (LitFog in shaders/scene.hlsli): the sun's share of the light in open
    // air.
    float fogSunShare;
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
    // What the sky shows beyond its atmosphere (NightSkyConstants in shaders/night_sky.hlsli) to rays that leave the
    // map, or kNoView.
    uint nightSkyView;
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
    // How the baked bounce light's sun and sky parts (shaders/baked.hlsli) scale: the sun's and sky's light now over
    // what they were baked with.
    float3 bakedSunScale;
    float bakedSkyScale;
    // The 2D sky's motion since the last main view (shaders/primary.hlsl): the rows of the turn from this view's
    // directions to last view's, for the directions within skyMotionDisc.w (a cosine) of skyMotionDisc.xyz.
    float4 skyMotion[3];
    float4 skyMotionDisc;
    // Map lights' colour (the menu's lamp tint, linear), and how strongly glowing surfaces light others (HitEmission).
    float3 lightTint;
    float emissiveScale;
    // The baked bounce light's sky part per colour channel, with bakedSkyScale: the sky's colour now over the map's.
    float3 bakedSkyTint;
    // The sun's angular radius for soft shadows (SunDiscDirectionAt), and 1 - its cosine.
    float sunAngularRadius;
    float sunConeSpan;
    // The froxel volume's layout (shaders/froxels.hlsli), this frame's jitter of its sample points within their froxels,
    // and last frame's slices, which its history is found in.
    uint3 froxelDims;
    float2 froxelUvScale;
    float froxelStart;
    float froxelRange;
    float3 froxelJitter;
    float prevFroxelStart;
    float prevFroxelRange;
    // How the light in open air compares with the map's own (LitFog): the sun's per colour channel and the sky's, and the
    // map's own irradiance averaged over all directions (a quarter of its sun's, half its sky's).
    float3 fogSunScale;
    float fogSkyScale;
    float fogAirLight;
    // The clouds over the procedural sky (shaders/clouds.hlsl), or kNoView: their cube, which reflections and indirect
    // light see (CloudsToward), and the map of the sun's transmittance through them (CloudShadowAt in
    // shaders/scene.hlsli), two square cascades side by side over the plane across the sun.
    uint cloudCubeView;
    uint cloudShadowView;
    uint4 lightWarpViews[16];
    // A point's place on the cloud shadow map's plane, in units: (dot(position, cloudShadowU.xyz) + cloudShadowU.w, the
    // same with V); times each cascade's scale (1 / its width) plus a half, its texture coordinates in the cascade, kept
    // half a texel (cloudShadowEdge) inside it.
    float4 cloudShadowU;
    float4 cloudShadowV;
    float cloudShadowFineScale;
    float cloudShadowCoarseScale;
    float cloudShadowEdge;
    // How much the cloud shadows darken (Settings::cloudShadowStrength, 0 without them).
    float cloudShadowStrength;
    // Ground mist (Settings::groundMist; MistSigma in shaders/froxels.hlsli): its albedo, extinction per unit at
    // mistBase, 1 / the height over which it thins by e, its noise's turbulence, 1 / its scale and drift, its phase
    // function's g, the light it scatters per unit of opacity beyond the froxel volume (MistFarLight), and whether it is on.
    float3 mistColor;
    float mistDensity;
    float mistBase;
    float mistInvFalloff;
    float mistTurbulence;
    float mistInvScale;
    float3 mistOffset;
    float mistPhaseG;
    float3 mistFarColor;
    float mistEnabled;
    // Aerial perspective (shaders/sky_atmosphere.hlsl step 7, AerialAt): its volume over the view (light and
    // transmittance) and the cloud shadows' change of the 2D sky's light, or kNoView; the 3D skybox's distances this
    // frame (shaders/primary.hlsl), or kNoView; the volume's reach in kilometres.
    uint aerialView;
    uint aerialSkyView;
    uint skyboxDistanceView;
    float aerialRangeKm;
    // Particle shadows (shaders/particle_shadows.hlsl, ParticleShadowAt): the shadow map over the plane across the sun, or
    // kNoView, and the eye's place along the sun its depths are measured from; the map's scale (1 / its width in units)
    // and centre on the plane, the extinction volume's cells per unit and corner, and the sprites splatted into it.
    uint particleShadowView;
    float particleShadowAlong;
    float particleShadowScale;
    float particleVolumeInvCell;
    float3 particleVolumeOrigin;
    uint particleCount;
    float2 particleShadowCentre;
    // Aerial perspective's volume spans the view widened by this share on each side (as the clouds' history does).
    float aerialOverscan;
    // The dynamic lights (dynamicLightsView) in three runs: lights sampled with the map's, then from dynamicExactFirst the
    // strongest glowing model patches, lit exactly, then from dynamicFlashFirst the flashes of effects, lit exactly and
    // only directly (Scene::SetDynamicLights).
    uint dynamicExactFirst;
    uint dynamicFlashFirst;
    uint3 padding11;
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
    uint input5;
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
    float4 offset = mul(g_frame.clipToEye, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return g_frame.eyePosition + offset.xyz / offset.w;
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

float Luminance(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }

// Rays that reach surfaces right next to a light see its falloff blow up. One sample brighter than this many
// times the display's peak on screen is scaled down to it, so it cannot leave a firefly for the denoisers; on an HDR
// display the limit rises with the peak, so glints the display can show are kept.
static const float kMaxSampleBrightness = 8.0;

float3 ClampSample(float3 color)
{
    float limit = kMaxSampleBrightness * max(g_frame.displayPeak, 1.0) / max(g_frame.tonemapScale, 1e-3);
    float luminance = Luminance(color);
    return luminance > limit ? color * (limit / luminance) : color;
}

// The clouds toward a direction (shaders/clouds.hlsl): the light they and the air in front of them add (rgb) and the
// share of the sky behind they let through (a).
float4 CloudsToward(float3 direction)
{
    if (g_frame.cloudCubeView == kNoView)
        return float4(0.0, 0.0, 0.0, 1.0);
    TextureCube<float4> clouds = ResourceDescriptorHeap[g_frame.cloudCubeView];
    return clouds.SampleLevel(g_linearClamp, direction, 0.0);
}

// The 2D sky in a direction as the game draws it (src/shaderapi_capture.cpp draws it again into a cube), with the clouds
// over it, before the tonemap scale; the map's sky light where it has not been drawn.
float3 SkyRadiance(float3 direction)
{
    if (g_frame.skyCubeView == kNoView)
        return g_frame.skyColor;
    TextureCube<float4> sky = ResourceDescriptorHeap[g_frame.skyCubeView];
    float3 radiance = sky.SampleLevel(g_linearClamp, direction, 0.0).rgb;
    if (!all(radiance >= 0.0))
        return g_frame.skyColor;
    float4 clouds = CloudsToward(direction);
    return radiance * clouds.a + clouds.rgb;
}

// Source's range fog (CalcRangeFog and BlendPixelFog in common_ps_fxc.h): the share of the fog colour over a surface at
// a distance from the eye. Source measures it along the view axis, so turning the camera changed the fog on a surface;
// the fog here follows the distance itself. fogParams.y: 1 under water, whose fog Source blends in linearly.
float FogOpacity(float distance)
{
    float fog = saturate(min(g_frame.fogParams.z, distance * g_frame.fogParams.w - g_frame.fogParams.x));
    return g_frame.fogParams.y > 0.0 ? fog : fog * fog;
}

// The light that shows the map's fog now over the map's own, by colour (LitFog in shaders/scene.hlsli, in open air): day
// and night, and the sun's and sky's strengths and colours.
float3 FogDaylight()
{
    return g_frame.fogSunShare * g_frame.fogSunScale + (1.0 - g_frame.fogSunShare) * g_frame.fogSkyScale;
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

// The game's additive passes at a surface pixel of the resolved image (linear light, as the frame holds it). They were drawn
// with the frame's jitter, which shifts the image by `shift` output pixels (x right, y down); sampling there puts them
// back in place.
float3 AdditiveAfterResolve(int2 pixel, float2 shift)
{
    if (g_frame.additiveAfterResolve == 0)
        return 0.0;
    // Only surfaces take the additive passes; over the sky and water the capture's additive draws are not the game's.
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    if (flags.Load(int3(pixel, 0)).r < 0.5)
        return 0.0;
    Texture2D<float4> light = ResourceDescriptorHeap[g_frame.lightView];
    return light.SampleLevel(g_linearClamp, (float2(pixel) + 0.5 + shift) * g_frame.invSize, 0.0).rgb;
}

// Roughness (GGX alpha) below which a reflection is a mirror.
static const float kMirrorRoughness = 0.02;

// The GGX roughness (alpha) of a pixel's reflection: from the Phong exponent its twin wrote to the self-illumination
// target's alpha (negative for models, positive for GMod's brush Phong, 0 for none), by the Blinn-Phong to Beckmann
// relation alpha = sqrt(2 / (n + 2)) (Walter et al. 2007) carried over to GGX. Source gives no roughness; without an
// exponent, or without glossy reflections, its env_cubemap reflections are mirrors (0).
// Source's units are inches (Valve Developer Community, "Dimensions"): kilometres per unit.
static const float kKilometresPerUnit = 2.54e-5;
// The least spread along the sun a cloud's optical depth is taken to have (shaders/clouds.hlsl), in kilometres.
static const float kCloudShadowMinSpread = 0.1;

// A point in the map's space, or for `inSkybox` in the 3D skybox's (scaled about its camera).
float3 MapSpace(float3 position, bool inSkybox)
{
    if (inSkybox && g_frame.worldToSky > 0.0)
        return (position - float3(g_frame.skyCameraX, g_frame.skyCameraY, g_frame.skyCameraZ)) / g_frame.worldToSky;
    return position;
}

// The share of the sun's light the clouds let through to a point (shaders/clouds.hlsl's shadow map: the fine cascade
// where it covers the point, else the coarse one). The map holds each line's optical depth toward the sun and where
// along the sun it lies (mean and spread), so a point takes the share of the depth beyond it: the whole depth below the
// clouds, less within them, little above them.
float CloudShadowAt(float3 origin, bool inSkybox)
{
    if (g_frame.cloudShadowView == kNoView)
        return 1.0;
    float3 position = MapSpace(origin, inSkybox);
    float2 plane = float2(dot(position, g_frame.cloudShadowU.xyz) + g_frame.cloudShadowU.w, dot(position, g_frame.cloudShadowV.xyz) + g_frame.cloudShadowV.w);
    Texture2D<float4> shadow = ResourceDescriptorHeap[g_frame.cloudShadowView];
    float edge = g_frame.cloudShadowEdge;
    float2 uv = plane * g_frame.cloudShadowFineScale + 0.5;
    float cascade = 0.0;
    if (any(abs(uv - 0.5) > 0.5 - edge)) {
        uv = plane * g_frame.cloudShadowCoarseScale + 0.5;
        cascade = 1.0;
    }
    uv = clamp(uv, edge, 1.0 - edge);
    float4 texel = shadow.SampleLevel(g_linearClamp, float2((uv.x + cascade) * 0.5, uv.y), 0.0);
    float along = kKilometresPerUnit * dot(position, g_frame.sunDirection);
    float beyond = saturate(0.5 + (texel.y - along) / (2.0 * max(texel.z, kCloudShadowMinSpread)));
    return lerp(1.0, exp(-texel.x * beyond), g_frame.cloudShadowStrength);
}

// The share of the sun's light last frame's smoke lets through to a point (shaders/particle_shadows.hlsl): the map holds
// each line's optical depth toward the sun and where along it that lies, so the point takes the part beyond it.
float ParticleShadowAt(float3 position)
{
    if (g_frame.particleShadowView == kNoView)
        return 1.0;
    float2 plane = float2(dot(position, g_frame.cloudShadowU.xyz), dot(position, g_frame.cloudShadowV.xyz)) - g_frame.particleShadowCentre;
    float2 uv = plane * g_frame.particleShadowScale + 0.5;
    if (any(abs(uv - 0.5) > 0.5))
        return 1.0;
    Texture2D<float4> shadow = ResourceDescriptorHeap[g_frame.particleShadowView];
    float4 texel = shadow.SampleLevel(g_linearClamp, uv, 0.0);
    float along = dot(position, g_frame.sunDirection) - g_frame.particleShadowAlong;
    return exp(-texel.x * saturate(0.5 + (texel.y - along) / (2.0 * max(texel.z, 1.0))));
}

// The share of the sun's light clouds and smoke let through to a point (in the 3D skybox's space with `inSkybox`, where no
// smoke is).
float SunOcclusionAt(float3 position, bool inSkybox)
{
    float shade = CloudShadowAt(position, inSkybox);
    return inSkybox ? shade : shade * ParticleShadowAt(position);
}

// Specular anti-aliasing (Tokuyoshi and Kaplanyan, "Stable Geometric Specular Antialiasing with Projected-Space NDF
// Filtering", JCGT 2021): the squared roughness of the filter kernel over the normals one pixel covers, which
// shaders/primary.hlsl keeps in the face normal target's alpha, times the setting's strength.
float KernelRoughness2At(int2 pixel)
{
    if (g_frame.specularAntialiasing <= 0.0)
        return 0.0;
    Texture2D<float4> normals = ResourceDescriptorHeap[g_frame.geometricNormalView];
    return g_frame.specularAntialiasing * normals.Load(int3(clamp(pixel, int2(0, 0), int2(g_frame.size) - 1), 0)).w;
}

// A GGX alpha widened by the kernel: the two lobes convolved, their squared roughnesses added.
float WidenAlpha(float alpha, float kernel2)
{
    return sqrt(saturate(alpha * alpha + kernel2));
}

// A pixel's reflection lobe: GGX from its Phong exponent (alpha = sqrt(2 / (n + 2))), a mirror without one, both
// widened by the specular anti-aliasing kernel, so a mirror whose normals vary within the pixel turns glossy.
float PhongRoughness(int2 pixel)
{
    if (g_frame.glossyReflections == 0)
        return 0.0;
    Texture2D<float4> emissive = ResourceDescriptorHeap[g_frame.emissiveView];
    float exponent = abs(emissive.Load(int3(pixel, 0)).a);
    float kernel2 = KernelRoughness2At(pixel);
    if (exponent > 0.0)
        return clamp(WidenAlpha(sqrt(2.0 / (exponent + 2.0)), kernel2), kMirrorRoughness, 1.0);
    return kernel2 > kMirrorRoughness * kMirrorRoughness ? sqrt(kernel2) : 0.0;
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
