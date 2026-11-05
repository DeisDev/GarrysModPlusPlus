#include "common.hlsli"

// Accumulates reflections over frames. A mirror image moves with the reflected point, not with the mirror, so
// the history is reprojected from the virtual point behind the mirror (the eye ray continued by the hit
// distance) and clipped to the spread of the current 3x3 neighbourhood in YCoCg, which rejects stale history. A rough
// reflection's image blurs towards the surface, so its virtual point moves towards it with the roughness (a
// heuristic: the hit distance times 1 - alpha).
// input0: this frame's reflections (rgb, hit distance; 0 for pixels without a reflection); input1: last
// frame's accumulated reflections (rgb, sample count); output0: new accumulation; step: 1 when the history may
// be used.
// The viewmodel's run (g_frame.viewmodel) keeps its own history in textures its parts write at their own pixels, so
// the mirror's place last frame and the history's taps are checked against the records its light history keeps
// (shaders/gi_temporal.hlsl: input2 last frame's records (moments, distance, tag), input3 its face normals, step this
// frame's tag), and its camera last frame is its own (g_frame.prevViewProj).

static const float kMaxSamples = 16.0;
// Mirrors kViewmodelTags in src/lighting.cpp: tags run from 1 to this.
static const uint kViewmodelTags = 1u << 24;

float3 RgbToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 YCoCgToRgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// The face normal under a pixel (shaders/primary.hlsl).
float3 FaceNormalAt(int2 pixel)
{
    Texture2D<float4> normals = ResourceDescriptorHeap[g_frame.geometricNormalView];
    return normals.Load(int3(pixel, 0)).xyz;
}

// Whether a viewmodel record written last frame lies at `tap`, at `distance` from the eye then, facing like `normal`.
bool ViewmodelRecordAt(Texture2D<float4> records, Texture2D<float4> normals, int2 tap, float previousTag, float distance, float3 normal)
{
    if (any(tap < 0) || any(tap >= int2(g_frame.size)))
        return false;
    float4 record = records.Load(int3(tap, 0));
    return record.w == previousTag && abs(record.z - distance) <= 0.5 + distance * 0.01 && dot(normals.Load(int3(tap, 0)).xyz, normal) >= 0.9;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    id.xy = DispatchPixel(id.xy);
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    bool viewmodel = g_frame.viewmodel != 0;
    Texture2D<float4> current = ResourceDescriptorHeap[g_pass.input0];
    Texture2D<float4> history = ResourceDescriptorHeap[g_pass.input1];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    float4 center = current.Load(int3(pixel, 0));
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    // A viewmodel captured in parts is lit once per part, each writing this frame's texture at its own pixels.
    if (surface.w <= 0.0 && viewmodel)
        return;
    if (surface.w <= 0.0 || (!viewmodel && g_pass.step == 0) || g_frame.historyValid == 0) {
        output[pixel] = float4(center.rgb, 1.0);
        return;
    }
    float3 position = WorldPositionFromDepth(pixel, DepthAt(pixel));
    float3 view = normalize(position - g_frame.eyePosition);
    float3 reprojected = center.a > 0.0 ? g_frame.eyePosition + view * (surface.w + center.a * (1.0 - PhongRoughness(pixel))) : position;
    float4 clip = mul(g_frame.prevViewProj, float4(reprojected, 1.0));
    float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    // The mirror itself must have been on screen at a matching distance.
    Texture2D<float2> motion = ResourceDescriptorHeap[g_frame.motionView];
    Texture2D<float> previousDistance = ResourceDescriptorHeap[g_frame.previousDistanceView];
    int2 surfacePixel = int2(floor(float2(pixel) + 0.5 + motion.Load(int3(pixel, 0))));
    float distance = previousDistance.Load(int3(pixel, 0));
    bool valid = clip.w > 0.0 && all(uv >= 0.0) && all(uv <= 1.0) && all(surfacePixel >= 0) && all(surfacePixel < int2(g_frame.size));
    float4 previous = 0.0;
    if (viewmodel) {
        Texture2D<float4> records = ResourceDescriptorHeap[g_pass.input2];
        Texture2D<float4> normals = ResourceDescriptorHeap[g_pass.input3];
        float previousTag = float(g_pass.step > 1 ? g_pass.step - 1 : kViewmodelTags);
        float3 normal = FaceNormalAt(pixel);
        valid = valid && distance > 0.0 && ViewmodelRecordAt(records, normals, surfacePixel, previousTag, distance, normal);
        if (valid) {
            // The history's texels around the virtual point that the viewmodel wrote last frame (others are older).
            float2 at = uv * float2(g_frame.size) - 0.5;
            int2 base = int2(floor(at));
            float2 f = at - float2(base);
            float weightSum = 0.0;
            [unroll]
            for (int i = 0; i < 4; ++i) {
                int2 tap = base + int2(i & 1, i >> 1);
                if (any(tap < 0) || any(tap >= int2(g_frame.size)) || records.Load(int3(tap, 0)).w != previousTag)
                    continue;
                float weight = (i & 1 ? f.x : 1.0 - f.x) * (i >> 1 ? f.y : 1.0 - f.y);
                previous += history.Load(int3(tap, 0)) * weight;
                weightSum += weight;
            }
            valid = weightSum >= 1e-3;
            previous /= max(weightSum, 1e-3);
        }
    } else if (valid) {
        float4 previousSurface = SurfaceAt(g_frame.prevSurfaceView, surfacePixel);
        valid = previousSurface.w > 0.0 && abs(previousSurface.w - distance) <= 4.0 + distance * 0.02;
        previous = history.SampleLevel(g_linearClamp, uv, 0.0);
    }
    if (!valid) {
        output[pixel] = float4(center.rgb, 1.0);
        return;
    }
    float3 moment1 = 0.0;
    float3 moment2 = 0.0;
    int2 last = int2(g_frame.size) - 1;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float3 color = RgbToYCoCg(current.Load(int3(clamp(pixel + int2(x, y), 0, last), 0)).rgb);
            moment1 += color;
            moment2 += color * color;
        }
    }
    float3 mean = moment1 / 9.0;
    float3 deviation = sqrt(max(moment2 / 9.0 - mean * mean, 0.0));
    float3 clipped = YCoCgToRgb(clamp(RgbToYCoCg(previous.rgb), mean - 1.5 * deviation, mean + 1.5 * deviation));
    float samples = min(previous.a + 1.0, kMaxSamples);
    output[pixel] = float4(lerp(clipped, center.rgb, 1.0 / samples), samples);
}
