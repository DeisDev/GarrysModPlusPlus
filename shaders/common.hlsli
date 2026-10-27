#ifndef GMODPP_COMMON_HLSLI
#define GMODPP_COMMON_HLSLI

// Every compute pass shares this layout. Textures and buffers are reached through descriptor heap indices
// in g_frame and g_pass.
#define ROOT_SIGNATURE "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), CBV(b0), SRV(t0), RootConstants(num32BitConstants = 12, b1), " \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP), "     "StaticSampler(s1, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP), "     "StaticSampler(s2, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP)"

// Mirrors gmodpp::FrameConstants in src/effects.h.
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
    // occlusion), and later bounces (shadowless props, by FoliageShadows).
    uint sunShadowMask;
    uint indirectMask;
    uint bounceMask;
    // Ray flags added to indirect light and ambient occlusion rays.
    uint indirectRayFlags;
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

ConstantBuffer<FrameConstants> g_frame : register(b0);
ConstantBuffer<PassConstants> g_pass : register(b1);
RaytracingAccelerationStructure g_scene : register(t0);
SamplerState g_linearClamp : register(s0);
SamplerState g_pointWrap : register(s1);
SamplerState g_linearWrap : register(s2);

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
