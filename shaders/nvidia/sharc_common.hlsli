#ifndef GMODPP_SHARC_COMMON_HLSLI
#define GMODPP_SHARC_COMMON_HLSLI

// The radiance cache (NVIDIA SHaRC): a world-space hash grid of voxels that hold the light leaving surfaces, divided by
// their albedo (material demodulation), with emission kept apart. Passes that include this are built at compile time
// with SHaRC's headers (CMakeLists.txt, SHARC_SHADER_OPTIONS); nothing here is compiled at runtime.

#include "../scene.hlsli"

// Mirrors gmodpp::SamplingConstants in src/lighting.h.
struct SamplingConstants
{
    // The main view's eye, which sets the voxels' size for every view, and its eye when the cache was last updated.
    float3 cacheCamera;
    float sceneScale;
    float3 cacheCameraPrevious;
    float levelBias;
    uint cacheEnabled;
    uint capacity;
    // UAVs of the accumulation and resolved buffers (structured, 16 bytes per entry).
    uint accumulationView;
    uint resolvedView;
    uint accumulationFrames;
    uint staleFrames;
    float radianceScale;
    uint padding;
};

ConstantBuffer<SamplingConstants> g_sampling : register(b2);
RWStructuredBuffer<uint64_t> g_sharcHash : register(u0, space2);

#define SHARC_SEPARATE_EMISSIVE 1
#define SHARC_MATERIAL_DEMODULATION 1
#define SHARC_PROPAGATION_DEPTH 3
#define SHARC_ENABLE_FADE_ACCELERATION 1
#define HASH_GRID_USE_NORMALS 1
#include "SharcCommon.h"

// Albedo below this divides the cache's light by this instead.
static const float kMinDemodulation = 1.0 / 1024.0;

SharcParameters CacheParameters()
{
    SharcParameters parameters;
    parameters.hashGridParameters.cameraPosition = g_sampling.cacheCamera;
    parameters.hashGridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    parameters.hashGridParameters.sceneScale = g_sampling.sceneScale;
    parameters.hashGridParameters.levelBias = g_sampling.levelBias;
    parameters.hashGridData.capacity = g_sampling.capacity;
    parameters.hashGridData.hashEntriesBuffer = g_sharcHash;
    parameters.radianceScale = g_sampling.radianceScale;
    parameters.accumulationBuffer = ResourceDescriptorHeap[g_sampling.accumulationView];
    parameters.resolvedBuffer = ResourceDescriptorHeap[g_sampling.resolvedView];
    return parameters;
}

// The radiance cache's voxel holding a surface, as SHaRC hashes it: its face normal (facing where the light leaves to),
// the albedo its light is divided by, and the emission added to it.
SharcHitData CacheHit(float3 position, float3 faceNormal, float3 albedo, float3 emission)
{
    SharcHitData hit;
    hit.positionWorld = position;
    hit.normalWorld = faceNormal;
    hit.materialDemodulation = max(albedo, kMinDemodulation);
    hit.emissive = emission;
    return hit;
}

// The edge of the cache's voxel at a point.
float CacheVoxelSize(float3 position)
{
    SharcParameters parameters = CacheParameters();
    return HashGridGetVoxelSize(HashGridGetLevel(position, parameters.hashGridParameters), parameters.hashGridParameters);
}

#endif
