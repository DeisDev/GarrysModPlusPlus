#include "scene.hlsli"
#include "froxels.hlsli"

// Volumetric light in a froxel volume (shaders/froxels.hlsli), a grid fitted to the view's frustum, as in Wronski 2014
// ("Volumetric Fog", Assassin's Creed IV) and Hillaire 2015 ("Physically Based and Unified Volumetric Rendering in
// Frostbite"). The medium is the water the eye is in (g_frame.eyeWater), the map's fog, lit (g_frame.volumetricFog:
// LitFog), or else a thin, even haze.
//
// Scatter (step 0, a thread per froxel): the light the medium scatters towards the eye per unit of its opacity, before
// the tonemap scale, at a point jittered within the froxel each frame (froxelJitter): the sun's with its shadow, the fog's
// and the water's own, and in the main view's fog and water one map or dynamic light picked by SampleLight with its
// shadow ray. It is blended into last frame's light at the point's place in last frame's volume, 5% new once enough
// frames are in (Hillaire's), which averages the jitter and the light sampling. Projected lights (flashlights) move with
// the player and would leave trails in that history: they are traced at each froxel's centre every frame, into a volume
// of their own. input0: last frame's light (rgb, frames averaged); input1-3: the main view's indirect light volume
// (shaders/probes.hlsl) or kNoView; input4: 1 when last frame's light may be used, plus 2 to sample map and dynamic
// lights; output0: this frame's light; output1: the projected lights' (rgb).
//
// Integrate (step 1, a thread per froxel column): walks the column from the eye, adding each froxel's light times the
// opacity the medium gains across it (Hillaire's energy-conserving integration: the transmittance before the froxel
// minus after it), and writes the light gathered by each froxel's far side and the transmittance there. input0, input1:
// the scatter's outputs; output0: the integrated volume (FroxelLight).

// The haze reaches this far at most.
static const float kMaxDistance = 4096.0;
// Strongly forward scattering: the haze shows as shafts and beams seen against the light, not as a wash over distant
// surfaces (g = 0.6 hazed whole maps over, 2026-10-31).
static const float kPhaseG = 0.8;
static const float kHistoryShare = 0.05;

float Phase(float cosine)
{
    float g2 = kPhaseG * kPhaseG;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(1.0 + g2 - 2.0 * kPhaseG * cosine, 1e-4), 1.5));
}

// The screen position of a point within the froxel grid (cells and their fractions).
float2 FroxelUv(float2 cell)
{
    return cell / float2(g_frame.froxelDims.xy) / g_frame.froxelUvScale;
}

// The eye ray through screen position `uv`: the step along it per unit of view depth.
float3 FroxelRay(float2 uv)
{
    float4 toPoint = mul(g_frame.clipToEye, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
    float3 offset = toPoint.xyz / toPoint.w;
    return offset / max(dot(g_frame.viewProj[3].xyz, offset), 1e-6);
}

// The share of the light from view depth `viewDepth` along a ray (`rayScale` units per unit of view depth) that the
// medium lets through to the eye: water's by colour (Beer-Lambert), Source's fog's (1 - FogOpacity) or the haze's.
float3 MediumTransmittance(float viewDepth, float rayScale)
{
    if (g_frame.eyeWater != kNoView)
        return exp(-LoadWaterBody(g_frame.eyeWater).extinction * (viewDepth * rayScale));
    if (g_frame.volumetricFog != 0)
        return 1.0 - FogOpacity(viewDepth * rayScale);
    return exp(-g_frame.volumetricDensity * min(viewDepth * rayScale, kMaxDistance));
}

// One map or dynamic light picked by SampleLight, with its shadow ray, scattered towards a ray along `direction` by the
// air's phase function or, in water, by the water's.
float3 SampledLamp(float3 position, float3 direction, bool water, inout uint seed)
{
    LightSet set = LightSetAt(position);
    set.count = set.pointLights;
    float3 irradiance;
    float3 toLight;
    float distance;
    uint picked;
    if (!SampleLight(set, position, 0.0, seed, irradiance, toLight, distance, picked))
        return 0.0;
    float phase = water ? WaterPhase(dot(direction, toLight)) : Phase(dot(direction, toLight));
    return irradiance * phase * LightTransmittance(set, picked, position, position + toLight * distance, float2(Random(seed), Random(seed)));
}

// The light the medium scatters towards the eye at a point per unit of its opacity, without projected lights. Radiance
// scattered from irradiance E is opacity * phase * pi * E in the units the lighting uses; the water's phase function
// already averages 1 over all directions (WaterLampLight).
float3 MediumLight(float3 position, float3 direction, uint3 volume, bool lamps, inout uint seed)
{
    if (g_frame.eyeWater != kNoView) {
        WaterBody body = LoadWaterBody(g_frame.eyeWater);
        float3 light = LitWater(position, body, direction);
        if (lamps)
            light += SampledLamp(position, direction, true, seed) * WaterAlbedo(body);
        return light;
    }
    if (g_frame.volumetricFog != 0) {
        float3 light = LitFog(position, volume);
        if (lamps)
            light += 3.14159265 * SampledLamp(position, direction, false, seed);
        return light;
    }
    if (g_frame.sunEnabled == 0.0)
        return 0.0;
    float3 light = 3.14159265 * g_frame.sunColor * Phase(dot(direction, g_frame.sunDirection)) * SunVisibility(position, g_frame.sunShadowMask);
    // Under Source's water fog without a body of water (fogParams.y, shaders/deferred_lighting.hlsl) the haze's light
    // crosses the water to the eye and takes its colour; without that the air's haze washed the water's fog out to grey.
    if (g_frame.fogEnabled != 0 && g_frame.fogParams.y > 0.0) {
        float3 tint = g_frame.fogColor / max(max(g_frame.fogColor.r, max(g_frame.fogColor.g, g_frame.fogColor.b)), 1e-4);
        light *= tint * (1.0 - FogOpacity(length(position - g_frame.eyePosition)));
    }
    return light;
}

// The projected lights' light scattered towards the eye at a point per unit of the medium's opacity, each with its shadow
// ray (ProjectorTransmittance, along the path a water surface bends it to).
float3 ProjectedLight(float3 position, float3 direction)
{
    bool water = g_frame.eyeWater != kNoView;
    float3 light = 0.0;
    for (uint p = 0; p < g_frame.projectorCount; ++p) {
        Projector projector = LoadProjector(p);
        float3 toLight;
        float distance;
        float3 irradiance = ProjectorIrradiance(projector, position, 0.0, toLight, distance);
        if (all(irradiance <= 0.0))
            continue;
        float phase = water ? WaterPhase(dot(direction, toLight)) : 3.14159265 * Phase(dot(direction, toLight));
        light += irradiance * phase * ProjectorTransmittance(projector, position);
    }
    return water ? light * WaterAlbedo(LoadWaterBody(g_frame.eyeWater)) : light;
}

void Scatter(uint3 id)
{
    float slices = float(g_frame.froxelDims.z);
    float3 cell = float3(id) + g_frame.froxelJitter;
    float2 uv = FroxelUv(cell.xy);
    float3 ray = FroxelRay(uv);
    float viewDepth = FroxelDepth(cell.z / slices, g_frame.froxelStart, g_frame.froxelRange);
    float3 position = g_frame.eyePosition + ray * viewDepth;
    float3 direction = normalize(ray);
    uint seed = PixelSeed(uint2(id.x + id.z * g_frame.froxelDims.x, id.y), 0xB1u);
    uint3 volume = uint3(g_pass.input1, g_pass.input2, g_pass.input3);
    float3 light = MediumLight(position, direction, volume, (g_pass.input4 & 2u) != 0, seed);
    // A sample brighter on screen than ClampSample allows, over the opacity its froxel adds, is scaled down to it.
    float rayScale = length(ray);
    float near = FroxelDepth(float(id.z) / slices, g_frame.froxelStart, g_frame.froxelRange);
    float far = FroxelDepth(float(id.z + 1) / slices, g_frame.froxelStart, g_frame.froxelRange);
    float gained = Luminance(max(MediumTransmittance(near, rayScale) - MediumTransmittance(far, rayScale), 0.0));
    light = gained > 0.0 ? ClampSample(light * gained) / gained : light;

    float frames = 1.0;
    if ((g_pass.input4 & 1u) != 0) {
        float4 clip = mul(g_frame.prevViewProj, float4(position, 1.0));
        float2 previousUv = clip.xy / max(clip.w, 1e-6) * float2(0.5, -0.5) + 0.5;
        float previousSlice = FroxelSlice(clip.w, g_frame.prevFroxelStart, g_frame.prevFroxelRange);
        if (clip.w > 0.0 && all(previousUv >= 0.0) && all(previousUv <= 1.0) && clip.w >= g_frame.prevFroxelStart && previousSlice <= 1.0) {
            Texture3D<float4> history = ResourceDescriptorHeap[g_pass.input0];
            float4 previous = history.SampleLevel(g_linearClamp, float3(previousUv * g_frame.froxelUvScale, previousSlice), 0.0);
            frames = min(previous.a + 1.0, 1.0 / kHistoryShare);
            light = lerp(previous.rgb, light, 1.0 / frames);
        }
    }
    RWTexture3D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    output[id] = float4(light, frames);

    float3 projected = 0.0;
    if (g_frame.projectorCount != 0) {
        float3 centre = float3(id) + 0.5;
        float3 centreRay = FroxelRay(FroxelUv(centre.xy));
        float3 centrePosition = g_frame.eyePosition + centreRay * FroxelDepth(centre.z / slices, g_frame.froxelStart, g_frame.froxelRange);
        projected = ProjectedLight(centrePosition, normalize(centreRay));
        projected = gained > 0.0 ? ClampSample(projected * gained) / gained : projected;
    }
    RWTexture3D<float3> projectors = ResourceDescriptorHeap[g_pass.output1];
    projectors[id] = projected;
}

void Integrate(uint2 column)
{
    uint slices = g_frame.froxelDims.z;
    float rayScale = length(FroxelRay(FroxelUv(float2(column) + 0.5)));
    Texture3D<float4> light = ResourceDescriptorHeap[g_pass.input0];
    Texture3D<float4> projected = ResourceDescriptorHeap[g_pass.input1];
    RWTexture3D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    float3 before = MediumTransmittance(g_frame.froxelStart, rayScale);
    float3 gathered = 0.0;
    [loop]
    for (uint k = 0; k < slices; ++k) {
        float3 after = MediumTransmittance(FroxelDepth(float(k + 1) / float(slices), g_frame.froxelStart, g_frame.froxelRange), rayScale);
        int4 texel = int4(column, k, 0);
        gathered += max(before - after, 0.0) * (light.Load(texel).rgb + projected.Load(texel).rgb);
        output[uint3(column, k)] = float4(gathered, Luminance(after));
        before = after;
    }
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_frame.froxelDims.xy))
        return;
    g_inAir = true;
    if (g_pass.step == 0)
        Scatter(id);
    else
        Integrate(id.xy);
}
