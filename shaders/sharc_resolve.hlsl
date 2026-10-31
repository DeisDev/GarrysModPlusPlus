#include "nvidia/sharc_common.hlsli"

// Step 0: merges each radiance cache entry's light of this frame (shaders/sharc_update.hlsl) into its history over up
// to g_sampling.accumulationFrames frames, evicts entries no path reached for g_sampling.staleFrames frames and clears
// the accumulation for the next frame. Step 1: empties every entry (a new cache or a new map).

[RootSignature(ROOT_SIGNATURE)]
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (g_sampling.cacheEnabled == 0 || id.x >= g_sampling.capacity)
        return;
    SharcParameters parameters = CacheParameters();
    if (g_pass.step == 1) {
        g_sharcHash[id.x] = HASH_GRID_INVALID_HASH_KEY;
        parameters.accumulationBuffer[id.x] = SharcZeroAccumulationData();
        parameters.resolvedBuffer[id.x] = SharcZeroPackedData();
        return;
    }
    SharcResolveParameters resolve;
    resolve.cameraPositionPrev = g_sampling.cacheCameraPrevious;
    resolve.accumulationFrameNum = g_sampling.accumulationFrames;
    resolve.responsiveFrameNum = 0;
    resolve.staleFrameNumMax = g_sampling.staleFrames;
    resolve.frameIndex = g_frame.frameIndex;
    SharcResolveEntry(id.x, parameters, resolve);
}
