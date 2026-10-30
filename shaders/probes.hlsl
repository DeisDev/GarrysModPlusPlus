#include "baked.hlsli"

// Irradiance volume for surfaces lit outside the G-buffer (the translucent surfaces the game draws after the
// capture, shaders/forward_light.hlsli). A grid of kProbeGrid probes around the camera holds irradiance as
// a + b.n per colour channel (L1 spherical harmonics). Probes sit at cell centres, (cell + 0.5) * spacing, and
// are stored at cell modulo the grid size, so the grid scrolls with the camera without moving data; a probe
// whose cell changed starts over.
//
// Trace (step 0, input4: 1 when last frame's volume may be used; a thread group per probe, its threads sharing the
// rays): every frame a share of the probes traces g_frame.volumeRays rays. A hit returns its albedo times the
// light reaching it (sun, one light picked by SampleLight, and last frame's volume as indirect light), a miss
// the sky. Probes that mostly see back faces are inside geometry and marked invalid.
// input0-2: last frame's red, green and blue coefficients; input3: last frame's cells (xyz) and validity (w);
// output0-2, output3: this frame's.
//
// Fill (step 1, a thread per probe): invalid probes take the average of their valid neighbours, so light does not leak in from
// inside walls, and every probe adds the direct light at it: the sun and the kProbeLights strongest point lights,
// each with a shadow ray (projected lights are left to the game). Only the output has it, as hits add their own
// direct light. input0-3: this frame's coefficients and cells; output0-2: the volume the game samples.
//
// Baked fill (step 2, a thread per probe, indirect_lighting = 1): instead of tracing, each probe takes the leaf
// ambient light at it (L1 of its ambient cube) and adds the direct light as the fill does. output0-2: the volume.

static const uint3 kProbeGrid = uint3(32, 32, 16);
// Probes away from the camera are traced every this many frames.
static const uint kUpdatePeriod = 4;
static const float kHistoryWeight = 0.85;
static const float kInsideFraction = 0.3;
static const float kMaxDistance = 16384.0;
static const uint kProbeLights = 4;
static const uint kProbeThreads = 32;
static const uint kProbeCount = kProbeGrid.x * kProbeGrid.y * kProbeGrid.z;

// Each thread's sums of its rays: radiance (xyz) and back faces met (w), then radiance times direction per channel.
groupshared float4 g_rayTotals[kProbeThreads];
groupshared float3 g_raySh[kProbeThreads][3];

uint3 ProbeId(uint probe)
{
    return uint3(probe % kProbeGrid.x, (probe / kProbeGrid.x) % kProbeGrid.y, probe / (kProbeGrid.x * kProbeGrid.y));
}

uint3 Texel(int3 cell)
{
    int3 grid = int3(kProbeGrid);
    return uint3(((cell % grid) + grid) % grid);
}

// Irradiance from last frame's volume, or the sky's outside the probe window.
float3 VolumeIrradiance(float3 position, float3 normal)
{
    float3 window = (position / g_frame.probeSpacing - 0.5) - float3(g_frame.probeOrigin);
    if (any(window < 0.0) || any(window > float3(kProbeGrid) - 1.0))
        return g_frame.skyColor;
    float3 uvw = position / (g_frame.probeSpacing * float3(kProbeGrid));
    Texture3D<float4> red = ResourceDescriptorHeap[g_pass.input0];
    Texture3D<float4> green = ResourceDescriptorHeap[g_pass.input1];
    Texture3D<float4> blue = ResourceDescriptorHeap[g_pass.input2];
    float4 r = red.SampleLevel(g_linearWrap, uvw, 0.0);
    float4 g = green.SampleLevel(g_linearWrap, uvw, 0.0);
    float4 b = blue.SampleLevel(g_linearWrap, uvw, 0.0);
    return max(float3(r.x + dot(r.yzw, normal), g.x + dot(g.yzw, normal), b.x + dot(b.yzw, normal)), 0.0);
}

// Whether the ray met the back of the triangle it hit. Source's triangles are clockwise seen from the front, so the
// cross product of their edges points out of the back.
bool HitBackFace(HitInfo hit, float3 direction)
{
    InstanceRecord record = LoadInstance(hit.instance);
    ByteAddressBuffer positions = ResourceDescriptorHeap[record.positions];
    ByteAddressBuffer indices = ResourceDescriptorHeap[record.indices];
    uint3 index = indices.Load3(hit.primitive * 12);
    float3 p0 = positions.Load<float3>(index.x * 12);
    float3 normal = mul((float3x3)hit.objectToWorld, cross(positions.Load<float3>(index.y * 12) - p0, positions.Load<float3>(index.z * 12) - p0));
    return dot(normal, direction) < 0.0;
}

void Trace(uint3 id, uint thread)
{
    int3 cell = g_frame.probeOrigin + int3(id);
    uint3 texel = Texel(cell);
    Texture3D<float4> red = ResourceDescriptorHeap[g_pass.input0];
    Texture3D<float4> green = ResourceDescriptorHeap[g_pass.input1];
    Texture3D<float4> blue = ResourceDescriptorHeap[g_pass.input2];
    Texture3D<int4> cells = ResourceDescriptorHeap[g_pass.input3];
    RWTexture3D<float4> redOut = ResourceDescriptorHeap[g_pass.output0];
    RWTexture3D<float4> greenOut = ResourceDescriptorHeap[g_pass.output1];
    RWTexture3D<float4> blueOut = ResourceDescriptorHeap[g_pass.output2];
    RWTexture3D<int4> cellsOut = ResourceDescriptorHeap[g_pass.output3];
    float4 previous[3] = {red[texel], green[texel], blue[texel]};
    int4 previousCell = cells[texel];
    bool fresh = any(previousCell.xyz != cell) || previousCell.w < 0 || g_pass.input4 == 0;
    if (!fresh && (texel.x + texel.y * 3 + texel.z * 7 + g_frame.frameIndex) % kUpdatePeriod != 0) {
        if (thread == 0) {
            redOut[texel] = previous[0];
            greenOut[texel] = previous[1];
            blueOut[texel] = previous[2];
            cellsOut[texel] = previousCell;
        }
        return;
    }
    float3 position = (float3(cell) + 0.5) * g_frame.probeSpacing;
    uint probeSeed = Hash(texel.x + texel.y * 64u + texel.z * 4096u + Hash(g_frame.frameIndex * 0x9E3779B9u + 0x7Bu));
    float3 a = 0.0;
    float3 b[3] = {(float3)0.0, (float3)0.0, (float3)0.0};
    uint backFaces = 0;
    for (uint r = thread; r < g_frame.volumeRays; r += kProbeThreads) {
        uint seed = Hash(probeSeed + r);
        float3 direction = UniformSphere((r + Random(seed)) / g_frame.volumeRays, Random(seed));
        float3 radiance = g_frame.skyColor;
        HitInfo hit;
        if (TraceHit(position, direction, kMaxDistance, g_frame.indirectMask, hit)) {
            if (HitBackFace(hit, direction)) {
                ++backFaces;
                radiance = 0.0;
            } else {
                HitSurface surface = SurfaceOf(hit, position, direction);
                float3 origin = surface.position + surface.normal;
                float3 irradiance = SunIrradiance(surface, origin, g_frame.bounceMask) + VolumeIrradiance(surface.position, surface.normal) * g_frame.indirectScale;
                float3 light;
                float3 toLight;
                float distance;
                uint picked;
                LightSet set = LightSetAt(surface.position, surface.normal);
                if (SampleLight(set, surface.position, surface.normal, seed, light, toLight, distance, picked))
                    irradiance += light * LightTransmittance(set, picked, origin, toLight, distance);
                radiance = ClampSample(surface.albedo * irradiance);
            }
        }
        a += radiance;
        b[0] += radiance.r * direction;
        b[1] += radiance.g * direction;
        b[2] += radiance.b * direction;
    }
    g_rayTotals[thread] = float4(a, backFaces);
    g_raySh[thread][0] = b[0];
    g_raySh[thread][1] = b[1];
    g_raySh[thread][2] = b[2];
    GroupMemoryBarrierWithGroupSync();
    if (thread != 0)
        return;
    float4 totals = 0.0;
    for (uint t = 0; t < kProbeThreads; ++t) {
        totals += g_rayTotals[t];
        if (t != 0) {
            b[0] += g_raySh[t][0];
            b[1] += g_raySh[t][1];
            b[2] += g_raySh[t][2];
        }
    }
    a = totals.xyz / g_frame.volumeRays;
    backFaces = uint(totals.w);
    float4 sampled[3] = {float4(a.r, b[0] * (2.0 / g_frame.volumeRays)), float4(a.g, b[1] * (2.0 / g_frame.volumeRays)), float4(a.b, b[2] * (2.0 / g_frame.volumeRays))};
    float weight = fresh ? 0.0 : kHistoryWeight;
    redOut[texel] = lerp(sampled[0], previous[0], weight);
    greenOut[texel] = lerp(sampled[1], previous[1], weight);
    blueOut[texel] = lerp(sampled[2], previous[2], weight);
    cellsOut[texel] = int4(cell, backFaces > g_frame.volumeRays * kInsideFraction ? 0 : 1);
}

// Light of irradiance E (facing it) from direction d gives E max(n.d, 0), whose L1 fit is E / 4 + (E / 2) d.n.
void AddDirection(float3 irradiance, float3 direction, inout float4 value[3])
{
    value[0] += float4(irradiance.r * 0.25, irradiance.r * 0.5 * direction);
    value[1] += float4(irradiance.g * 0.25, irradiance.g * 0.5 * direction);
    value[2] += float4(irradiance.b * 0.25, irradiance.b * 0.5 * direction);
}

void AddDirectLight(float3 position, inout float4 value[3])
{
    if (g_frame.sunEnabled != 0.0)
        AddDirection(g_frame.sunColor * SunVisibility(position, g_frame.sunShadowMask), g_frame.sunDirection, value);
    LightSet set = LightSetAt(position, 0.0);
    uint top[kProbeLights];
    float topWeight[kProbeLights];
    [unroll]
    for (uint k = 0; k < kProbeLights; ++k) {
        top[k] = 0;
        topWeight[k] = 0.0;
    }
    for (uint i = 0; i < set.pointLights; ++i) {
        Light light = LoadLight(i >= set.mapLights ? g_frame.lightCount + i - set.mapLights : LightRangeIndex(set.ranges, i));
        float3 toLight;
        float distance;
        uint candidate = i;
        float candidateWeight = Luminance(LightIrradiance(light, position, normalize(light.origin - position), toLight, distance));
        [unroll]
        for (uint k = 0; k < kProbeLights; ++k) {
            if (candidateWeight > topWeight[k]) {
                uint index = top[k];
                float w = topWeight[k];
                top[k] = candidate;
                topWeight[k] = candidateWeight;
                candidate = index;
                candidateWeight = w;
            }
        }
    }
    [unroll]
    for (uint k = 0; k < kProbeLights; ++k) {
        if (topWeight[k] <= VisibleIrradiance())
            break;
        Light light = LoadLight(top[k] >= set.mapLights ? g_frame.lightCount + top[k] - set.mapLights : LightRangeIndex(set.ranges, top[k]));
        float3 toLight;
        float distance;
        float3 irradiance = LightIrradiance(light, position, normalize(light.origin - position), toLight, distance);
        AddDirection(irradiance * LightTransmittance(set, top[k], position, toLight, distance), toLight, value);
    }
}

void Fill(uint3 id)
{
    int3 cell = g_frame.probeOrigin + int3(id);
    uint3 texel = Texel(cell);
    Texture3D<float4> red = ResourceDescriptorHeap[g_pass.input0];
    Texture3D<float4> green = ResourceDescriptorHeap[g_pass.input1];
    Texture3D<float4> blue = ResourceDescriptorHeap[g_pass.input2];
    Texture3D<int4> cells = ResourceDescriptorHeap[g_pass.input3];
    RWTexture3D<float4> redOut = ResourceDescriptorHeap[g_pass.output0];
    RWTexture3D<float4> greenOut = ResourceDescriptorHeap[g_pass.output1];
    RWTexture3D<float4> blueOut = ResourceDescriptorHeap[g_pass.output2];
    float4 value[3] = {red[texel], green[texel], blue[texel]};
    // Direct light inside a wall comes from the first open neighbour instead.
    float3 directPosition = (float3(cell) + 0.5) * g_frame.probeSpacing;
    bool direct = cells[texel].w != 0;
    if (!direct) {
        static const int3 kNeighbours[6] = {int3(1, 0, 0), int3(-1, 0, 0), int3(0, 1, 0), int3(0, -1, 0), int3(0, 0, 1), int3(0, 0, -1)};
        float4 sum[3] = {(float4)0.0, (float4)0.0, (float4)0.0};
        float count = 0.0;
        [unroll]
        for (uint n = 0; n < 6; ++n) {
            int3 neighbour = int3(id) + kNeighbours[n];
            if (any(neighbour < 0) || any(neighbour >= int3(kProbeGrid)))
                continue;
            uint3 neighbourTexel = Texel(g_frame.probeOrigin + neighbour);
            if (cells[neighbourTexel].w == 0)
                continue;
            sum[0] += red[neighbourTexel];
            sum[1] += green[neighbourTexel];
            sum[2] += blue[neighbourTexel];
            if (!direct) {
                directPosition = (float3(g_frame.probeOrigin + neighbour) + 0.5) * g_frame.probeSpacing;
                direct = true;
            }
            count += 1.0;
        }
        if (count > 0.0) {
            value[0] = sum[0] / count;
            value[1] = sum[1] / count;
            value[2] = sum[2] / count;
        }
    }
    if (direct)
        AddDirectLight(directPosition, value);
    redOut[texel] = value[0];
    greenOut[texel] = value[1];
    blueOut[texel] = value[2];
}

void BakedFill(uint3 id)
{
    int3 cell = g_frame.probeOrigin + int3(id);
    uint3 texel = Texel(cell);
    float3 position = (float3(cell) + 0.5) * g_frame.probeSpacing;
    float3 cube[6];
    if (!LeafAmbientCube(position, cube)) {
        for (uint side = 0; side < 6; ++side)
            cube[side] = g_frame.skyColor;
    }
    // a + b.n: the mean of the six directions, and half the difference along each axis.
    float3 mean = (cube[0] + cube[1] + cube[2] + cube[3] + cube[4] + cube[5]) / 6.0;
    float3 slopeX = (cube[0] - cube[1]) * 0.5;
    float3 slopeY = (cube[2] - cube[3]) * 0.5;
    float3 slopeZ = (cube[4] - cube[5]) * 0.5;
    float4 value[3] = {float4(mean.r, slopeX.r, slopeY.r, slopeZ.r), float4(mean.g, slopeX.g, slopeY.g, slopeZ.g), float4(mean.b, slopeX.b, slopeY.b, slopeZ.b)};
    AddDirectLight(position, value);
    RWTexture3D<float4> redOut = ResourceDescriptorHeap[g_pass.output0];
    RWTexture3D<float4> greenOut = ResourceDescriptorHeap[g_pass.output1];
    RWTexture3D<float4> blueOut = ResourceDescriptorHeap[g_pass.output2];
    redOut[texel] = value[0];
    greenOut[texel] = value[1];
    blueOut[texel] = value[2];
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(kProbeThreads, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupIndex, uint3 id : SV_DispatchThreadID)
{
    if (g_pass.step == 0)
        Trace(ProbeId(group.x), thread);
    else if (id.x >= kProbeCount)
        return;
    else if (g_pass.step == 1)
        Fill(ProbeId(id.x));
    else
        BakedFill(ProbeId(id.x));
}
