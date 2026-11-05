#include "froxels.hlsli"

// The upscaler's output (Ray Reconstruction's or FSR's) for the composite, with the game's additive passes added
// (AdditiveAfterResolve). Where a pixel and the eight around it show the open 2D sky, the sky as the game drew it at
// the output's resolution stands in, behind the haze shaders/deferred_lighting.hlsl puts in front of it: the upscaler
// takes the sky at its render resolution, sampled at pixel centres, and smoothed the clouds' detail away as if it were
// noisy light (2026-11-03). Silhouettes against the sky keep the upscaler's anti-aliased edges. Under water the eye sees
// no open sky: where nothing is drawn the deferred pass shows the water's light, and the game's draw there is Source's
// unlit view into the air.
//
// Geometry thinner than a pixel (ropes, wires) covers a pixel in a few of the jitter's phases only and leaves gaps
// between the pixels it covers in any one frame; taking the sky in those gaps threw away what the upscaler had gathered
// there. So a pixel is open sky only once no geometry has come near it for the jitter's whole cycle, as long as the image
// there has not moved by more than a pixel meanwhile: remembered for a cycle while the camera turned, the upscaler's
// softer sky trailed a cycle's turn behind every silhouette (2026-11-04).
//
// input0: the upscaler's output; input1, input2: the jitter's image shift in output pixels (float bits); input3: the 3D
// skybox's distance per pixel (0 where it drew nothing), or kNoView; input4: last frame's output1, or kNoView; input5:
// the frames the jitter takes to come round; step: the render width (low 16 bits) and height (high 16 bits), at which
// the motion vectors are; output0: linear light; output1: the frames since geometry was near each pixel (0 now, at most
// 255), carried along the motion vectors.

static const uint kNeverNear = 255;

// Whether the main view's depth and the 3D skybox leave the pixels around `pixel` empty.
bool OpenSky(int2 pixel)
{
    Texture2D<float> skybox = ResourceDescriptorHeap[g_pass.input3 != kNoView ? g_pass.input3 : g_frame.depthView];
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            int2 at = clamp(pixel + int2(x, y), int2(0, 0), int2(g_frame.size) - 1);
            if (DepthAt(at) < 1.0 || (g_pass.input3 != kNoView && skybox.Load(int3(at, 0)) > 0.0))
                return false;
        }
    }
    return true;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<uint> frames = ResourceDescriptorHeap[g_pass.output1];
    float2 shift = float2(asfloat(g_pass.input1), asfloat(g_pass.input2));

    // The motion vectors are at render resolution, in its pixels.
    uint2 renderSize = uint2(g_pass.step & 0xFFFFu, g_pass.step >> 16);
    float2 scale = float2(g_frame.size) / float2(renderSize);
    Texture2D<float2> motionVectors = ResourceDescriptorHeap[g_frame.motionView];
    float2 motion = motionVectors.Load(int3(min(int2((float2(pixel) + 0.5) / scale), int2(renderSize) - 1), 0)) * scale;
    uint since = 0;
    if (OpenSky(pixel)) {
        since = kNeverNear;
        int2 last = int2(floor(float2(pixel) + 0.5 + motion));
        if (g_pass.input4 != kNoView && all(last >= 0) && all(last < int2(g_frame.size))) {
            Texture2D<uint> history = ResourceDescriptorHeap[g_pass.input4];
            since = min(history.Load(int3(last, 0)) + 1, kNeverNear);
        }
    }
    frames[pixel] = since;
    bool geometryNear = since < g_pass.input5 && float(since) * length(motion) <= 1.0;

    if (g_frame.eyeWater == kNoView && !geometryNear) {
        // Read where the jitter moved the sky this frame, so it holds still.
        Texture2D<float4> drawn = ResourceDescriptorHeap[g_frame.colorView];
        float3 sky = drawn.SampleLevel(g_linearClamp, (float2(pixel) + 0.5 + shift) * g_frame.invSize, 0.0).rgb;
        // Under the map's fog the deferred pass leaves the sky as the game drew it.
        if (g_frame.fogEnabled == 0) {
            float3 direction = normalize(WorldPositionFromDepth(pixel, 0.5) - g_frame.eyePosition);
            float4 haze = FroxelLightOverSky((float2(pixel) + 0.5) * g_frame.invSize, direction);
            sky = sky * haze.a + haze.rgb * g_frame.tonemapScale;
        }
        output[pixel] = float4(sky, 1.0);
        return;
    }
    Texture2D<float4> source = ResourceDescriptorHeap[g_pass.input0];
    float3 color = source.Load(int3(pixel, 0)).rgb;
    output[pixel] = float4(color + AdditiveAfterResolve(pixel, shift), 1.0);
}
