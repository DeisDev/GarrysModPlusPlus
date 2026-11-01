#include "scene.hlsli"

// Caustics and the water's sun map, in two squares around the eye: a fine one (FrameConstants' caustics) and a coarse
// one that covers the whole map (its coarse caustics). Cells are lines of flat water's bent sunlight, indexed where they
// cross the plane of the highest water surface (SunLineCell in shaders/water_body.hlsli); one photon follows each cell's
// line from that plane to the first water surface on it.
// Caustics: the photon's sunlight bends at the rippled normal (WaterSurfaceNormal) and crosses the water to the first
// surface it meets. Where the water were flat, it would have stayed on its line; it is counted at the cell of the line
// through its landing spot, so cells whose surfaces the ripples gather light on count more than one photon and cells they
// leave in shadow fewer. The count is a ratio, over the photons that left the cells around (none where no water lies on
// the lines: 1 there).
// The sun map: per cell the sunlight that reaches the line's water surface from above (SunVisibilityAlong, per colour)
// and how far along the line below the plane the light goes before something stops it (WaterSunMapAt), so the water's
// own light, and the light of what lies in it, is shadowed without rays of their own.
// Pass arguments: input1, input2: the square's corner (float bits), input3: 1 / its size; step bits 0-1: 0 clears output0
// (the photons counted at each cell, fixed point, then from x = cells on the photons that left it) and output2 (the sun
// map), 1 traces the photons, 2 resolves the counts (input0) into output1, smoothed over 3x3 cells; bit 2: the coarse
// square, whose photons count no caustics (its cells are wider than ripples).

static const uint kCausticsCells = 1024;
static const uint kCoarseCells = 512;
// Photon counts are summed in fixed point: one photon's full weight.
static const float kPhotonScale = 1024.0;

uint Cells() { return (g_pass.step & 4u) != 0 ? kCoarseCells : kCausticsCells; }
float2 Origin() { return float2(asfloat(g_pass.input1), asfloat(g_pass.input2)); }
float2 CellSize() { return 1.0 / (asfloat(g_pass.input3) * Cells()); }

void Splat(RWTexture2D<uint> counts, float2 cell, float weight)
{
    float2 base = floor(cell - 0.5);
    float2 f = cell - 0.5 - base;
    int2 c = int2(base);
    float weights[4] = {(1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y};
    int2 offsets[4] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)};
    [unroll]
    for (uint k = 0; k < 4; ++k) {
        int2 target = c + offsets[k];
        if (all(target >= 0) && all(target < int(Cells())))
            InterlockedAdd(counts[target], uint(weight * weights[k] * kPhotonScale + 0.5));
    }
}

void TracePhoton(uint2 id, RWTexture2D<uint> counts, RWTexture2D<float4> sunMap)
{
    float2 size = CellSize();
    float3 toSun = g_frame.sunDirection;
    float3 up = RefractedSunDirection(toSun);
    // The cell's line, from a unit above the plane of the highest surface, to its first water surface.
    float3 start = float3(Origin() + (float2(id) + 0.5) * size, g_frame.waterTop) + up / up.z;
    HitInfo surfaceHit;
    if (!TraceHit(start, -up, 65536.0, kWaterMask, surfaceHit))
        return;
    uint water = WaterBodyOfTriangle(surfaceHit.instance, surfaceHit.primitive);
    if (water == kNoWater)
        return;
    float3 surface = start - up * surfaceHit.t;
    float surfaceAlong = surfaceHit.t - 1.0 / up.z;
    uint mask = g_frame.sunShadowMask & ~kWaterMask;
    // The sun map: the light above the surface, and how far along the line it goes under it.
    float3 above = SunVisibilityAlong(surface + float3(0.0, 0.0, kWaterSurfaceOffset), toSun, mask, RAY_FLAG_NONE);
    float3 under = surface - float3(0.0, 0.0, kWaterSurfaceOffset);
    HitInfo blocked;
    float reach = TraceHit(under, -up, 65536.0, mask, blocked) ? blocked.t : kOpenWater;
    sunMap[id] = float4(above, surfaceAlong + reach);
    if ((g_pass.step & 4u) != 0)
        return;
    InterlockedAdd(counts[id + uint2(Cells(), 0)], uint(kPhotonScale));
    float3 normal = WaterSurfaceNormal(water, LoadWaterBody(water), surface);
    float cosine = dot(normal, toSun);
    if (cosine <= 0.0)
        return;
    float3 bent = refract(-toSun, normal, 1.0 / kWaterIor);
    if (dot(bent, bent) <= 0.0 || bent.z >= -1e-3)
        return;
    // The sunlight a facet of the ripple takes over its cell, and the share it lets in, against flat water's.
    float flatCosine = toSun.z;
    float weight = (cosine / max(normal.z, 1e-3)) / flatCosine * (1.0 - Fresnel(cosine, kWaterIor)) / max(1.0 - Fresnel(flatCosine, kWaterIor), 1e-3);
    HitInfo landing;
    if (!TraceHit(under, bent, 65536.0, mask, landing))
        return;
    float along;
    float2 crossing = SunLineCell(under + bent * landing.t, along);
    Splat(counts, (crossing - Origin()) / size, weight);
}

float Resolve(int2 cell)
{
    Texture2D<uint> counts = ResourceDescriptorHeap[g_pass.input0];
    float landed = 0.0;
    float left = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            int2 tap = clamp(cell + int2(x, y), 0, int(Cells()) - 1);
            float weight = (2.0 - abs(x)) * (2.0 - abs(y));
            landed += float(counts.Load(int3(tap, 0))) * weight;
            left += float(counts.Load(int3(tap + int2(Cells(), 0), 0))) * weight;
        }
    }
    return left > 0.25 * 16.0 * kPhotonScale ? landed / left : 1.0;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Cells()))
        return;
    uint step = g_pass.step & 3u;
    if (step == 0) {
        RWTexture2D<uint> counts = ResourceDescriptorHeap[g_pass.output0];
        RWTexture2D<float4> sunMap = ResourceDescriptorHeap[g_pass.output2];
        counts[id.xy] = 0;
        counts[id.xy + uint2(Cells(), 0)] = 0;
        sunMap[id.xy] = float4(1.0, 1.0, 1.0, kNoWaterCell);
    } else if (step == 1) {
        RWTexture2D<uint> counts = ResourceDescriptorHeap[g_pass.output0];
        RWTexture2D<float4> sunMap = ResourceDescriptorHeap[g_pass.output2];
        TracePhoton(id.xy, counts, sunMap);
    } else {
        RWTexture2D<float> caustics = ResourceDescriptorHeap[g_pass.output1];
        caustics[id.xy] = Resolve(int2(id.xy));
    }
}
