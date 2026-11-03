#include "common.hlsli"

// The screen rectangle around the pixels a run draws (G-buffer surfaces and water) for the viewmodel's and the 3D
// skybox's runs, widened by kMargin for the passes that read around a pixel, and an indirect dispatch of 8 x 8 groups
// over it (DispatchPixel). step 0 resets the bounds, step 1 grows them by each covered pixel and clears input0 (the 3D
// skybox's distances, which its rectangle's passes write only inside it; or kNoView), step 2 writes the dispatch.
// output0: bounds (min xy, max xy), dispatch arguments at byte 16, origin at byte 32.

// The GI filter reaches 2 * 16 pixels; the others less.
static const uint kMargin = 40;

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer rect = ResourceDescriptorHeap[g_pass.output0];
    if (g_pass.step == 0) {
        if (all(id == 0))
            rect.Store4(0, uint4(0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0));
        return;
    }
    if (g_pass.step == 2) {
        if (any(id != 0))
            return;
        uint4 bounds = rect.Load4(0);
        uint2 origin = 0;
        uint2 groups = 0;
        if (bounds.x <= bounds.z) {
            origin = uint2(max(int2(bounds.xy) - int(kMargin), 0)) & ~7u;
            uint2 end = min(bounds.zw + kMargin + 1, g_frame.size);
            groups = (end - origin + 7) / 8;
        }
        rect.Store3(16, uint3(groups, 1));
        rect.Store2(32, origin);
        return;
    }
    Texture2D<float4> flags = ResourceDescriptorHeap[g_frame.flagsView];
    bool inside = all(id.xy < g_frame.size);
    if (inside && g_pass.input0 != kNoView) {
        RWTexture2D<float> distances = ResourceDescriptorHeap[g_pass.input0];
        distances[id.xy] = 0.0;
    }
    float4 coverage = inside ? flags.Load(int3(id.xy, 0)) : 0.0;
    bool covered = inside && (coverage.r >= 0.5 || coverage.g >= 0.5);
    uint2 low = WaveActiveMin(covered ? id.xy : uint2(0xFFFFFFFFu, 0xFFFFFFFFu));
    uint2 high = WaveActiveMax(covered ? id.xy : uint2(0, 0));
    if (WaveActiveAnyTrue(covered) && WaveIsFirstLane()) {
        rect.InterlockedMin(0, low.x);
        rect.InterlockedMin(4, low.y);
        rect.InterlockedMax(8, high.x);
        rect.InterlockedMax(12, high.y);
    }
}
