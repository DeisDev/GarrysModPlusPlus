#include "common.hlsli"

// Skins the vertices of animated models (characters, NPCs, ragdolls) for their BLASes, as studio models are skinned:
// each vertex is moved by up to three bones' pose-to-world transforms, blended by its weights. Thread group y is the job
// (one entity), x covers its vertices.
// input0: jobs (SkinJob records, then each job's bone transforms, three float4 rows each).

// Mirrors SkinnedCasters::Job in src/skinned_casters.cpp.
struct SkinJob
{
    uint sourceView;
    uint vertexCount;
    uint outputUav;
    uint bonesOffset;
};

// Mirrors SkinnedCasters::SourceVertex: the bind pose position, then the bone indices (8 bits each) with the number of
// bones in the top 8 bits, and their weights.
struct SkinVertex
{
    float3 position;
    uint bones;
    float3 weights;
    uint padding;
};

[RootSignature(ROOT_SIGNATURE)]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
    ByteAddressBuffer jobs = ResourceDescriptorHeap[g_pass.input0];
    SkinJob job = jobs.Load<SkinJob>(group.y * 16);
    uint vertex = id.x;
    if (vertex >= job.vertexCount)
        return;
    ByteAddressBuffer source = ResourceDescriptorHeap[job.sourceView];
    SkinVertex v = source.Load<SkinVertex>(vertex * 32);
    float4 position = float4(v.position, 1.0);
    float3 skinned = 0.0;
    uint count = v.bones >> 24;
    for (uint i = 0; i < count; ++i) {
        uint at = job.bonesOffset + ((v.bones >> (8 * i)) & 0xFFu) * 48;
        float3 moved = float3(dot(jobs.Load<float4>(at), position), dot(jobs.Load<float4>(at + 16), position), dot(jobs.Load<float4>(at + 32), position));
        skinned += moved * v.weights[i];
    }
    RWByteAddressBuffer output = ResourceDescriptorHeap[job.outputUav];
    output.Store<float3>(vertex * 12, skinned);
}
