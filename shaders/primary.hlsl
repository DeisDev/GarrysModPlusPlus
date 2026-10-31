#include "scene.hlsli"

// The triangle each G-buffer pixel shows, found with a ray from the eye: its instance, triangle and barycentrics
// (a visibility buffer for later passes), whether its material is unlit, and where the point was last frame.
// Instances keep last frame's transform and skinned ones last frame's vertices (Scene::InstanceRecord), so the
// motion covers moving objects as well as the camera; pixels without a matching triangle follow the camera.
// output0: visibility (instance, triangle, barycentrics; instance kNoView where nothing matches the G-buffer);
// output1: motion in pixels towards last frame's position; output2: last frame's distance from the eye;
// output3: kPixelHit, kPixelUnlit and kPixelIgnoreNormals flags; output4: the triangle's face normal facing the eye, or the
// G-buffer normal without a triangle (the normal map's normal can point into the surface, which rays must not).
// input0: with step 1 (the 3D skybox view), a UAV for each pixel's distance from the eye in the map's space (0 for
// none), its surfaces being 1 / worldToSky times nearer in the skybox's; otherwise (the main view) that distance, for
// the pixels the main view leaves empty, or none.

float3 InterpolatedPosition(uint positionsView, uint3 index, float2 barycentrics)
{
    ByteAddressBuffer positions = ResourceDescriptorHeap[NonUniformResourceIndex(positionsView)];
    float3 p0 = positions.Load<float3>(index.x * 12);
    return p0 + barycentrics.x * (positions.Load<float3>(index.y * 12) - p0) + barycentrics.y * (positions.Load<float3>(index.z * 12) - p0);
}

// How far a hit point moved since last frame, in world space.
float3 HitMotion(HitInfo hit)
{
    InstanceRecord record = LoadInstance(hit.instance);
    ByteAddressBuffer indices = ResourceDescriptorHeap[NonUniformResourceIndex(record.indices)];
    uint3 index = indices.Load3(hit.primitive * 12);
    float3 current = InterpolatedPosition(record.positions, index, hit.barycentrics);
    float3 previous = record.previousPositions != kNoView ? InterpolatedPosition(record.previousPositions, index, hit.barycentrics) : current;
    float3x4 previousTransform = float3x4(record.previousTransform[0], record.previousTransform[1], record.previousTransform[2]);
    return mul(previousTransform, float4(previous, 1.0)) - mul(hit.objectToWorld, float4(current, 1.0));
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    id.xy = DispatchPixel(id.xy);
    if (any(id.xy >= g_frame.size))
        return;
    int2 pixel = int2(id.xy);
    RWTexture2D<uint4> visibility = ResourceDescriptorHeap[g_pass.output0];
    RWTexture2D<float2> motion = ResourceDescriptorHeap[g_pass.output1];
    RWTexture2D<float> previousDistance = ResourceDescriptorHeap[g_pass.output2];
    RWTexture2D<uint> flags = ResourceDescriptorHeap[g_pass.output3];
    RWTexture2D<float4> geometricNormal = ResourceDescriptorHeap[g_pass.output4];
    float4 surface = SurfaceAt(g_frame.surfaceView, pixel);
    float depth = DepthAt(pixel);
    float3 position = WorldPositionFromDepth(pixel, depth);
    if (g_pass.input0 != kNoView && g_pass.step == 1) {
        RWTexture2D<float> skyboxDistance = ResourceDescriptorHeap[g_pass.input0];
        skyboxDistance[pixel] = depth < 1.0 && g_frame.worldToSky > 0.0 ? length(position - g_frame.eyePosition) / g_frame.worldToSky : 0.0;
    } else if (g_pass.input0 != kNoView && depth >= 1.0) {
        // The skybox view shares this view's orientation and jitter, and may differ in resolution (Ray Reconstruction).
        Texture2D<float> skyboxDistance = ResourceDescriptorHeap[g_pass.input0];
        uint2 skyboxSize;
        skyboxDistance.GetDimensions(skyboxSize.x, skyboxSize.y);
        int2 source = min(int2((float2(pixel) + 0.5) * float2(skyboxSize) / float2(g_frame.size)), int2(skyboxSize) - 1);
        float distance = skyboxDistance.Load(int3(source, 0));
        if (distance > 0.0)
            position = g_frame.eyePosition + normalize(position - g_frame.eyePosition) * distance;
    }
    float3 previous = position;
    uint4 visible = uint4(kNoView, 0, 0, 0);
    uint pixelFlags = 0;
    float3 faceNormal = surface.xyz;
    if (surface.w > 0.0 && g_frame.traceMask != 0) {
        float3 view = normalize(position - g_frame.eyePosition);
        HitInfo hit;
        // The eye is inside the local player's body. Glass a few units in front of a surface would otherwise give it
        // the glass's material.
        if (TraceHit(g_frame.eyePosition, view, surface.w + 8.0 + surface.w * 0.02, g_frame.traceMask & ~kLocalPlayerMask, hit, RAY_FLAG_NONE, true) &&
            abs(hit.t - surface.w) <= 4.0 + surface.w * 0.02) {
            visible = uint4(hit.instance, hit.primitive, asuint(hit.barycentrics));
            // An unlit pixel shows its G-buffer albedo as it is, so the hit must be the surface the G-buffer shows: at a
            // silhouette the ray can slip past it to an unlit surface just behind (gm_skywatching's black window backings
            // turned the white window frames' edges into bright lines).
            bool exact = abs(hit.t - surface.w) <= 0.5 + surface.w * 0.004;
            pixelFlags = kPixelHit | (exact && (MaterialFlags(hit) & kMaterialUnlit) != 0 ? kPixelUnlit : 0u) |
                ((LoadInstance(hit.instance).flags & kInstanceIgnoreNormals) != 0 ? kPixelIgnoreNormals : 0u);
            previous = position + HitMotion(hit);
            faceNormal = SurfaceOf(hit, g_frame.eyePosition, view).normal;
        }
    }
    float2 pixelMotion = 0.0;
    float4 currentClip = mul(g_frame.viewProj, float4(position, 1.0));
    float4 previousClip = mul(g_frame.prevViewProj, float4(previous, 1.0));
    if (g_frame.historyValid != 0 && currentClip.w > 0.0 && previousClip.w > 0.0) {
        float2 currentUv = float2(currentClip.x / currentClip.w * 0.5 + 0.5, 0.5 - currentClip.y / currentClip.w * 0.5);
        float2 previousUv = float2(previousClip.x / previousClip.w * 0.5 + 0.5, 0.5 - previousClip.y / previousClip.w * 0.5);
        pixelMotion = (previousUv - currentUv) * float2(g_frame.size);
    }
    visibility[pixel] = visible;
    motion[pixel] = pixelMotion;
    previousDistance[pixel] = length(previous - g_frame.prevEyePosition);
    flags[pixel] = pixelFlags;
    geometricNormal[pixel] = float4(faceNormal, 0.0);
}
