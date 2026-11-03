#include "post.hlsli"

struct VertexOutput
{
    float4 position : SV_Position;
};

[RootSignature(POST_ROOT_SIGNATURE)]
VertexOutput VSMain(uint vertex : SV_VertexID)
{
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    VertexOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// input0: the back buffer, whose 8-bit values the game wrote sRGB-encoded; output0: the view in linear colour.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void PrepareMain(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= g_post.size))
        return;
    Texture2D<float4> source = ResourceDescriptorHeap[g_postPass.input0];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    float4 color = source.Load(int3(int2(pixel) + g_post.origin, 0));
    output[pixel] = float4(SrgbToLinear(color.rgb), color.a);
}

// The camera's state, one thread for the frame. output0: the focus distance (x), whether it holds one yet (y), and
// how much of the flare's light reaches the lens per colour (zw and the next element's x): the lighting's traced
// visibility (input0) where the held weapon does not cover the light's disc.
static const uint kFocusTaps = 9;

[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(1, 1, 1)]
void CameraMain()
{
    RWStructuredBuffer<float4> camera = ResourceDescriptorHeap[g_postPass.output0];
    float4 state = camera[0];
    float focus = g_post.manualFocus;
    if (focus <= 0.0) {
        // The nearest surfaces in a small circle around the crosshair, ignoring the held weapon: the third nearest of nine
        // taps, so a thin edge in front does not pull the focus.
        float distances[kFocusTaps];
        int2 centre = int2(g_post.size / 2);
        float radius = 0.01 * float(g_post.size.y);
        [unroll]
        for (uint i = 0; i < kFocusTaps; ++i) {
            float angle = float(i) * 2.399963;
            float2 offset = i == 0 ? 0.0 : radius * sqrt(float(i) / float(kFocusTaps - 1)) * float2(cos(angle), sin(angle));
            float depth = DeviceDepth(centre + int2(round(offset)));
            float inverse = InverseViewDepth(depth);
            distances[i] = IsViewmodelDepth(depth) || inverse <= 0.0 ? 1e30 : 1.0 / inverse;
        }
        [unroll]
        for (uint a = 0; a < 3; ++a) {
            [unroll]
            for (uint b = a + 1; b < kFocusTaps; ++b) {
                float low = min(distances[a], distances[b]);
                distances[b] = max(distances[a], distances[b]);
                distances[a] = low;
            }
        }
        float target = distances[2] < 1e30 ? distances[2] : (distances[0] < 1e30 ? distances[0] : state.x);
        // The focus moves through distances by ratios, so a jump from near to far settles as evenly as the reverse.
        if (state.y != 0.0 && target > 0.0 && g_post.focusTime > 0.0) {
            float settle = 1.0 - exp(-g_post.frameSeconds / g_post.focusTime);
            target = exp(lerp(log(max(state.x, 1e-3)), log(target), settle));
        }
        focus = target > 0.0 ? target : 256.0;
    }
    float3 flare = 0.0;
    if (g_post.flareVisibilityView != kNoView) {
        Texture2D<float4> traced = ResourceDescriptorHeap[g_post.flareVisibilityView];
        flare = traced.Load(int3(0, 0, 0)).rgb;
        // The held weapon is not in the traced scene: the share of the disc's taps it leaves uncovered on screen.
        float covered = 0.0;
        float taps = 0.0;
        for (uint i = 0; i < kFocusTaps; ++i) {
            float angle = float(i) * 2.399963;
            float2 offset = i == 0 ? 0.0 : g_post.flareRadius * sqrt(float(i) / float(kFocusTaps - 1)) * float2(cos(angle), sin(angle));
            int2 tap = int2(round(g_post.flarePosition + offset));
            if (all(tap >= 0) && all(tap < int2(g_post.size))) {
                taps += 1.0;
                covered += IsViewmodelDepth(DeviceDepth(tap)) ? 1.0 : 0.0;
            }
        }
        flare *= taps > 0.0 ? 1.0 - covered / taps : 1.0;
    }
    camera[0] = float4(focus, 1.0, flare.rg);
    camera[1] = float4(flare.b, 0.0, 0.0, 0.0);
}

// Motion blur after McGuire, Hennessy, Bukowski and Osman, "A Reconstruction Filter for Plausible Motion Blur" (I3D
// 2012): each tile's longest motion over the exposure, then each tile's neighbourhood's, which the reconstruction gathers
// along.

// The motion the shutter sees at a pixel, in pixels, at most a tile long; none on the held weapon, which moves with the
// camera.
float2 ExposedMotion(int2 pixel)
{
    if (IsViewmodelDepth(DeviceDepth(pixel)))
        return 0.0;
    Texture2D<float2> motion = ResourceDescriptorHeap[g_post.motionView];
    float2 m = motion.Load(int3(pixel + g_post.origin, 0)) * g_post.shutterShare;
    float speed = sqrt(dot(m, m));
    float limit = float(g_post.blurTile);
    return speed > limit ? m * (limit / speed) : m;
}

groupshared float3 g_longest[64];

// output0: each tile's longest exposed motion (xy).
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void TileMaxMain(uint2 thread : SV_GroupThreadID, uint2 tile : SV_GroupID, uint index : SV_GroupIndex)
{
    float3 longest = 0.0;
    uint step = (g_post.blurTile + 7) / 8;
    for (uint y = 0; y < step; ++y) {
        for (uint x = 0; x < step; ++x) {
            uint2 pixel = tile * g_post.blurTile + thread * step + uint2(x, y);
            if (any(pixel >= min(g_post.size, (tile + 1) * g_post.blurTile)))
                continue;
            float2 m = ExposedMotion(int2(pixel));
            if (dot(m, m) > longest.z)
                longest = float3(m, dot(m, m));
        }
    }
    g_longest[index] = longest;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32; stride > 0; stride >>= 1) {
        if (index < stride && g_longest[index + stride].z > g_longest[index].z)
            g_longest[index] = g_longest[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        RWTexture2D<float2> output = ResourceDescriptorHeap[g_postPass.output0];
        output[tile] = g_longest[0].xy;
    }
}

// input0: the tiles' longest motion; output0: the longest of each tile's 3x3 neighbourhood.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void NeighborMaxMain(uint2 tile : SV_DispatchThreadID)
{
    if (any(tile >= g_post.tileCount))
        return;
    Texture2D<float2> tiles = ResourceDescriptorHeap[g_postPass.input0];
    float3 longest = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            int2 neighbour = clamp(int2(tile) + int2(x, y), 0, int2(g_post.tileCount) - 1);
            float2 m = tiles.Load(int3(neighbour, 0));
            if (dot(m, m) > longest.z)
                longest = float3(m, dot(m, m));
        }
    }
    RWTexture2D<float2> output = ResourceDescriptorHeap[g_postPass.output0];
    output[tile] = longest.xy;
}

static const uint kBlurSamples = 15;

// How much of a sample's colour the pixel sees as the sample's object, `distance` pixels away, moves `speed` pixels
// across it (McGuire et al.'s cone), or while both stand still across the gap (their cylinder).
float Cone(float distance, float speed)
{
    return saturate(1.0 - distance / max(speed, 1e-3));
}

float Cylinder(float distance, float speed)
{
    return 1.0 - smoothstep(0.95 * speed, 1.05 * speed, distance);
}

// 1 where a surface at view depth `a` lies in front of one at `b`, fading over 2% of the depth.
float SoftDepthCompare(float a, float b)
{
    return saturate(1.0 - (a - b) / (0.02 * min(a, b)));
}

float ViewDepth(int2 pixel)
{
    return 1.0 / max(InverseViewDepth(DeviceDepth(pixel)), 1e-6);
}

// Jimenez's interleaved gradient noise (Next Generation Post Processing in Call of Duty: Advanced Warfare, 2014): a
// fixed offset per pixel, so the gathered samples dither between neighbours.
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// input0: the view (linear), input1: the tiles' neighbourhood motion; output0: the blurred view.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void MotionBlurMain(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= g_post.size))
        return;
    Texture2D<float4> color = ResourceDescriptorHeap[g_postPass.input0];
    Texture2D<float2> neighbourhood = ResourceDescriptorHeap[g_postPass.input1];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    float4 centre = color.Load(int3(pixel, 0));
    float2 dominant = neighbourhood.Load(int3(pixel / g_post.blurTile, 0));
    float dominantLength = length(dominant);
    if (dominantLength < 0.5) {
        output[pixel] = centre;
        return;
    }
    float2 centreMotion = ExposedMotion(int2(pixel));
    float centreSpeed = max(length(centreMotion), 0.5);
    float centreDepth = ViewDepth(int2(pixel));
    float weight = 1.0 / centreSpeed;
    float3 sum = centre.rgb * weight;
    float jitter = InterleavedGradientNoise(float2(pixel)) - 0.5;
    [loop]
    for (uint i = 0; i < kBlurSamples; ++i) {
        float t = lerp(-1.0, 1.0, (float(i) + jitter + 1.0) / float(kBlurSamples + 1));
        float2 offset = dominant * (0.5 * t);
        int2 sample = clamp(int2(round(float2(pixel) + offset)), 0, int2(g_post.size) - 1);
        float distance = length(offset);
        float sampleSpeed = max(length(ExposedMotion(sample)), 0.5);
        float sampleDepth = ViewDepth(sample);
        float front = SoftDepthCompare(sampleDepth, centreDepth);
        float back = SoftDepthCompare(centreDepth, sampleDepth);
        float alpha = front * Cone(distance, sampleSpeed) + back * Cone(distance, centreSpeed) +
            2.0 * Cylinder(distance, sampleSpeed) * Cylinder(distance, centreSpeed);
        weight += alpha;
        sum += alpha * color.Load(int3(sample, 0)).rgb;
    }
    output[pixel] = float4(sum / weight, centre.a);
}
