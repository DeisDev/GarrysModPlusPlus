#include "common.hlsli"

// Particle shadows (src/particle_shadows.h, Lighting::RecordParticles): last frame's smoke sprites as spheres, splatted
// into a volume of extinction around the eye, then a shadow map over the plane across the sun, which every sun shadow ray
// reads (ParticleShadowAt in shaders/common.hlsli).
// 0: clears the volume's sums (output0, a thread per cell).
// 1: a thread per sprite (input0, g_frame.particleCount of them): its extinction, such that a ray through its centre
//    loses its opacity, kept over its volume and added in fixed point to the cells whose centres lie inside it (the
//    nearest cell for sprites smaller than a cell).
// 2: the sums as extinction per unit (output1, a thread per cell).
// 3: a thread per texel of the shadow map (output0): the line toward the sun through it, from kShadowSpan / 2 below the
//    eye's place along the sun to as far above, through the volume (input0): its optical depth and the mean and spread
//    of where along the sun it lies (units from the eye's place, particleShadowAlong), weighted by optical depth.

struct Particle
{
    float3 centre;
    float radius;
    float opacity;
    float3 padding;
};

// Extinction per unit is summed in units of 2^-20.
static const float kFixedPoint = 1048576.0;
// The most cells a sprite reaches from its centre along each axis.
static const int kReach = 6;
static const uint kShadowSteps = 96;
static const float kShadowSpan = 3840.0;
static const float kMinSpread = 16.0;

void Splat(uint index)
{
    if (index >= g_frame.particleCount)
        return;
    StructuredBuffer<Particle> particles = ResourceDescriptorHeap[g_pass.input0];
    RWTexture3D<uint> sums = ResourceDescriptorHeap[g_pass.output0];
    uint3 size;
    sums.GetDimensions(size.x, size.y, size.z);
    Particle particle = particles[index];
    float radius = particle.radius * g_frame.particleVolumeInvCell;
    float3 centre = (particle.centre - g_frame.particleVolumeOrigin) * g_frame.particleVolumeInvCell;
    float extinction = -log(1.0 - particle.opacity) / (2.0 * particle.radius);
    // Its extinction times its volume in cells, which the cells share.
    float total = extinction * (4.0 / 3.0 * 3.14159265) * radius * radius * radius;
    int reach = min(int(ceil(radius)), kReach);
    int3 middle = int3(floor(centre));
    uint inside = 0;
    [loop]
    for (int z = -reach; z <= reach; ++z) {
        [loop]
        for (int y = -reach; y <= reach; ++y) {
            [loop]
            for (int x = -reach; x <= reach; ++x) {
                float3 offset = float3(middle + int3(x, y, z)) + 0.5 - centre;
                inside += dot(offset, offset) <= radius * radius ? 1u : 0u;
            }
        }
    }
    if (inside == 0) {
        if (all(middle >= 0) && all(middle < int3(size)))
            InterlockedAdd(sums[middle], uint(total * kFixedPoint + 0.5));
        return;
    }
    uint share = uint(total / inside * kFixedPoint + 0.5);
    [loop]
    for (int z2 = -reach; z2 <= reach; ++z2) {
        [loop]
        for (int y2 = -reach; y2 <= reach; ++y2) {
            [loop]
            for (int x2 = -reach; x2 <= reach; ++x2) {
                int3 cell = middle + int3(x2, y2, z2);
                float3 offset = float3(cell) + 0.5 - centre;
                if (dot(offset, offset) <= radius * radius && all(cell >= 0) && all(cell < int3(size)))
                    InterlockedAdd(sums[cell], share);
            }
        }
    }
}

void ShadowTexel(uint2 texel)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    uint2 size;
    output.GetDimensions(size.x, size.y);
    if (any(texel >= size))
        return;
    Texture3D<float> volume = ResourceDescriptorHeap[g_pass.input0];
    uint3 cells;
    volume.GetDimensions(cells.x, cells.y, cells.z);
    float2 plane = ((float2(texel) + 0.5) / float2(size) - 0.5) / g_frame.particleShadowScale + g_frame.particleShadowCentre;
    float3 sun = g_frame.sunDirection;
    // The plane's axes and the sun's direction are orthonormal: a point is its place on each.
    float3 across = g_frame.cloudShadowU.xyz * plane.x + g_frame.cloudShadowV.xyz * plane.y;
    float step = kShadowSpan / kShadowSteps;
    float depth = 0.0;
    float along = 0.0;
    float alongSquared = 0.0;
    [loop]
    for (uint i = 0; i < kShadowSteps; ++i) {
        float s = (i + 0.5) * step - 0.5 * kShadowSpan;
        float3 position = across + sun * (g_frame.particleShadowAlong + s);
        float3 uvw = (position - g_frame.particleVolumeOrigin) * g_frame.particleVolumeInvCell / float3(cells);
        if (any(uvw < 0.0) || any(uvw > 1.0))
            continue;
        float weight = volume.SampleLevel(g_linearClamp, uvw, 0.0) * step;
        depth += weight;
        along += weight * s;
        alongSquared += weight * s * s;
    }
    if (depth <= 0.0) {
        output[texel] = float4(0.0, 0.0, kMinSpread, 0.0);
        return;
    }
    float mean = along / depth;
    output[texel] = float4(depth, mean, max(sqrt(max(alongSquared / depth - mean * mean, 0.0)), kMinSpread), 0.0);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    if (g_pass.step == 1) {
        Splat(group.x * 64 + index);
        return;
    }
    if (g_pass.step == 3) {
        ShadowTexel(uint2(group.x * 8 + index % 8, group.y * 8 + index / 8));
        return;
    }
    if (g_pass.step == 0) {
        RWTexture3D<uint> sums = ResourceDescriptorHeap[g_pass.output0];
        sums[id] = 0;
        return;
    }
    RWTexture3D<uint> sums = ResourceDescriptorHeap[g_pass.input0];
    RWTexture3D<float> extinction = ResourceDescriptorHeap[g_pass.output1];
    extinction[id] = float(sums[id]) / kFixedPoint;
}
