#ifndef GMODPP_RADIANCE_CACHE_HLSLI
#define GMODPP_RADIANCE_CACHE_HLSLI

#include "scene.hlsli"

// The world-space radiance cache's queries. Passes built with it (GMODPP_SHARC, src/lighting.cpp) read NVIDIA SHaRC's
// cache (shaders/nvidia/sharc_query.hlsli); others, and devices without its 16-bit types and 64-bit atomics, have none.
//
// CachedRadiance: the light leaving a hit towards a ray's origin, its albedo times the cached light plus `emission`
// (BounceEmission), as the live path would trace it. False where the cache is off or holds none there, where the ray
// (`rayLength`) is too short for the voxel it ends in, or where its cone (`coneWidth` across at the hit; a diffuse
// bounce's is the ray's length) is narrower than the voxels.
#ifdef GMODPP_SHARC
#include "nvidia/sharc_query.hlsli"
#else
bool RadianceCacheEnabled() { return false; }

bool CachedRadiance(HitSurface hit, float3 emission, float rayLength, float coneWidth, out float3 radiance)
{
    radiance = 0.0;
    return false;
}

float3 CacheDebugColor(float3 position, float3 normal) { return 0.0; }
#endif

#endif
