#include "scene.hlsli"
#include "froxels.hlsli"

// Volumetric light in a froxel volume (shaders/froxels.hlsli), a grid fitted to the view's frustum, as in Wronski 2014
// ("Volumetric Fog", Assassin's Creed IV) and Hillaire 2015 ("Physically Based and Unified Volumetric Rendering in
// Frostbite"). The medium is the water the eye is in (g_frame.eyeWater), the map's fog, lit (g_frame.volumetricFog:
// LitFog), or else a thin, even haze, with ground mist over it where that is on (g_frame.mistEnabled): an exponential
// height fog broken up by noise that drifts with the wind (MistSigma), whose extinction varies, so the scatter keeps
// each froxel's in a volume of its own (output2, last frame's in input5), which the integration then takes.
//
// Scatter (step 0, a thread per froxel): the light the medium scatters towards the eye per unit of its opacity, before
// the tonemap scale, at a point jittered within the froxel each frame (froxelJitter): the sun's with its shadow, the fog's
// and the water's own, and in the main view's fog and water one map or dynamic light with its shadow ray, averaged over
// the froxel's stretch of the eye ray (SampledLampAlong). It is blended into last frame's light at the point's place in
// last frame's volume, 5% new once enough frames are in (Hillaire's), which averages the jitter and the light sampling.
// Projected lights (flashlights) move with
// the player and would leave trails in that history: they are traced at each froxel's centre every frame, into a volume
// of their own. input0: last frame's light (rgb, frames averaged); input1-3: the main view's indirect light volume
// (shaders/probes.hlsl) or kNoView; input4: 1 when last frame's light may be used, plus 2 to sample map and dynamic
// lights; output0: this frame's light; output1: the projected lights' (rgb); with ground mist, input5: last frame's
// extinction, output2: this frame's.
//
// Integrate (step 1, a thread per froxel column): walks the column from the eye, adding each froxel's light times the
// opacity the medium gains across it (Hillaire's energy-conserving integration: the transmittance before the froxel
// minus after it), and writes the light gathered by each froxel's far side and the transmittance there. input0, input1:
// the scatter's outputs; with ground mist, input2: the froxels' extinction; output0: the integrated volume (FroxelLight).

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

// Gradient noise (Perlin 2002's quintic fade) in [-1, 1], mean 0, from a hash of the lattice.
float3 LatticeGradient(int3 cell)
{
    uint h = Hash(uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u ^ uint(cell.z) * 83492791u);
    float3 g = float3(h & 1023u, (h >> 10) & 1023u, (h >> 20) & 1023u) / 511.5 - 1.0;
    return g / max(length(g), 1e-4);
}

float GradientNoise(float3 p)
{
    int3 cell = int3(floor(p));
    float3 f = p - float3(cell);
    float3 fade = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float corners[8];
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        int3 corner = int3(i & 1, (i >> 1) & 1, i >> 2);
        corners[i] = dot(LatticeGradient(cell + corner), f - float3(corner));
    }
    float4 x = lerp(float4(corners[0], corners[2], corners[4], corners[6]), float4(corners[1], corners[3], corners[5], corners[7]), fade.x);
    float2 y = lerp(x.xz, x.yw, fade.y);
    return lerp(y.x, y.y, fade.z);
}

// Ground mist's extinction per unit at a point: thinning by e every 1 / mistInvFalloff units above mistBase, broken up by
// three octaves of noise mistTurbulence strong, mistInvScale per unit, carried by the wind (mistOffset).
float MistSigma(float3 position)
{
    float3 p = (position + g_frame.mistOffset) * g_frame.mistInvScale;
    float noise = 0.5714 * GradientNoise(p) + 0.2857 * GradientNoise(p * 2.03 + 17.1) + 0.1429 * GradientNoise(p * 4.01 + 41.7);
    float above = clamp((position.z - g_frame.mistBase) * g_frame.mistInvFalloff, -60.0, 60.0);
    return g_frame.mistDensity * exp(-above) * max(0.0, 1.0 + 2.0 * g_frame.mistTurbulence * noise);
}

float MistPhase(float cosine)
{
    float g = g_frame.mistPhaseG;
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(1.0 + g2 - 2.0 * g * cosine, 1e-4), 1.5));
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

// A distance along the ray from `origin` along `direction`, within [near, far], drawn with a density proportional to the
// inverse square of its point's distance to `light` (equiangular sampling: Kulla and Fajardo 2012, "Importance Sampling
// Techniques for Path Tracing in Participating Media"), which cancels a point light's falloff along the ray. The density
// is taken as if the ray passed at least `spread` from the light, so it stays finite where the ray meets the light.
// `density` is the draw's probability density per unit of distance.
float EquiangularDistance(float3 light, float spread, float3 origin, float3 direction, float near, float far, float u, out float density)
{
    float closest = dot(light - origin, direction);
    float offset = max(length(origin + direction * closest - light), spread);
    float first = atan((near - closest) / offset);
    float last = atan((far - closest) / offset);
    float along = offset * tan(lerp(first, last, u));
    density = offset / (max(last - first, 1e-7) * (offset * offset + along * along));
    return closest + along;
}

// The light of one map or dynamic light scattered towards the eye by the air's phase function or, in water, the water's,
// averaged over the stretch from `near` to `far` (distances) of the eye ray along `direction`, with its shadow ray.
// Lights and points of the stretch are picked together by resampled importance sampling (Talbot 2005, as SampleLight
// picks lights): up to kLightCandidates lights drawn uniformly and one from the cell's tail, each at a distance drawn
// equiangularly towards it, one kept in proportion to its unshadowed light there, phase function included, over the
// density of its draw. Picked by their light at one point jittered through a froxel, lights flickered in distant froxels,
// which reach hundreds of units deep: the point fell near a lamp in a few frames and far from it in the rest, and the
// air's phase function (g = 0.8) weighs lights in front of the eye some 700 times those behind it.
float3 SampledLampAlong(float3 direction, float near, float far, bool water, inout uint seed)
{
    float3 eye = g_frame.eyePosition;
    LightSet set = LightSetAt(eye + direction * (0.5 * (near + far)));
    set.count = set.pointLights;
    bool exhaustive = set.count <= kLightCandidates;
    uint draws = exhaustive ? set.count : kLightCandidates;
    // Uniform draws weigh each candidate by count / draws.
    float drawWeight = exhaustive ? 1.0 : float(set.count) / float(max(draws, 1u));
    uint tailLight;
    float tailProbability;
    bool tail = SampleTailLight(set.ranges, Random(seed), tailLight, tailProbability);
    float total = 0.0;
    uint picked = 0;
    float3 pickedPoint = 0.0;
    float3 pickedLight = 0.0;
    float3 pickedIrradiance = 0.0;
    [loop]
    for (uint d = 0; d <= draws; ++d) {
        uint i;
        float weight;
        if (d < draws) {
            i = exhaustive ? d : min(uint(Random(seed) * set.count), set.count - 1);
            weight = drawWeight;
        } else {
            if (!tail)
                break;
            i = set.count + tailLight;
            weight = 1.0 / tailProbability;
        }
        Light light = SetPointLight(set, i);
        float density;
        float3 position = eye + direction * EquiangularDistance(light.origin, max(light.size, 1.0), eye, direction, near, far, Random(seed), density);
        float3 toLight;
        float distance;
        float3 irradiance = LightIrradiance(light, position, 0.0, toLight, distance);
        float cosine = dot(direction, toLight);
        float target = Luminance(irradiance) * (water ? WaterPhase(cosine) : Phase(cosine));
        if (target <= 0.0)
            continue;
        float resampling = target * weight / density;
        total += resampling;
        if (Random(seed) * total < resampling) {
            picked = i;
            pickedPoint = position;
            pickedLight = light.origin;
            pickedIrradiance = irradiance;
        }
    }
    if (total <= 0.0)
        return 0.0;
    // The picked light's irradiance and phase over its target leave its colour; the total over the stretch's length makes
    // the estimate the stretch's average.
    float3 shadow = LightTransmittance(set, picked, pickedPoint, pickedLight, float2(Random(seed), Random(seed)));
    return pickedIrradiance / Luminance(pickedIrradiance) * shadow * (total / max(far - near, 1e-4));
}

// The light ground mist scatters towards the eye per unit of its opacity: the sun's with its shadow, the sky's as much as
// reaches the point (the indirect light volume against the open sky's, as LitFog takes it, so mist under a roof is dark)
// and, in the main view, a lamp's, all times its albedo.
float3 MistLight(float3 position, float3 direction, float near, float far, uint3 volume, bool lamps, inout uint seed)
{
    float3 light = 0.0;
    if (g_frame.sunEnabled != 0.0)
        light += 3.14159265 * g_frame.sunColor * MistPhase(dot(direction, g_frame.sunDirection)) * SunVisibility(position, g_frame.sunShadowMask);
    float ambient = 1.0;
    if (volume.r != kNoView) {
        float3 indirect = VolumeIrradiance(volume, position, 0.0);
        if (indirect.r >= 0.0)
            ambient = saturate(Luminance(indirect) / max(0.5 * Luminance(g_frame.skyColor), 1e-6));
    }
    light += 0.5 * g_frame.skyColor * ambient;
    if (lamps)
        light += 3.14159265 * SampledLampAlong(direction, near, far, false, seed);
    return g_frame.mistColor * light;
}

// The light the medium scatters towards the eye at a point per unit of its opacity, without projected lights; the lamps'
// is averaged over the stretch [near, far] (distances) of the eye ray the point lies on. Radiance scattered from
// irradiance E is opacity * phase * pi * E in the units the lighting uses; the water's phase function already averages 1
// over all directions (WaterLampLight).
float3 MediumLight(float3 position, float3 direction, float near, float far, uint3 volume, bool lamps, inout uint seed)
{
    if (g_frame.eyeWater != kNoView) {
        WaterBody body = LoadWaterBody(g_frame.eyeWater);
        float3 light = LitWater(position, body, direction);
        if (lamps)
            light += SampledLampAlong(direction, near, far, true, seed) * WaterAlbedo(body);
        return light;
    }
    if (g_frame.volumetricFog != 0) {
        float3 light = LitFog(position, volume);
        if (lamps)
            light += 3.14159265 * SampledLampAlong(direction, near, far, false, seed);
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
    float rayScale = length(ray);
    float near = FroxelDepth(float(id.z) / slices, g_frame.froxelStart, g_frame.froxelRange);
    float far = FroxelDepth(float(id.z + 1) / slices, g_frame.froxelStart, g_frame.froxelRange);
    float3 light = MediumLight(position, direction, near * rayScale, far * rayScale, volume, (g_pass.input4 & 2u) != 0, seed);
    // A sample brighter on screen than ClampSample allows, over the opacity its froxel adds, is scaled down to it.
    float gained = Luminance(max(MediumTransmittance(near, rayScale) - MediumTransmittance(far, rayScale), 0.0));
    // With ground mist, the haze and the mist share the froxel by their extinction.
    float sigma = 0.0;
    if (g_frame.mistEnabled != 0.0) {
        float haze = viewDepth * rayScale < kMaxDistance ? g_frame.volumetricDensity : 0.0;
        float mist = MistSigma(position);
        sigma = haze + mist;
        float3 mistLight = MistLight(position, direction, near * rayScale, far * rayScale, volume, (g_pass.input4 & 2u) != 0, seed);
        light = sigma > 0.0 ? (haze * light + mist * mistLight) / sigma : 0.0;
        gained = 1.0 - exp(-sigma * (far - near) * rayScale);
    }
    light = gained > 0.0 ? ClampSample(light * gained) / gained : light;

    float frames = 1.0;
    if ((g_pass.input4 & 1u) != 0) {
        float4 clip = mul(g_frame.prevViewProj, float4(position, 1.0));
        float2 previousUv = clip.xy / max(clip.w, 1e-6) * float2(0.5, -0.5) + 0.5;
        float previousSlice = FroxelSlice(clip.w, g_frame.prevFroxelStart, g_frame.prevFroxelRange);
        if (clip.w > 0.0 && all(previousUv >= 0.0) && all(previousUv <= 1.0) && clip.w >= g_frame.prevFroxelStart && previousSlice <= 1.0) {
            Texture3D<float4> history = ResourceDescriptorHeap[g_pass.input0];
            float3 previousPoint = float3(previousUv * g_frame.froxelUvScale, previousSlice);
            float4 previous = history.SampleLevel(g_linearClamp, previousPoint, 0.0);
            frames = min(previous.a + 1.0, 1.0 / kHistoryShare);
            light = lerp(previous.rgb, light, 1.0 / frames);
            if (g_frame.mistEnabled != 0.0) {
                Texture3D<float> sigmaHistory = ResourceDescriptorHeap[g_pass.input5];
                sigma = lerp(sigmaHistory.SampleLevel(g_linearClamp, previousPoint, 0.0), sigma, 1.0 / frames);
            }
        }
    }
    RWTexture3D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    output[id] = float4(light, frames);
    if (g_frame.mistEnabled != 0.0) {
        RWTexture3D<float> sigmas = ResourceDescriptorHeap[g_pass.output2];
        sigmas[id] = sigma;
    }

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
    bool mist = g_frame.mistEnabled != 0.0;
    if (mist)
        before = 1.0;
    [loop]
    for (uint k = 0; k < slices; ++k) {
        int4 texel = int4(column, k, 0);
        float3 after;
        if (mist) {
            // Ground mist's extinction varies within the volume: each froxel's, over its stretch of the ray.
            Texture3D<float> sigmas = ResourceDescriptorHeap[g_pass.input2];
            float near = FroxelDepth(float(k) / float(slices), g_frame.froxelStart, g_frame.froxelRange);
            float far = FroxelDepth(float(k + 1) / float(slices), g_frame.froxelStart, g_frame.froxelRange);
            after = before * exp(-sigmas.Load(texel) * (far - near) * rayScale);
        } else {
            after = MediumTransmittance(FroxelDepth(float(k + 1) / float(slices), g_frame.froxelStart, g_frame.froxelRange), rayScale);
        }
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
