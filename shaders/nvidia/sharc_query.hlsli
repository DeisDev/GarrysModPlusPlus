#ifndef GMODPP_SHARC_QUERY_HLSLI
#define GMODPP_SHARC_QUERY_HLSLI

#include "sharc_common.hlsli"

// Queries of the radiance cache (shaders/radiance_cache.hlsli). The 3D skybox's scaled-down space is not in it.
bool RadianceCacheEnabled()
{
    return g_sampling.cacheEnabled != 0 && (g_frame.traceMask & kSkyboxMask) == 0;
}

bool CachedRadiance(HitSurface hit, float3 emission, float rayLength, float coneWidth, out float3 radiance)
{
    radiance = 0.0;
    if (!RadianceCacheEnabled())
        return false;
    // A voxel's light averages over the voxel: rays shorter than its diagonal end inside the voxel they left from, and
    // narrow cones see detail it does not hold (RTXPT's TerminateThreshold of 3.5 voxels).
    float voxel = CacheVoxelSize(hit.position);
    if (rayLength <= voxel * 1.7320508 || coneWidth <= voxel * 3.5)
        return false;
    return SharcGetCachedRadiance(CacheParameters(), CacheHit(hit.position, hit.normal, hit.albedo, emission), radiance, false);
}

float3 CacheDebugColor(float3 position, float3 normal)
{
    return HashGridDebugColoredHash(position, normal, CacheParameters().hashGridParameters);
}

#endif
