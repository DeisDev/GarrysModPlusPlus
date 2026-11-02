#include "common.hlsli"
#include "atmosphere.hlsli"
#include "cube.hlsli"

// The procedural sky that replaces the game's 2D sky (shaders/atmosphere.hlsli), in steps:
// 0: the transmittance table (output0, kTransmittanceSize).
// 1: the multiple scattering table (output0, kMultipleScatteringSize square) from the transmittance (input0).
// 2: the clear sky around an eye kEyeHeight above the ground, lit by sunlight of irradiance 1 from the direction in
//    input2-4 (float bits), into a cube's faces (output0) from both tables (input0, input1).
// 3: the game's colours (one group): the mean radiance of the game's sky (input0, the faces of the capture's cube, -1
//    where nothing was drawn) and of step 2's sky (input1) in kBands bands of elevation, and from them each band's
//    multiplier (output0, kBands x 1). Above the horizon a band keeps its brightness and takes the game's colour, and
//    one scale makes the sky above the horizon as bright as the game's; below it, where the clear sky is only a ground
//    of guessed albedo, a band takes the game's colour and brightness. Without the game's sky above the horizon the scale
//    makes it as bright as the map's sky light.
// 4: the procedural sky (output0, a cube's faces): step 2's sky (input0) times the multiplier of its elevation (input1),
//    a smooth curve through the bands.

static const uint kBands = 32;
static const uint kLanesPerBand = 8;
// Each band is sampled at kAzimuths x kHeights directions spaced evenly in azimuth and in the sine of elevation, so each
// stands for the same solid angle; every lane takes kHeights / kLanesPerBand rows.
static const uint kAzimuths = 32;
static const uint kHeights = 16;

static const uint kTransmittanceSteps = 40;
static const uint kMultipleScatteringSteps = 20;
static const uint kSkySteps = 32;

static const float kIsotropicPhase = 1.0 / (4.0 * 3.14159265);

groupshared float4 g_capturedSums[kBands * kLanesPerBand];
groupshared float3 g_skySums[kBands * kLanesPerBand];
groupshared float3 g_captured[kBands];
groupshared float3 g_sky[kBands];
groupshared float g_scale;

struct Scattered
{
    float3 radiance;
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
        float3 inScattered = sunlight * (medium.rayleigh * rayleighPhase + medium.mie * miePhase);
        if (multiple)
            inScattered += scattering * multipleScattering.SampleLevel(g_linearClamp, MultipleScatteringUv(r, sunMu), 0.0).rgb;
        float3 extinction = max(medium.extinction, 1e-9);
        float3 stepTransmittance = exp(-extinction * step);
        result.radiance += throughput * (inScattered - inScattered * stepTransmittance) / extinction;
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
        g_sky[band] = skyBand / (kAzimuths * kHeights);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) {
        // The bands above the horizon weighted by their solid angles: the game's sky over the clear sky where the game
        // drew, or the map's sky light over the clear sky's mean.
        float captured = 0.0;
        float drawn = 0.0;
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
            }
        }
        g_scale = captured > 0.0 && drawn > 0.0 ? captured / drawn : Luminance(g_frame.skyColor) / max(all / weights, 1e-30);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < kBands) {
        RWTexture2D<float4> bands = ResourceDescriptorHeap[g_pass.output0];
        bands[uint2(index, 0)] = float4(BandMultiplier(index, g_scale), 1.0);
    }
}

// Step 3 runs as one group of kBands x kLanesPerBand threads.
[RootSignature(ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
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
        output[id] = float4(Integrate(origin, direction, sun, kSkySteps, true, transmittance, multipleScattering).radiance, 1.0);
        return;
    }
    if (g_pass.step == 3) {
        ColourBands(index);
        return;
    }
    RWTexture2DArray<float4> output = ResourceDescriptorHeap[g_pass.output0];
    Texture2DArray<float4> sky = ResourceDescriptorHeap[g_pass.input0];
    Texture2D<float4> bands = ResourceDescriptorHeap[g_pass.input1];
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
    float3 multiplier = 0.0;
    [unroll]
    for (int k = 0; k < 4; ++k)
        multiplier += weights[k] * bands.Load(int3(clamp(first + k, 0, int(kBands) - 1), 0, 0)).rgb;
    output[id] = float4(min(sky.Load(int4(id, 0)).rgb * multiplier, 65504.0), 1.0);
}
