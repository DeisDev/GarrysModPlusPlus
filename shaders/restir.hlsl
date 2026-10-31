#define RTXDI_ENABLE_BOILING_FILTER 1
#define RTXDI_BOILING_FILTER_GROUP_SIZE 8
#include "nvidia/rtxdi_bridge.hlsli"
#include "phong.hlsli"
#include "Rtxdi/LightSampling/PresamplingFunctions.hlsli"
#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/DI/ReservoirStorage.hlsli"
#include "Rtxdi/DI/TemporalResampling.hlsli"
#include "Rtxdi/DI/SpatialResampling.hlsli"
#include "Rtxdi/DI/BoilingFilter.hlsli"

// Direct light of the main view's local lights with NVIDIA RTXDI (src/restir.h has the order and resources):
// PdfMain: each light's power into the light PDF texture's first mip (step 0), then each mip the sums of four texels of
// the one before (step = the mip).
// PresampleMain: tiles of lights picked by power (Power RIS) from the PDF.
// RegirMain: each ReGIR cell around the eye filled with lights picked in proportion to their bound over the cell.
// InitialMain: each pixel's reservoir from candidates of its ReGIR cell (and a BRDF ray that may meet a glowing
// triangle), with a shadow ray to the one kept.
// TemporalMain and SpatialMain: reuse of last frame's reservoir at the reprojected pixel and of a neighbour's.
// ShadeMain: one shadow ray to each pixel's sample (or its reused visibility) and its light over its density: the
// diffuse light to output0 (the sampled remainder, which is denoised with the indirect light) and its Phong highlight
// and rim added to output1 (shaders/direct.hlsl's highlights); with a debug view, its light (DebugView::LightSample) or
// reservoir's sample count (ReservoirM) to output2.

static const uint kPresampleSeed = 7;
static const uint kRegirSeed = 8;
static const uint kRegirCellSeed = 9;

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void PdfMain(uint3 id : SV_DispatchThreadID)
{
    uint level = g_pass.step;
    uint2 size = max(g_restir.pdfSize >> level, 1u);
    if (any(id.xy >= size))
        return;
    RWTexture2D<float> output = ResourceDescriptorHeap[g_restir.pdfMipUav + level];
    if (level == 0) {
        uint index = RTXDI_ZCurveToLinearIndex(id.xy);
        output[id.xy] = index < g_restir.lightBuffer.localLightBufferRegion.numLights ? LightPower(index) : 0.0;
        return;
    }
    RWTexture2D<float> input = ResourceDescriptorHeap[g_restir.pdfMipUav + level - 1];
    uint2 inputSize = max(g_restir.pdfSize >> (level - 1), 1u);
    float sum = 0.0;
    [unroll]
    for (uint k = 0; k < 4; ++k) {
        uint2 texel = id.xy * 2 + uint2(k & 1, k >> 1);
        if (all(texel < inputSize))
            sum += input[texel];
    }
    output[id.xy] = sum;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(256, 1, 1)]
void PresampleMain(uint3 id : SV_DispatchThreadID)
{
    RTXDI_RISBufferSegmentParameters tiles = g_restir.localLightRis;
    if (id.x >= tiles.tileCount * tiles.tileSize)
        return;
    uint tile = id.x / tiles.tileSize;
    uint sampleInTile = id.x % tiles.tileSize;
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(uint2(sampleInTile, tile), g_restir.runtime.frameIndex, kPresampleSeed);
    Texture2D pdf = ResourceDescriptorHeap[g_restir.pdfView];
    RTXDI_PresampleLocalLights(rng, pdf, g_restir.pdfSize, tile, sampleInTile, g_restir.lightBuffer.localLightBufferRegion, tiles);
}

// The visibility cluster of the ReGIR cell a thread group fills: a cell's lightsPerCell slots are whole groups.
groupshared uint g_groupCluster;

[RootSignature(ROOT_SIGNATURE)]
[numthreads(256, 1, 1)]
void RegirMain(uint3 id : SV_DispatchThreadID, uint thread : SV_GroupIndex)
{
    uint lightSlot = id.x;
    uint lightsPerCell = g_restir.regir.commonParams.lightsPerCell;
    if (thread == 0) {
        float3 centre;
        float radius;
        g_groupCluster = RTXDI_ReGIR_CellIndexToWorldPos(g_restir.regir, int(lightSlot / lightsPerCell), centre, radius) ? ClusterAt(centre) : g_frame.clusterCount;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lightSlot >= g_restir.regirLightSlots)
        return;
    g_volumeCluster = g_groupCluster;
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(uint2(lightSlot & 0xfff, lightSlot >> 12), g_restir.runtime.frameIndex, kRegirSeed);
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(uint2(lightSlot / lightsPerCell, 0), g_restir.runtime.frameIndex, kRegirCellSeed);
    RTXDI_PresampleLocalLightsForReGIR(rng, coherentRng, lightSlot, g_restir.lightBuffer.localLightBufferRegion, g_restir.localLightRis, g_restir.regir);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void InitialMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    RAB_Surface surface = RAB_GetGBufferSurface(int2(id.xy), false);
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    if (RAB_IsSurfaceValid(surface)) {
        RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(id.xy, g_restir.runtime.frameIndex, RTXDI_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
        RTXDI_RandomSamplerState coherentRng =
            RTXDI_InitRandomSampler(id.xy / RTXDI_TILE_SIZE_IN_PIXELS, g_restir.runtime.frameIndex, RTXDI_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
        RAB_LightSample sample;
        reservoir = RTXDI_SampleLightsForSurface(rng, coherentRng, surface, g_restir.restir.initialSamplingParams, g_restir.lightBuffer, g_restir.localLightRis,
            g_restir.environmentRis, g_restir.regir, sample);
    }
    RTXDI_StoreDIReservoir(reservoir, g_restir.restir.reservoirBufferParams, id.xy, g_restir.restir.bufferIndices.initialSamplingOutputBufferIndex);
}

// Every thread of a group reaches the boiling filter's barriers.
[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void TemporalMain(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID)
{
    bool inside = all(id.xy < g_frame.size);
    RAB_Surface surface = RAB_EmptySurface();
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    if (inside) {
        surface = RAB_GetGBufferSurface(int2(id.xy), false);
        reservoir = RTXDI_LoadDIReservoir(g_restir.restir.reservoirBufferParams, id.xy, g_restir.restir.bufferIndices.initialSamplingOutputBufferIndex);
    }
    if (RAB_IsSurfaceValid(surface) && g_restir.historyValid != 0) {
        RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(id.xy, g_restir.runtime.frameIndex, RTXDI_DI_TEMPORAL_RESAMPLING_RANDOM_SEED);
        Texture2D<float2> motion = ResourceDescriptorHeap[g_frame.motionView];
        Texture2D<float> previousDistance = ResourceDescriptorHeap[g_frame.previousDistanceView];
        float3 screenMotion = float3(motion.Load(int3(id.xy, 0)), previousDistance.Load(int3(id.xy, 0)) - surface.distance);
        int2 temporalPixel;
        RAB_LightSample sample = RAB_EmptyLightSample();
        reservoir = RTXDI_DITemporalResampling(id.xy, surface, reservoir, rng, g_restir.runtime, g_restir.restir.reservoirBufferParams, screenMotion,
            g_restir.restir.bufferIndices.temporalResamplingInputBufferIndex, g_restir.restir.temporalResamplingParams, temporalPixel, sample);
    }
    if (g_restir.restir.boilingFilterParams.enableBoilingFilter != 0)
        RTXDI_BoilingFilter(local.xy, g_restir.restir.boilingFilterParams.boilingFilterStrength, reservoir);
    if (inside)
        RTXDI_StoreDIReservoir(reservoir, g_restir.restir.reservoirBufferParams, id.xy, g_restir.restir.bufferIndices.temporalResamplingOutputBufferIndex);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void SpatialMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    RAB_Surface surface = RAB_GetGBufferSurface(int2(id.xy), false);
    RTXDI_DIReservoir reservoir =
        RTXDI_LoadDIReservoir(g_restir.restir.reservoirBufferParams, id.xy, g_restir.restir.bufferIndices.spatialResamplingInputBufferIndex);
    if (RAB_IsSurfaceValid(surface)) {
        RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(id.xy, g_restir.runtime.frameIndex, RTXDI_DI_SPATIAL_RESAMPLING_RANDOM_SEED);
        RAB_LightSample sample = RAB_EmptyLightSample();
        reservoir = RTXDI_DISpatialResampling(id.xy, surface, reservoir, rng, g_restir.runtime, g_restir.restir.reservoirBufferParams,
            g_restir.restir.bufferIndices.spatialResamplingInputBufferIndex, g_restir.restir.spatialResamplingParams, sample);
    }
    RTXDI_StoreDIReservoir(reservoir, g_restir.restir.reservoirBufferParams, id.xy, g_restir.restir.bufferIndices.spatialResamplingOutputBufferIndex);
}

// A light's index as a colour (DebugView::LightSample).
float3 LightColour(uint index)
{
    uint hash = Hash(index + 1);
    return float3(hash & 0xFF, (hash >> 8) & 0xFF, (hash >> 16) & 0xFF) / 255.0;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void ShadeMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RAB_Surface surface = RAB_GetGBufferSurface(pixel, false);
    uint shadingBuffer = g_restir.restir.bufferIndices.shadingInputBufferIndex;
    RTXDI_DIReservoir reservoir = RTXDI_LoadDIReservoir(g_restir.restir.reservoirBufferParams, id.xy, shadingBuffer);
    float3 diffuse = 0.0;
    float3 highlight = 0.0;
    if (RAB_IsSurfaceValid(surface) && RTXDI_IsValidDIReservoir(reservoir)) {
        RAB_LightSample sample =
            RAB_SamplePolymorphicLight(RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false), surface, RTXDI_GetDIReservoirSampleUV(reservoir));
        RTXDI_ShadingParameters shading = g_restir.restir.shadingParams;
        RTXDI_VisibilityReuseParameters reuse;
        reuse.maxAge = shading.finalVisibilityMaxAge;
        reuse.maxDistance = shading.finalVisibilityMaxDistance;
        float3 visibility;
        bool reused = shading.reuseFinalVisibility != 0 && RTXDI_GetDIReservoirVisibility(reservoir, reuse, visibility);
        if (!reused) {
            visibility = SampleTransmittance(surface, sample);
            if (shading.reuseFinalVisibility != 0) {
                RTXDI_StoreVisibilityInDIReservoir(reservoir, visibility, false);
                RTXDI_StoreDIReservoir(reservoir, g_restir.restir.reservoirBufferParams, id.xy, shadingBuffer);
            }
        }
        float3 light = sample.irradiance * visibility * RTXDI_GetDIReservoirInvPdf(reservoir);
        Texture2D<float4> gbufferFlags = ResourceDescriptorHeap[g_frame.flagsView];
        float4 marks = gbufferFlags.Load(int3(pixel, 0));
        UseSurface(surface);
        g_lightWarpView = WarpsTotalLight(MaterialBits(marks)) ? kNoView : LightWarpView(marks);
        diffuse = ClampSample(light * LightWarp(surface.normal, sample.toLight));
        Phong phong = PhongAt(pixel, true, surface.position, SurfaceAt(g_frame.surfaceView, pixel).xyz);
        highlight = ClampSample(max(ModelHighlight(phong, light, sample.toLight), ModelRim(phong, light, sample.toLight)));
    }
    RWTexture2D<float4> remainder = ResourceDescriptorHeap[g_pass.output0];
    remainder[pixel] = float4(diffuse, 1.0);
    if (any(highlight > 0.0)) {
        RWTexture2D<float4> highlights = ResourceDescriptorHeap[g_pass.output1];
        highlights[pixel] = float4(highlights[pixel].rgb + highlight, 0.0);
    }
    if (g_pass.output2 != kNoView) {
        RWTexture2D<float4> debug = ResourceDescriptorHeap[g_pass.output2];
        float3 colour = 0.0;
        if (RTXDI_IsValidDIReservoir(reservoir))
            colour = g_frame.debugView == 13 ? LightColour(RTXDI_GetDIReservoirLightIndex(reservoir)) : saturate(reservoir.M / 20.0);
        debug[pixel] = float4(colour, 1.0);
    }
}
