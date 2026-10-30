#include "scene.hlsli"

// Ray-traced ambient occlusion: contact shadows finer than the denoised indirect light keeps, applied to it in
// the lighting pass.
// Trace (step 0): g_frame.aoRays cosine-weighted rays of kRadius per pixel; a hit at distance t counts as
// t / kRadius of open sky. The result is blended into last frame's accumulation, found with the motion vectors.
// input0: last frame's accumulation (r: occlusion, g: sample count), or none; output0: this frame's.
// Blur (step 1): a 5x5 blur over the same surface. input0: accumulation; output0: ambient occlusion (r).

static const float kRadius = 48.0;
static const float kMaxSamples = 16.0;

float Trace(int2 pixel, float4 surface)
{
    float3 position = LitPosition(WorldPositionFromDepth(pixel, DepthAt(pixel)));
    float3 geometricNormal = GeometricNormalAt(pixel);
    float3 origin = SurfaceRayOrigin(position, geometricNormal, surface.w);
    uint seed = PixelSeed(pixel, 0xA0u);
    float open = 0.0;
    for (uint r = 0; r < g_frame.aoRays; ++r) {
        float3 direction = AboveSurface(CosineSample(surface.xyz, Random(seed), Random(seed)), geometricNormal);
        HitInfo hit;
        open += TraceHit(origin, direction, kRadius, g_frame.indirectMask, hit, g_frame.indirectRayFlags) ? saturate(hit.t / kRadius) : 1.0;
    }
    return open / g_frame.aoRays;
}

float2 History(int2 pixel, float4 surface)
{
    if (g_pass.input0 == kNoView || g_frame.historyValid == 0)
        return 0.0;
    Texture2D<float2> motion = ResourceDescriptorHeap[g_frame.motionView];
    Texture2D<float> previousDistance = ResourceDescriptorHeap[g_frame.previousDistanceView];
    Texture2D<float2> history = ResourceDescriptorHeap[g_pass.input0];
    // Bilinear taps, each rejected where the surface differs, as gi_temporal.hlsl reads its history: the nearest
    // pixel alone lost the history at every subpixel move.
    float2 previous = float2(pixel) + motion.Load(int3(pixel, 0));
    int2 base = int2(floor(previous));
    float2 f = previous - float2(base);
    float distance = previousDistance.Load(int3(pixel, 0));
    float2 accumulated = 0.0;
    float weightSum = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        int2 tap = base + int2(i & 1, i >> 1);
        if (any(tap < 0) || any(tap >= int2(g_frame.size)))
            continue;
        float4 previousSurface = SurfaceAt(g_frame.prevSurfaceView, tap);
        if (previousSurface.w <= 0.0 || abs(previousSurface.w - distance) > 2.0 + distance * 0.01 || dot(previousSurface.xyz, surface.xyz) < 0.9)
            continue;
        float weight = (i & 1 ? f.x : 1.0 - f.x) * (i >> 1 ? f.y : 1.0 - f.y);
        accumulated += history.Load(int3(tap, 0)) * weight;
        weightSum += weight;
    }
    return weightSum < 1e-3 ? 0.0 : accumulated / weightSum;
}

float Blur(int2 pixel, float4 surface)
{
    Texture2D<float2> accumulation = ResourceDescriptorHeap[g_pass.input0];
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    float tolerance = 1.0 + surface.w * 0.004;
    float sum = 0.0;
    float weightSum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            int2 tap = pixel + int2(x, y);
            if (any(tap < 0) || any(tap >= int2(g_frame.size)))
                continue;
            float4 tapSurface = SurfaceAt(g_frame.surfaceView, tap);
            if (tapSurface.w <= 0.0)
                continue;
            float plane = abs(dot(surface.xyz, WorldPositionFromDepth(tap, DepthAt(tap)) - position)) / tolerance;
            float weight = exp(-plane * plane) * pow(saturate(dot(surface.xyz, tapSurface.xyz)), 16.0);
            sum += accumulation.Load(int3(tap, 0)).r * weight;
            weightSum += weight;
        }
    }
    return weightSum > 0.0 ? sum / weightSum : 1.0;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    id.xy = DispatchPixel(id.xy);
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    if (g_pass.step == 0) {
        RWTexture2D<float2> output = ResourceDescriptorHeap[g_pass.output0];
        if (surface.w <= 0.0 || g_frame.traceMask == 0) {
            output[pixel] = float2(1.0, 0.0);
            return;
        }
        float2 history = History(pixel, surface);
        float samples = min(history.g + 1.0, kMaxSamples);
        output[pixel] = float2(lerp(history.r, Trace(pixel, surface), 1.0 / samples), samples);
    } else {
        RWTexture2D<float> output = ResourceDescriptorHeap[g_pass.output0];
        output[pixel] = surface.w > 0.0 ? Blur(pixel, surface) : 1.0;
    }
}
