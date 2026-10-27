#include "scene.hlsli"

// Volumetric light: sunlight and projected lights (flashlights, lamps) scattered by a thin, even haze between
// the eye and each surface, at half resolution.
// March (step 0): kSteps samples along each eye ray, offset per pixel and frame, each with a shadow ray to the
// sun and to every projected light; a Henyey-Greenstein phase function favours light seen against its direction.
// output0: in-scattered radiance before the tonemap scale.
// Accumulate (step 1): blends the march into last frame's result, found with the motion vectors and clipped to
// the current neighbourhood. input0: march; input1: last frame's accumulation; output0: this frame's (rgb,
// sample count); input2: 1 when last frame's accumulation may be used.

static const uint kSteps = 8;
// Sky pixels and far surfaces scatter over this much air at most.
static const float kMaxDistance = 4096.0;
static const float kPhaseG = 0.4;
static const float kMaxSamples = 16.0;

float Phase(float cosine)
{
    float g2 = kPhaseG * kPhaseG;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(1.0 + g2 - 2.0 * kPhaseG * cosine, 1e-4), 1.5));
}

// The full-resolution pixel a half-resolution pixel stands for this frame.
int2 FullPixel(int2 pixel)
{
    uint frame = g_frame.frameIndex & 3u;
    return min(pixel * 2 + int2(frame & 1u, frame >> 1), int2(g_frame.size) - 1);
}

float3 March(int2 pixel)
{
    int2 full = FullPixel(pixel);
    float4 surface = SurfaceAt(g_frame.surfaceView, full);
    float3 target = WorldPositionFromDepth(full, DepthAt(full));
    float3 toTarget = target - g_frame.eyePosition;
    float rayLength = min(surface.w > 0.0 ? surface.w : kMaxDistance, kMaxDistance);
    float3 direction = toTarget / max(sqrt(dot(toTarget, toTarget)), 1e-3);
    uint seed = PixelSeed(full, 0xB1u);
    float step = rayLength / kSteps;
    float density = g_frame.volumetricDensity;
    float sunPhase = Phase(dot(direction, g_frame.sunDirection));
    float3 light = 0.0;
    for (uint s = 0; s < kSteps; ++s) {
        float t = (s + Random(seed)) * step;
        float3 position = g_frame.eyePosition + direction * t;
        float3 scattered = 0.0;
        if (g_frame.sunEnabled != 0.0)
            scattered += g_frame.sunColor * sunPhase * SunVisibility(position);
        for (uint p = 0; p < g_frame.projectorCount; ++p) {
            float3 toLight;
            float distance;
            Projector projector = LoadProjector(p);
            float3 irradiance = ProjectorIrradiance(projector, position, normalize(projector.origin - position), toLight, distance);
            if (Luminance(irradiance) > 0.0)
                scattered += irradiance * Phase(dot(direction, -toLight)) * Transmittance(position, toLight, ShadowRayLength(true, distance), ShadowMask(true));
        }
        // Radiance scattered from irradiance E is density * phase * pi * E in the units the lighting uses.
        light += scattered * (3.14159265 * density * step * exp(-density * t));
    }
    return ClampSample(light);
}

float3 RgbToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 YCoCgToRgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

float4 Accumulate(int2 pixel, uint2 size)
{
    Texture2D<float4> march = ResourceDescriptorHeap[g_pass.input0];
    float3 current = march.Load(int3(pixel, 0)).rgb;
    if (g_pass.input2 == 0)
        return float4(current, 1.0);
    float3 moment1 = 0.0;
    float3 moment2 = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float3 c = RgbToYCoCg(march.Load(int3(clamp(pixel + int2(x, y), 0, int2(size) - 1), 0)).rgb);
            moment1 += c;
            moment2 += c * c;
        }
    }
    float3 mean = moment1 / 9.0;
    float3 deviation = sqrt(max(moment2 / 9.0 - mean * mean, 0.0));
    Texture2D<float2> motion = ResourceDescriptorHeap[g_frame.motionView];
    Texture2D<float4> history = ResourceDescriptorHeap[g_pass.input1];
    float2 uv = (float2(pixel * 2) + 1.0 + motion.Load(int3(FullPixel(pixel), 0))) * g_frame.invSize;
    if (any(uv < 0.0) || any(uv > 1.0))
        return float4(current, 1.0);
    float4 previous = history.SampleLevel(g_linearClamp, uv, 0.0);
    float3 clipped = YCoCgToRgb(clamp(RgbToYCoCg(previous.rgb), mean - 2.0 * deviation, mean + 2.0 * deviation));
    float samples = min(previous.a + 1.0, kMaxSamples);
    return float4(lerp(clipped, current, 1.0 / samples), samples);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 size = (g_frame.size + 1) / 2;
    if (any(id.xy >= size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    output[pixel] = g_pass.step == 0 ? float4(March(pixel), 1.0) : Accumulate(pixel, size);
}
