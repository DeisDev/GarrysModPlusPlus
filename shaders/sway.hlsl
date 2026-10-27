#include "common.hlsli"

// Moves the vertices of swaying static props as VertexLitGeneric's vertex shader does for $treesway materials
// (ComputeTreeSway in the SDK's tree_sway.h), for this frame's time and wind and for last frame's, which give
// motion vectors. Thread group y is the job, x covers its vertices.
// input0: jobs (a header, then SwayJob records); input1: SwayMaterial table; output0, output1: this frame's and last
// frame's vertex positions of every prop.

// Mirrors SwayingProps::Header and SwayingProps::Job in src/swaying_props.cpp.
struct SwayHeader
{
    float time;
    float previousTime;
    float2 wind;
    float2 previousWind;
    uint2 padding;
};

struct SwayJob
{
    float4 objectToWorld[3];
    uint sourceView;
    uint vertexMaterialsView;
    uint vertexCount;
    uint outputOffset;
};

// Mirrors gmodpp::SwayMaterial in src/swaying_props.h.
struct SwayMaterial
{
    float height;
    float startHeight;
    float radius;
    float startRadius;
    float speed;
    float intensity;
    float scrumbleWaveCount;
    float scrumbleIntensity;
    float fastSpeedScale;
    float scrumbleFalloff;
    float falloff;
    float scrumbleSpeed;
    float lerpStart;
    float lerpEnd;
    uint mode;
    uint staticWind;
};

static const uint kNoSway = 0xFFFFFFFFu;

float3 TreeSway(float3 position, SwayMaterial m, float3x3 rotation, float3 root, float time, float2 wind)
{
    static const float kWindOffsetScale = 19.0;
    if (m.staticWind != 0)
        wind = float2(0.5, 0.5);
    float windIntensity = length(wind);
    // The wind in model space: the shader multiplies it by the transposed model rotation.
    float3 windOS = mul(float3(wind, 0.0), rotation);

    float swayScaleHeight = saturate((position.z - m.height * m.startHeight) / ((1.0 - m.startHeight) * m.height));
    float swayScaleRadius = saturate(length(position.xy - m.radius * m.startRadius) / ((1.0 - m.startRadius) * m.radius));
    float heightThreshold = m.mode == 2 ? step(position.z - m.height * m.startHeight, 0.0) : step(0.0, position.z - m.height * m.startHeight);
    float orthoBranchScale = abs(dot(windOS, float3(position.xy, 0.0)));
    orthoBranchScale = 1.0 - saturate(orthoBranchScale / (max(length(windOS), 0.0001) * max(length(position.xy), 0.0001)));

    float swayScaleTrunk = m.intensity * pow(swayScaleHeight, m.falloff);
    float swayScaleBranches = m.mode == 2 ? 0.0 : m.intensity * orthoBranchScale * swayScaleRadius * heightThreshold;
    float windTimeOffset = dot(root, float3(1.0, 1.0, 1.0)) * kWindOffsetScale;
    float slowSwayTime = (time + windTimeOffset) * m.speed;

    float3 scrumblePosition = normalize(position) * m.scrumbleWaveCount;
    float scrumbleScale = pow(swayScaleRadius, m.scrumbleFalloff) * m.scrumbleIntensity * heightThreshold;
    float speedLerp = smoothstep(m.lerpStart, m.lerpEnd, windIntensity);
    float4 sines = sin(float4(1.0, 2.31, m.fastSpeedScale, 2.14 * m.fastSpeedScale) * slowSwayTime);
    sines.xy = lerp(sines.xy, sines.zw, speedLerp);

    float3 offset = windOS * swayScaleTrunk * (sines.x + 0.1);
    offset += windOS * swayScaleBranches * (sines.y + 0.4);
    float3 scrumble = scrumbleScale;
    if (m.mode == 2)
        scrumble *= float3(0.5, 0.5, 1.0);
    offset += windIntensity * (scrumble * sin(m.scrumbleSpeed * time + scrumblePosition.yzx + windTimeOffset));
    return position + offset;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
    ByteAddressBuffer jobs = ResourceDescriptorHeap[g_pass.input0];
    SwayHeader header = jobs.Load<SwayHeader>(0);
    SwayJob job = jobs.Load<SwayJob>(32 + group.y * 64);
    uint vertex = id.x;
    if (vertex >= job.vertexCount)
        return;
    ByteAddressBuffer source = ResourceDescriptorHeap[job.sourceView];
    ByteAddressBuffer vertexMaterials = ResourceDescriptorHeap[job.vertexMaterialsView];
    RWByteAddressBuffer current = ResourceDescriptorHeap[g_pass.output0];
    RWByteAddressBuffer previous = ResourceDescriptorHeap[g_pass.output1];
    float3 position = source.Load<float3>(vertex * 12);
    float3 now = position;
    float3 before = position;
    uint material = vertexMaterials.Load(vertex * 4);
    if (material != kNoSway) {
        ByteAddressBuffer materials = ResourceDescriptorHeap[g_pass.input1];
        SwayMaterial m = materials.Load<SwayMaterial>(material * 64);
        float3x3 rotation = float3x3(job.objectToWorld[0].xyz, job.objectToWorld[1].xyz, job.objectToWorld[2].xyz);
        float3 root = float3(job.objectToWorld[0].w, job.objectToWorld[1].w, job.objectToWorld[2].w);
        now = TreeSway(position, m, rotation, root, header.time, header.wind);
        before = TreeSway(position, m, rotation, root, header.previousTime, header.previousWind);
    }
    current.Store<float3>(job.outputOffset + vertex * 12, now);
    previous.Store<float3>(job.outputOffset + vertex * 12, before);
}
