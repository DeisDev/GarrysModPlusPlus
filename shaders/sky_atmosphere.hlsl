#include "common.hlsli"
#include "night_sky.hlsli"

// The procedural sky that replaces the game's 2D sky (shaders/atmosphere.hlsli), in steps:
// 0: the transmittance table (output0, kTransmittanceSize).
// 1: the multiple scattering table (output0, kMultipleScatteringSize square) from the transmittance (input0).
// 2: the clear sky around an eye kEyeHeight above the ground, lit by sunlight of irradiance 1 from the direction in
//    input2-4 (float bits), into a cube's faces (output0) from both tables (input0, input1). Alpha: the luminance of its
//    aerosols' single scattering, the glow around the sun, which step 4 keeps in the sunlight's colour.
// 3: the game's colours (one group): the mean radiance of the game's sky (input0, the faces of the capture's cube, -1
//    where nothing was drawn) and of the sky with the map's sun (input1, step 2's sky for light_environment's sun, which
//    the game's sky shows) in kBands bands of elevation, and from them each band's multiplier (output0, kBands x 1). The
//    map's sky counts with the night's floor (input2, float bits), so that it still matches the game's. Above the horizon
//    a band keeps its brightness and takes the game's colour (alpha: the multiplier of its luminance alone), and one
//    scale makes the sky above the horizon as bright as the game's; below it, where the clear sky is only a ground of guessed albedo, a band takes the game's colour and
//    brightness. Without the game's sky above the horizon the scale makes it as bright as the map's sky light (input3,
//    float bits: its luminance). Texel kBands: the mean luminance of the game's sky above the horizon where it was drawn
//    (x, -1 without any), which tells night maps (Lighting::ReadMapSky), and the scale (y), which is how bright sunlight
//    of irradiance 1 is in the procedural sky's units, as the clouds (shaders/clouds.hlsl) are lit.
// 4: the procedural sky (output0, a cube's faces): step 2's sky for the sun the sky shows (input0), plus the moonless
//    night's floor times the sky with the map's sun (input2) and the Moon's share times the sky it lights (input4, step
//    2's sky for the Moon of day maps), times the multiplier of its elevation (input1), a smooth curve through the bands;
//    the floor and shares come from the slot's NightSkyConstants (input3). So the sky is the game's where the sun is the
//    map's, and darkens and reddens as the sun sets as the clear sky does, down to the floor and the moonlit sky. Alpha:
//    log2 of how much brighter it is than the physical sky (the sun's at the map's sun's irradiance, the Moon's at its
//    own) with a natural night sky's own light (kNightSkyRadiance), the gain the night sky's stars and the Moon are drawn
//    with (shaders/sky_draw.hlsl), as the eye adapted to this sky would see them. The sun's sky counts as dark as it would
//    be with the sun Settings::earlierStars lower (NightSkyConstants::starTwilight), so the night sky comes out earlier.
// 5: the daylight table (output0, kDaylightEntries x 1), one group per entry, for the CPU (Lighting::DaylightAt): for the
//    sun at the entry's elevation (DaylightElevation), the transmittance toward it from the eye (rgb) and the luminance
//    of the clear sky's irradiance on a surface facing up (a), from both tables (input0, input1).
// 6: the Milky Way's starlight in S10 units into a cube's faces (output0), in the stars' frame (MilkyWayStarlight in
//    shaders/night_sky.hlsli).

static const uint kBands = 32;
static const uint kLanesPerBand = 8;
// Each band is sampled at kAzimuths x kHeights directions spaced evenly in azimuth and in the sine of elevation, so each
// stands for the same solid angle; every lane takes kHeights / kLanesPerBand rows.
static const uint kAzimuths = 32;
static const uint kHeights = 16;

static const uint kTransmittanceSteps = 40;
static const uint kMultipleScatteringSteps = 20;
static const uint kSkySteps = 32;

// The daylight table's entries span the sun's elevation from -90 to 90 degrees (kDaylightEntries in src/lighting.cpp);
// each integrates the sky over kDaylightRings x kDaylightAzimuths directions, even in azimuth and in the cosine to the
// zenith.
static const uint kDaylightRings = 16;
static const uint kDaylightAzimuths = 32;

static const float kIsotropicPhase = 1.0 / (4.0 * 3.14159265);

groupshared float4 g_capturedSums[kBands * kLanesPerBand];
groupshared float3 g_skySums[kBands * kLanesPerBand];
groupshared float3 g_captured[kBands];
groupshared float3 g_sky[kBands];
groupshared float g_scale;
groupshared float g_daylightSums[kBands * kLanesPerBand];

struct Scattered
{
    float3 radiance;
    // The aerosols' single scattering in it.
    float3 mie;
    // Hillaire's f_ms (eq. 7, before the isotropic phase): the share of light scattered once more along the ray.
    float3 transfer;
};

// Sunlight of irradiance 1 from `sun` scattered toward `origin` along `direction` (Hillaire 2020, eq. 1-11, each step's
// in-scattering integrated over the step against its extinction as his implementation does), plus the sunlit ground
// where the ray ends on it. With `multiple`, the scattering follows the Rayleigh and aerosol phase functions and adds
// the multiple scattering table's light (eq. 11); without, it is isotropic and the table is what is being made.
Scattered Integrate(float3 origin, float3 direction, float3 sun, uint steps, bool multiple, Texture2D<float4> transmittance,
    Texture2D<float4> multipleScattering)
{
    Scattered result = (Scattered)0;
    float ground = SphereDistance(origin, direction, kGroundRadius);
    float span = ground >= 0.0 ? ground : SphereDistance(origin, direction, kAtmosphereRadius);
    if (span <= 0.0)
        return result;
    float cosine = dot(direction, sun);
    float rayleighPhase = multiple ? RayleighPhase(cosine) : kIsotropicPhase;
    float miePhase = multiple ? MiePhase(cosine) : kIsotropicPhase;
    float3 throughput = 1.0;
    float start = 0.0;
    [loop]
    for (uint i = 0; i < steps; ++i) {
        // Steps lengthen with the distance, as the air thins.
        float end = span * pow((i + 1.0) / steps, 2.0);
        float step = end - start;
        float3 position = origin + direction * (start + 0.5 * step);
        start = end;
        float r = length(position);
        float sunMu = dot(position, sun) / r;
        Medium medium = MediumAt(r - kGroundRadius);
        float3 sunlight = SphereDistance(position, sun, kGroundRadius) < 0.0 ? AtmosphereTransmittance(transmittance, g_linearClamp, r, sunMu) : 0.0;
        float3 scattering = medium.rayleigh + medium.mie;
        float3 mieScattered = sunlight * (medium.mie * miePhase);
        float3 inScattered = sunlight * (medium.rayleigh * rayleighPhase) + mieScattered;
        if (multiple)
            inScattered += scattering * multipleScattering.SampleLevel(g_linearClamp, MultipleScatteringUv(r, sunMu), 0.0).rgb;
        float3 extinction = max(medium.extinction, 1e-9);
        float3 stepTransmittance = exp(-extinction * step);
        result.radiance += throughput * (inScattered - inScattered * stepTransmittance) / extinction;
        result.mie += throughput * (mieScattered - mieScattered * stepTransmittance) / extinction;
        result.transfer += throughput * (scattering - scattering * stepTransmittance) / extinction;
        throughput *= stepTransmittance;
    }
    if (ground >= 0.0) {
        float3 position = origin + direction * ground;
        float sunMu = dot(normalize(position), sun);
        result.radiance += throughput * AtmosphereTransmittance(transmittance, g_linearClamp, kGroundRadius, sunMu) * (saturate(sunMu) * kGroundAlbedo / 3.14159265);
    }
    return result;
}

float BandElevation(float band)
{
    return (band + 0.5) / kBands * 3.14159265 - 0.5 * 3.14159265;
}

// A band's multiplier from the means of the game's sky (w: the share of its directions drawn) and of the clear sky.
float3 BandMultiplier(uint band, float scale)
{
    float3 captured = g_captured[band];
    float3 sky = g_sky[band];
    float capturedLuminance = Luminance(captured);
    float skyLuminance = Luminance(sky);
    if (capturedLuminance <= 0.0 || skyLuminance <= 0.0)
        return scale;
    float3 ratio = captured / max(sky, skyLuminance * 1e-6);
    return BandElevation(band) > 0.0 ? scale * ratio * (skyLuminance / capturedLuminance) : ratio;
}

void ColourBands(uint index)
{
    Texture2DArray<float4> captured = ResourceDescriptorHeap[g_pass.input0];
    Texture2DArray<float4> sky = ResourceDescriptorHeap[g_pass.input1];
    uint capturedSize, skySize, height, faces;
    captured.GetDimensions(capturedSize, height, faces);
    sky.GetDimensions(skySize, height, faces);
    uint band = index / kLanesPerBand;
    uint lane = index % kLanesPerBand;
    float lowest = sin(BandElevation(band - 0.5));
    float highest = sin(BandElevation(band + 0.5));
    float4 capturedSum = 0.0;
    float3 skySum = 0.0;
    [loop]
    for (uint row = lane; row < kHeights; row += kLanesPerBand) {
        float z = lerp(lowest, highest, (row + 0.5) / kHeights);
        float across = sqrt(saturate(1.0 - z * z));
        [loop]
        for (uint column = 0; column < kAzimuths; ++column) {
            float azimuth = (column + 0.5) / kAzimuths * 2.0 * 3.14159265;
            float3 direction = float3(across * cos(azimuth), across * sin(azimuth), z);
            float3 drawn = captured.Load(int4(CubeTexel(direction, capturedSize), 0)).rgb;
            if (all(drawn >= 0.0))
                capturedSum += float4(drawn, 1.0);
            skySum += sky.Load(int4(CubeTexel(direction, skySize), 0)).rgb;
        }
    }
    g_capturedSums[index] = capturedSum;
    g_skySums[index] = skySum;
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0) {
        float4 capturedBand = 0.0;
        float3 skyBand = 0.0;
        for (uint l = 0; l < kLanesPerBand; ++l) {
            capturedBand += g_capturedSums[index + l];
            skyBand += g_skySums[index + l];
        }
        g_captured[band] = capturedBand.w > 0.0 ? capturedBand.rgb / capturedBand.w : -1.0;
        g_sky[band] = skyBand * ((1.0 + asfloat(g_pass.input2)) / (kAzimuths * kHeights));
    }
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) {
        // The bands above the horizon weighted by their solid angles: the game's sky over the clear sky where the game
        // drew, or the map's sky light over the clear sky's mean.
        float captured = 0.0;
        float drawn = 0.0;
        float drawnWeights = 0.0;
        float all = 0.0;
        float weights = 0.0;
        for (uint b = kBands / 2; b < kBands; ++b) {
            float weight = sin(BandElevation(b + 0.5)) - sin(BandElevation(b - 0.5));
            float sky = Luminance(g_sky[b]) * weight;
            all += sky;
            weights += weight;
            if (g_captured[b].x >= 0.0) {
                captured += Luminance(g_captured[b]) * weight;
                drawn += sky;
                drawnWeights += weight;
            }
        }
        RWTexture2D<float4> bands = ResourceDescriptorHeap[g_pass.output0];
        g_scale = captured > 0.0 && drawn > 0.0 ? captured / drawn : asfloat(g_pass.input3) / max(all / weights, 1e-30);
        bands[uint2(kBands, 0)] = float4(drawnWeights > 0.0 ? captured / drawnWeights : -1.0, g_scale, 0.0, 1.0);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < kBands) {
        RWTexture2D<float4> bands = ResourceDescriptorHeap[g_pass.output0];
        float3 multiplier = BandMultiplier(index, g_scale);
        float skyLuminance = Luminance(g_sky[index]);
        bands[uint2(index, 0)] = float4(multiplier, skyLuminance > 0.0 ? Luminance(multiplier * g_sky[index]) / skyLuminance : Luminance(multiplier));
    }
}

float DaylightElevation(uint entry, uint entries)
{
    return ((entry + 0.5) / entries - 0.5) * 3.14159265;
}

void Daylight(uint entry, uint index)
{
    Texture2D<float4> transmittance = ResourceDescriptorHeap[g_pass.input0];
    Texture2D<float4> multipleScattering = ResourceDescriptorHeap[g_pass.input1];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    uint entries, height;
    output.GetDimensions(entries, height);
    float elevation = DaylightElevation(entry, entries);
    float3 sun = float3(cos(elevation), 0.0, sin(elevation));
    float3 origin = float3(0.0, 0.0, kGroundRadius + kEyeHeight);
    // The irradiance is the integral of radiance times the cosine z over dz and the azimuth.
    float irradiance = 0.0;
    [loop]
    for (uint k = index; k < kDaylightRings * kDaylightAzimuths; k += kBands * kLanesPerBand) {
        float z = (k / kDaylightAzimuths + 0.5) / kDaylightRings;
        float azimuth = (k % kDaylightAzimuths + 0.5) / kDaylightAzimuths * 2.0 * 3.14159265;
        float across = sqrt(1.0 - z * z);
        float3 direction = float3(across * cos(azimuth), across * sin(azimuth), z);
        irradiance += Luminance(Integrate(origin, direction, sun, kSkySteps, true, transmittance, multipleScattering).radiance) * z;
    }
    g_daylightSums[index] = irradiance;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = kBands * kLanesPerBand / 2; stride > 0; stride /= 2) {
        if (index < stride)
            g_daylightSums[index] += g_daylightSums[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        float total = g_daylightSums[0] * (2.0 * 3.14159265 / (kDaylightRings * kDaylightAzimuths));
        output[uint2(entry, 0)] = float4(AtmosphereTransmittance(transmittance, g_linearClamp, origin.z, sun.z), total);
    }
}

// A clear sky's light (step 2) in the game's colours by a band's multiplier: the aerosols' glow around its sun (alpha,
// its luminance) is sunlight scattered forward, which keeps the colour of the sunlight that makes it (the
// transmittance toward `sun` from the eye) and takes the band's brightness alone; the rest takes the band's colour.
// Coloured as a whole, the glow around a night map's Moon turned orange under a skybox greyer than the clear sky.
float3 Recoloured(float4 clear, float3 sun, float4 multiplier, Texture2D<float4> transmittance)
{
    float3 sunlight = AtmosphereTransmittance(transmittance, g_linearClamp, kGroundRadius + kEyeHeight, sun.z);
    float3 glow = min(clear.a * sunlight / max(Luminance(sunlight), 1e-12), clear.rgb);
    return (clear.rgb - glow) * multiplier.rgb + glow * multiplier.a;
}

// Steps 3 and 5 run as groups of kBands x kLanesPerBand threads.
[RootSignature(ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    if (g_pass.step == 0) {
        RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
        float r, mu;
        TransmittanceParameters((float2(id.xy) + 0.5) / float2(kTransmittanceSize), r, mu);
        float3 origin = float3(0.0, 0.0, r);
        float3 direction = float3(sqrt(saturate(1.0 - mu * mu)), 0.0, mu);
        float span = SphereDistance(origin, direction, kAtmosphereRadius);
        float3 depth = 0.0;
        [loop]
        for (uint i = 0; i < kTransmittanceSteps; ++i)
            depth += MediumAt(length(origin + direction * ((i + 0.5) / kTransmittanceSteps * span)) - kGroundRadius).extinction;
        output[id.xy] = float4(exp(-depth * (span / kTransmittanceSteps)), 1.0);
        return;
    }
    if (g_pass.step == 1) {
        RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
        Texture2D<float4> transmittance = ResourceDescriptorHeap[g_pass.input0];
        float sunMu = UnitRange((id.x + 0.5) / kMultipleScatteringSize, kMultipleScatteringSize) * 2.0 - 1.0;
        float height = UnitRange((id.y + 0.5) / kMultipleScatteringSize, kMultipleScatteringSize) * (kAtmosphereRadius - kGroundRadius);
        float3 origin = float3(0.0, 0.0, kGroundRadius + max(height, 1e-3));
        float3 sun = float3(sqrt(saturate(1.0 - sunMu * sunMu)), 0.0, sunMu);
        // Second-order light and f_ms over the sphere of directions with the isotropic phase (Hillaire 2020, eq. 6-9).
        const uint side = 8;
        float3 secondOrder = 0.0;
        float3 transfer = 0.0;
        [loop]
        for (uint i = 0; i < side * side; ++i) {
            float3 direction = UniformSphere((i / side + 0.5) / side, (i % side + 0.5) / side);
            Scattered scattered = Integrate(origin, direction, sun, kMultipleScatteringSteps, false, transmittance, transmittance);
            secondOrder += scattered.radiance;
            transfer += scattered.transfer;
        }
        // Each direction stands for 4 pi / side^2 steradians, times the isotropic phase: the means.
        secondOrder /= side * side;
        transfer /= side * side;
        output[id.xy] = float4(secondOrder / (1.0 - transfer), 1.0);
        return;
    }
    if (g_pass.step == 2) {
        RWTexture2DArray<float4> output = ResourceDescriptorHeap[g_pass.output0];
        Texture2D<float4> transmittance = ResourceDescriptorHeap[g_pass.input0];
        Texture2D<float4> multipleScattering = ResourceDescriptorHeap[g_pass.input1];
        uint width, height, faces;
        output.GetDimensions(width, height, faces);
        float3 direction = normalize(FaceDirection(id.z, (float2(id.xy) + 0.5) * (2.0 / width) - 1.0));
        float3 sun = float3(asfloat(g_pass.input2), asfloat(g_pass.input3), asfloat(g_pass.input4));
        float3 origin = float3(0.0, 0.0, kGroundRadius + kEyeHeight);
        Scattered scattered = Integrate(origin, direction, sun, kSkySteps, true, transmittance, multipleScattering);
        output[id] = float4(scattered.radiance, Luminance(scattered.mie));
        return;
    }
    if (g_pass.step == 3) {
        ColourBands(index);
        return;
    }
    if (g_pass.step == 5) {
        Daylight(group.x, index);
        return;
    }
    if (g_pass.step == 6) {
        RWTexture2DArray<float4> output = ResourceDescriptorHeap[g_pass.output0];
        uint width, height, faces;
        output.GetDimensions(width, height, faces);
        output[id] = float4(MilkyWayStarlight(normalize(FaceDirection(id.z, (float2(id.xy) + 0.5) * (2.0 / width) - 1.0))), 1.0);
        return;
    }
    RWTexture2DArray<float4> output = ResourceDescriptorHeap[g_pass.output0];
    Texture2DArray<float4> sky = ResourceDescriptorHeap[g_pass.input0];
    Texture2D<float4> bands = ResourceDescriptorHeap[g_pass.input1];
    Texture2DArray<float4> mapSky = ResourceDescriptorHeap[g_pass.input2];
    StructuredBuffer<NightSkyConstants> nightSky = ResourceDescriptorHeap[g_pass.input3];
    NightSkyConstants night = nightSky[0];
    uint width, height, faces;
    output.GetDimensions(width, height, faces);
    float3 direction = normalize(FaceDirection(id.z, (float2(id.xy) + 0.5) * (2.0 / width) - 1.0));
    // A uniform cubic B-spline through the bands: straight lines between their centres turned at each one, which showed
    // as stripes of colour.
    float band = (asin(direction.z) / 3.14159265 + 0.5) * kBands - 0.5;
    int first = int(floor(band)) - 1;
    float t = band - floor(band);
    float weights[4] = {pow(1.0 - t, 3.0) / 6.0, (3.0 * t * t * t - 6.0 * t * t + 4.0) / 6.0, (-3.0 * t * t * t + 3.0 * t * t + 3.0 * t + 1.0) / 6.0,
        t * t * t / 6.0};
    float4 multiplier = 0.0;
    [unroll]
    for (int k = 0; k < 4; ++k)
        multiplier += weights[k] * bands.Load(int3(clamp(first + k, 0, int(kBands) - 1), 0, 0));
    Texture2D<float4> transmittance = ResourceDescriptorHeap[night.transmittanceView];
    float4 clear = sky.Load(int4(id, 0));
    float3 radiance = Recoloured(clear, night.sunDirection, multiplier, transmittance) +
        night.moonlessFloor * Recoloured(mapSky.Load(int4(id, 0)), night.mapSunDirection, multiplier, transmittance);
    float3 moonlit = 0.0;
    if (night.moonIrradiance > 0.0) {
        Texture2DArray<float4> moonSky = ResourceDescriptorHeap[g_pass.input4];
        float4 moonClear = moonSky.Load(int4(id, 0));
        moonlit = moonClear.rgb;
        radiance += night.moonScale * Recoloured(moonClear, night.moonDirection, multiplier, transmittance);
    }
    radiance = min(radiance, 65504.0);
    float gain = Luminance(radiance) / (Luminance(clear.rgb) * night.mapLight * night.starTwilight + Luminance(moonlit) * night.moonIrradiance + kNightSkyRadiance);
    output[id] = float4(radiance, log2(max(gain, 1e-30)));
}
