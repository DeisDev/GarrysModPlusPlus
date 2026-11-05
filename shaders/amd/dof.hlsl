// AMD FidelityFX Depth of Field 1.1 (FidelityFX SDK 1.1.4, MIT) with its callbacks on the post effects' bindless
// resources (src/post_effects.cpp). DOF_PASS picks the pass, as the SDK's DirectX 12 backend builds one shader per pass:
// 0 the tiles' circle of confusion range, 1 the half-resolution colour and its mips, 2 the tiles' dilation, 3 the near
// and far blur, 4 the composite at full resolution.

// No ring merging (the SDK's FFX_DOF_DISABLE_RING_MERGE): merged rings read their point samples two mips coarser,
// which left flat out-of-focus areas in blocks of 8 pixels.
#define FFX_DOF_OPTION_MAX_RING_MERGE_LOG 0
// The half-resolution colour's mips (kDofMips in src/post_effects.cpp): six, the most SPD makes without its atomic
// counter, so blur up to 64 pixels per ring finds the mip its samples' spacing asks for (four ran out at 16, and the
// sparse rings of a low quality setting skipped texels).
#define FFX_DOF_OPTION_MAX_MIP 6

#include "../post/post.hlsli"
#include "ffx_core.h"
#include "ffx_dof_common.h"

FfxUInt32x2 InputSize()
{
    return g_post.size;
}

FfxUInt32x2 InputSizeHalf()
{
    return (g_post.size + 1) / 2;
}

FfxFloat32x2 InputSizeHalfRcp()
{
    return 1.0 / FfxFloat32x2(InputSizeHalf());
}

FfxUInt32 MaxRings()
{
    return g_post.maxRings;
}

// The thin lens's circle of confusion at the focus distance CameraMain chose (shaders/post/post.hlsl), positive in
// front of it; the held weapon stays sharp.
FfxFloat32 FfxDofGetCoc(FfxFloat32 depth)
{
    if (IsViewmodelDepth(depth))
        return 0.0;
    RWStructuredBuffer<float4> camera = ResourceDescriptorHeap[g_post.cameraView];
    float focus = camera[0].x;
    float coc = g_post.cocScale * g_post.focalLength * (focus * InverseViewDepth(depth) - 1.0) / max(focus - g_post.focalLength, 1e-3);
    return clamp(coc, -g_post.cocLimit, g_post.cocLimit);
}

FfxHalfOpt4 FfxDofLoadSource(FfxInt32x2 tex)
{
    Texture2D<float4> color = ResourceDescriptorHeap[g_postPass.input0];
    return color.Load(int3(tex, 0));
}

void FfxDofStoreBilatMip(FfxUInt32 mip, FfxInt32x2 tex, FfxHalfOpt4 value)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[mip < 4 ? g_post.dofBilatMips[mip] : g_post.dofBilatMipsHigh[mip - 4]];
    output[tex] = value;
}

void FfxDofStoreDilatedRadius(FfxUInt32x2 coord, FfxFloat32x2 dilatedMinMax)
{
    RWTexture2D<float2> output = ResourceDescriptorHeap[g_post.dofDilatedUav];
    output[coord] = dilatedMinMax;
}

FfxFloat32x2 FfxDofSampleDilatedRadius(FfxUInt32x2 coord)
{
    Texture2D<float2> dilated = ResourceDescriptorHeap[g_post.dofDilated];
    return dilated.SampleLevel(g_postLinear, FfxFloat32x2(coord) * InputSizeHalfRcp(), 0);
}

FfxFloat32x2 FfxDofLoadDilatedRadius(FfxUInt32x2 coord)
{
    Texture2D<float2> dilated = ResourceDescriptorHeap[g_post.dofDilated];
    return dilated.Load(int3(coord, 0));
}

FfxHalfOpt4 FfxDofLoadInput(FfxUInt32x2 coord)
{
    Texture2D<float4> bilateral = ResourceDescriptorHeap[g_post.dofBilat];
    return bilateral.Load(int3(coord, 0));
}

FfxHalfOpt4 FfxDofSampleInput(FfxFloat32x2 coord, FfxUInt32 mip)
{
    Texture2D<float4> bilateral = ResourceDescriptorHeap[g_post.dofBilat];
    return bilateral.SampleLevel(g_postPoint, coord, mip);
}

// `coord` spans the view; the depth buffer spans the back buffer, from the view's corner.
FfxFloat32x4 FfxDofGatherDepth(FfxFloat32x2 coord)
{
    Texture2D<float> depth = ResourceDescriptorHeap[g_post.depthView];
    return depth.GatherRed(g_postLinear, (coord * FfxFloat32x2(g_post.size) + FfxFloat32x2(g_post.origin)) * g_post.invDepthSize);
}

FfxFloat32 FfxDofLoadFullDepth(FfxUInt32x2 coord)
{
    return DeviceDepth(int2(coord));
}

void FfxDofStoreNear(FfxUInt32x2 coord, FfxHalfOpt4 color)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_post.dofNearUav];
    output[coord] = color;
}

void FfxDofStoreFar(FfxUInt32x2 coord, FfxHalfOpt4 color)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_post.dofFarUav];
    output[coord] = color;
}

// The blur's results are read through shader resource views: RGBA16F typed UAV loads are optional for some formats, and
// the composite only reads them.
FfxHalfOpt4 FfxDofLoadNear(FfxUInt32x2 coord)
{
    Texture2D<float4> nearField = ResourceDescriptorHeap[g_post.dofNear];
    return nearField.Load(int3(coord, 0));
}

FfxHalfOpt4 FfxDofLoadFar(FfxUInt32x2 coord)
{
    Texture2D<float4> farField = ResourceDescriptorHeap[g_post.dofFar];
    return farField.Load(int3(coord, 0));
}

void FfxDofAccumMaxTileRadius(FfxUInt32 radius)
{
    RWStructuredBuffer<uint> globals = ResourceDescriptorHeap[g_post.dofGlobals];
    InterlockedMax(globals[0], radius);
}

FfxUInt32 FfxDofGetMaxTileRadius()
{
    RWStructuredBuffer<uint> globals = ResourceDescriptorHeap[g_post.dofGlobals];
    return globals[0];
}

void FfxDofResetMaxTileRadius()
{
    RWStructuredBuffer<uint> globals = ResourceDescriptorHeap[g_post.dofGlobals];
    globals[0] = 0;
}

void FfxDofStoreTileRadius(FfxUInt32x2 tile, FfxFloat32x2 radius)
{
    RWTexture2D<float2> output = ResourceDescriptorHeap[g_post.dofTileRadiusUav];
    output[tile] = radius;
}

// RG16F has no guaranteed typed UAV loads (Microsoft Learn, "Typed unordered access view (UAV) loads"): the dilation
// reads the tiles through a shader resource view.
FfxFloat32x2 FfxDofLoadTileRadius(FfxUInt32x2 tile)
{
    Texture2D<float2> tiles = ResourceDescriptorHeap[g_post.dofTileRadius];
    return tiles.Load(int3(tile, 0));
}

FfxFloat32x4 FfxDofLoadFullInput(FfxUInt32x2 coord)
{
    Texture2D<float4> color = ResourceDescriptorHeap[g_postPass.input0];
    return color.Load(int3(coord, 0));
}

void FfxDofStoreOutput(FfxUInt32x2 coord, FfxFloat32x4 color)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    output[coord] = color;
}

#if DOF_PASS == 0
#include "ffx_dof_downsample_depth.h"

[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 dtID : SV_DispatchThreadID)
{
    DownsampleDepth(dtID.xy, InputSize());
}
#elif DOF_PASS == 1
#include "ffx_dof_downsample_color.h"

[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void main(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID)
{
    DownsampleColor(gi, gid.xy);
}
#elif DOF_PASS == 2
#include "ffx_dof_dilate_depth.h"

[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 dtID : SV_DispatchThreadID)
{
    FfxDofDilate(dtID.xy, InputSize());
}
#elif DOF_PASS == 3
#include "ffx_dof_blur.h"

[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 dtID : SV_DispatchThreadID)
{
    FfxDofBlur(dtID.xy, InputSizeHalf());
}
#else
#include "ffx_dof_composite.h"

[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 dtID : SV_DispatchThreadID, uint3 gtID : SV_GroupThreadID, uint3 gid : SV_GroupID, uint gix : SV_GroupIndex)
{
    FfxDofCombineHalfRes(dtID.xy, gtID.xy, gid.xy, gix, InputSizeHalf(), InputSize());
}
#endif
