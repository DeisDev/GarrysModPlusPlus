#ifndef POST_HLSLI
#define POST_HLSLI

// What the camera effects' passes share (src/post_effects.cpp): they run on the 3D view after the game's
// post-processing, in linear colour, in the view's own pixels (0 at its corner).

#define POST_ROOT_SIGNATURE \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), CBV(b0), RootConstants(num32BitConstants = 8, b1), " \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, " \
    "addressW = TEXTURE_ADDRESS_CLAMP), " \
    "StaticSampler(s1, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, " \
    "addressW = TEXTURE_ADDRESS_CLAMP)"

static const uint kNoView = 0xFFFFFFFFu;

// Mirrors PostConstants in src/post_effects.cpp, in 16-byte rows.
struct PostConstants
{
    uint2 size;
    float2 invSize;
    // The view's corner in the back buffer, the depth buffer and the motion vectors, and the depth buffer's size.
    int2 origin;
    float2 invDepthSize;
    // The depth buffer, and motion in pixels toward last frame's position (shaders/primary.hlsl) or kNoView. Device
    // depth to 1 / view depth is depth * depthScale + depthBias.
    uint depthView;
    uint motionView;
    float depthScale;
    float depthBias;
    // The held weapon is drawn with DepthRange(0, 0.1): depths below viewmodelDepth (above it with reversed depth).
    float viewmodelDepth;
    uint reverseDepth;
    uint padding4;
    float frameSeconds;
    // The camera's state (CameraMain in shaders/post/post.hlsl): the focus distance and the flare's visibility. Depth of
    // field (FidelityFX DoF): the circle of confusion's radius in half-resolution pixels is
    // cocScale * focalLength * (focus / viewDepth - 1) / (focus - focalLength), within +-cocLimit, gathered over at
    // most maxRings rings, at the focus distance CameraMain keeps: manualFocus when it is positive, else what the
    // crosshair points at, settling over focusTime seconds.
    uint cameraView;
    float cocScale;
    float focalLength;
    float cocLimit;
    uint maxRings;
    float manualFocus;
    float focusTime;
    // FidelityFX DoF's working textures: the half-resolution colour (an SRV of all its mips, each mip's UAV), the tiles'
    // circle of confusion range and its dilation, the near and far fields (UAV, then SRV) and its global maximum.
    uint dofBilat;
    uint4 dofBilatMips;
    uint dofTileRadiusUav;
    uint dofTileRadius;
    uint dofDilatedUav;
    uint dofDilated;
    uint dofNearUav;
    uint dofNear;
    uint dofFarUav;
    uint dofFar;
    uint dofGlobals;
    // Motion blur: the share of a frame's motion the shutter sees, and the tiles' size in pixels and count.
    float shutterShare;
    uint blurTile;
    uint padding0;
    uint2 tileCount;
    // Lens flare: the light's position on screen (pixels), its brightness on screen per colour, whether it lies in front
    // of the eye, the starburst's strength, the visibility the lighting traced (a 1x1 texture, or kNoView without a
    // light) and the disc's radius in pixels.
    float2 flarePosition;
    float3 flareColor;
    float flareInFront;
    float flareStarburst;
    uint flareVisibilityView;
    float flareRadius;
    // Lens, by each pixel's field angle (focalPixels is the focal length in pixels): lateral chromatic aberration
    // (FidelityFX Lens's dispersion strength), radial distortion (the coefficient of tan^2 of the angle) and the scale
    // that keeps the corners filled, vignetting (times the cos^4 law's exponent), film grain, contrast adaptive
    // sharpening (FidelityFX CAS).
    float chromaticAberration;
    float distortion;
    float distortionScale;
    float vignette;
    float grainAmount;
    float grainScale;
    uint grainSeed;
    float sharpness;
    // Colour grading: exposure times white balance per channel, the power around middle grey and saturation, when
    // gradeEnabled.
    uint gradeEnabled;
    float3 gradeGain;
    float gradeContrast;
    float gradeSaturation;
    // The motion vectors' resolution over the view's (below 1 when the lighting was upscaled).
    float2 motionScale;
    float focalPixels;
    // The tone curve (shaders/tone_curve.hlsli) up to the display's peak in paper whites, and the engine's colour
    // correction after it: its look-up count, default weight, weights and 32^3 volumes.
    uint toneCurve;
    float highlightRolloff;
    float tonePeak;
    uint ccCount;
    float ccDefaultWeight;
    // The HDR brightness boost's state (ExposureMain in shaders/post/post.hlsl), kNoView when off or in SDR, and Source's
    // auto exposure target: the share of pixels counted as bright and where the brightest of them start, in the view's
    // white (mat_tonemap_percent_bright_pixels and mat_tonemap_percent_target over 100).
    uint exposureView;
    float brightShare;
    float brightTarget;
    float4 ccWeights;
    uint4 ccViews;
    // In HDR, paper white (0 in SDR) and the HUD's and menu's white in nits (0: paper white). The frame's 1 is the HUD's
    // white. The most the brightness boost may brighten the view by (2 to the power of its stops).
    float paperWhite;
    float uiWhite;
    float exposureLimit;
    uint padding6;
};

// Per pass: descriptor indices and a parameter.
struct PostPass
{
    uint input0;
    uint input1;
    uint input2;
    uint output0;
    uint output1;
    uint output2;
    uint step;
    uint padding;
};

ConstantBuffer<PostConstants> g_post : register(b0);
ConstantBuffer<PostPass> g_postPass : register(b1);
SamplerState g_postLinear : register(s0);
SamplerState g_postPoint : register(s1);

float3 SrgbToLinear(float3 color)
{
    return select(color <= 0.04045, color / 12.92, pow((max(color, 0.0) + 0.055) / 1.055, 2.4));
}

float3 LinearToSrgb(float3 color)
{
    color = max(color, 0.0);
    return select(color <= 0.0031308, color * 12.92, 1.055 * pow(color, 1.0 / 2.4) - 0.055);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float DeviceDepth(int2 pixel)
{
    Texture2D<float> depth = ResourceDescriptorHeap[g_post.depthView];
    return depth.Load(int3(pixel + g_post.origin, 0));
}

bool IsViewmodelDepth(float depth)
{
    return g_post.reverseDepth != 0 ? depth > g_post.viewmodelDepth : depth < g_post.viewmodelDepth;
}

// 1 / the view depth at a device depth; the far plane's where the depth is clear (the sky and the 3D skybox).
float InverseViewDepth(float depth)
{
    return max(depth * g_post.depthScale + g_post.depthBias, 0.0);
}

#endif
