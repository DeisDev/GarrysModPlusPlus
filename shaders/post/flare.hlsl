#include "flare.hlsli"

// The lens flare (src/post_effects.cpp, src/lens_flare.cpp), as Hullin, Eisemann, Seidel and Lee's "Physically-Based
// Real-Time Lens Flare Rendering" (SIGGRAPH 2011) builds it, with Lee and Eisemann's paraxial ghosts ("Practical
// Real-Time Lens-Flare Rendering", EGSR 2013): each ghost is the aperture's image, drawn where the lens's matrices put
// it, clipped by the front element and tinted by its coatings; the starburst is the aperture's Fraunhofer diffraction,
// the squared magnitude of its Fourier transform, summed over wavelengths.

static const float kPi = 3.14159265;
static const uint kApertureSize = 256;
static const float kApertureRotation = 0.26;
// Blades bow outward between their corners by this share of the way to the circumscribed circle, and the polygon's
// corners lie at kApertureFill of the texture's half-size, so its 2-texel soft edge stays inside.
static const float kBladeCurvature = 0.15;
static const float kApertureFill = 0.97;
// For the diffraction pattern, the aperture's circumscribed circle is kDiffractionRadius texels of the transform's 256.
static const float kDiffractionRadius = 32.0;
static const uint kWavelengthSamples = 8;

// output0: the aperture (R8, 256 square): a polygon of `step` blades whose corners lie on the circle of radius
// kApertureFill around the centre.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void ApertureMain(uint2 texel : SV_DispatchThreadID)
{
    RWTexture2D<float4> aperture = ResourceDescriptorHeap[g_postPass.output0];
    float2 q = (float2(texel) + 0.5) / float(kApertureSize) * 2.0 - 1.0;
    float radius = length(q);
    float sector = 2.0 * kPi / float(max(g_postPass.step, 3u));
    float turn = (atan2(q.y, q.x) - kApertureRotation) / sector;
    float fromMiddle = (frac(turn) - 0.5) * sector;
    float polygon = cos(0.5 * sector) / cos(fromMiddle);
    float edge = lerp(polygon, 1.0, kBladeCurvature) * kApertureFill;
    aperture[texel] = saturate((edge - radius) * 0.25 * float(kApertureSize)).xxxx;
}

groupshared float2 g_fft[kApertureSize];
groupshared float g_energy[kApertureSize / 2];

float2 ComplexMultiply(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// A 256-point discrete Fourier transform of g_fft, loaded in bit-reversed order, by radix-2 butterflies; 128 threads.
void Fft(uint thread)
{
    [unroll]
    for (uint stage = 0; stage < 8; ++stage) {
        uint span = 1u << stage;
        uint k = thread & (span - 1);
        uint i0 = ((thread >> stage) << (stage + 1)) + k;
        uint i1 = i0 + span;
        float angle = -kPi * float(k) / float(span);
        float2 a = g_fft[i0];
        float2 b = ComplexMultiply(g_fft[i1], float2(cos(angle), sin(angle)));
        g_fft[i0] = a + b;
        g_fft[i1] = a - b;
        GroupMemoryBarrierWithGroupSync();
    }
}

uint BitReversed(uint index)
{
    return reversebits(index) >> 24;
}

// input0: the aperture; output0: each row's transform (RG32F, 256 square) of the aperture scaled so its circumscribed
// circle is kDiffractionRadius texels across the middle, times (-1)^(x+y) so the transform's zero frequency lands in the
// middle; output1: per row, the sum of the squared values (step: none). One group per row.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(128, 1, 1)]
void FftRowsMain(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    Texture2D<float4> aperture = ResourceDescriptorHeap[g_postPass.input0];
    uint row = group.x;
    float energy = 0.0;
    [unroll]
    for (uint i = 0; i < 2; ++i) {
        uint x = thread + i * 128;
        float2 q = (float2(x, row) + 0.5 - 0.5 * float(kApertureSize)) / kDiffractionRadius * kApertureFill;
        float value = all(abs(q) < 1.0) ? aperture.SampleLevel(g_postLinear, q * 0.5 + 0.5, 0).r : 0.0;
        energy += value * value;
        g_fft[BitReversed(x)] = float2(((x + row) & 1) != 0 ? -value : value, 0.0);
    }
    g_energy[thread] = energy;
    GroupMemoryBarrierWithGroupSync();
    Fft(thread);
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    output[uint2(thread, row)] = float4(g_fft[thread], 0.0, 0.0);
    output[uint2(thread + 128, row)] = float4(g_fft[thread + 128], 0.0, 0.0);
    if (thread == 0) {
        float sum = 0.0;
        for (uint t = 0; t < kApertureSize / 2; ++t)
            sum += g_energy[t];
        RWStructuredBuffer<float> energies = ResourceDescriptorHeap[g_postPass.output1];
        energies[row] = sum;
    }
}

// input0: the rows' transforms; input1: the rows' energies; output0: the power spectrum (R32F, 256 square), the squared
// magnitude of each column's transform; output1: the energies, whose element 256 gets their sum. One group per column.
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(128, 1, 1)]
void FftColumnsMain(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    Texture2D<float4> rows = ResourceDescriptorHeap[g_postPass.input0];
    uint column = group.x;
    [unroll]
    for (uint i = 0; i < 2; ++i) {
        uint y = thread + i * 128;
        g_fft[BitReversed(y)] = rows.Load(int3(column, y, 0)).xy;
    }
    GroupMemoryBarrierWithGroupSync();
    Fft(thread);
    RWTexture2D<float4> power = ResourceDescriptorHeap[g_postPass.output0];
    power[uint2(column, thread)] = dot(g_fft[thread], g_fft[thread]).xxxx;
    power[uint2(column, thread + 128)] = dot(g_fft[thread + 128], g_fft[thread + 128]).xxxx;
    if (column == 0 && thread == 0) {
        RWStructuredBuffer<float> energies = ResourceDescriptorHeap[g_postPass.output1];
        float sum = 0.0;
        for (uint r = 0; r < kApertureSize; ++r)
            sum += energies[r];
        energies[kApertureSize] = sum;
    }
}

// The CIE 1931 colour matching functions by Wyman, Sloan and Shirley's multi-lobe fits ("Simple Analytic Approximations
// to the CIE XYZ Color Matching Functions", JCGT 2013), in linear sRGB, negative parts left out.
float FitLobe(float wavelength, float centre, float below, float above)
{
    float t = (wavelength - centre) / (wavelength < centre ? below : above);
    return exp(-0.5 * t * t);
}

float3 WavelengthRgb(float wavelength)
{
    float x = 1.056 * FitLobe(wavelength, 599.8, 37.9, 31.0) + 0.362 * FitLobe(wavelength, 442.0, 16.0, 26.7) - 0.065 * FitLobe(wavelength, 501.1, 20.4, 26.2);
    float y = 0.821 * FitLobe(wavelength, 568.8, 46.9, 40.5) + 0.286 * FitLobe(wavelength, 530.9, 16.3, 31.1);
    float z = 1.217 * FitLobe(wavelength, 437.0, 11.8, 36.0) + 0.681 * FitLobe(wavelength, 459.0, 26.0, 13.8);
    float3 rgb = float3(3.2406 * x - 1.5372 * y - 0.4986 * z, -0.9689 * x + 1.8758 * y + 0.0415 * z, 0.0557 * x - 0.2040 * y + 1.0570 * z);
    return max(rgb, 0.0);
}

// input0: the power spectrum at 550 nm; input1: the energies; output0: the starburst (RGBA16F, 256 square): the power
// spectrum at kWavelengthSamples wavelengths from 400 to 700 nm, each scaled about the middle by wavelength / 550 (a
// longer wavelength diffracts wider) and weighted by its colour, each channel summing to 65536 over the texture (by
// Parseval's theorem the spectrum sums to 65536 x the aperture's energy).
[RootSignature(POST_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void StarburstMain(uint2 texel : SV_DispatchThreadID)
{
    Texture2D<float4> power = ResourceDescriptorHeap[g_postPass.input0];
    RWStructuredBuffer<float> energies = ResourceDescriptorHeap[g_postPass.input1];
    float energy = max(energies[kApertureSize], 1e-6);
    float2 fromMiddle = float2(texel) + 0.5 - 0.5 * float(kApertureSize);
    float3 sum = 0.0;
    float3 weights = 0.0;
    [loop]
    for (uint i = 0; i < kWavelengthSamples; ++i) {
        float wavelength = 400.0 + 300.0 * (float(i) + 0.5) / float(kWavelengthSamples);
        float3 weight = WavelengthRgb(wavelength);
        float shrink = 550.0 / wavelength;
        float2 uv = (0.5 * float(kApertureSize) + fromMiddle * shrink) / float(kApertureSize);
        float value = all(uv >= 0.0) && all(uv <= 1.0) ? power.SampleLevel(g_postLinear, uv, 0).r : 0.0;
        sum += weight * (value * shrink * shrink);
        weights += weight;
    }
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_postPass.output0];
    output[texel] = float4(sum / (max(weights, 1e-6) * energy), 1.0);
}

// Mirrors lens_flare::Ghost in src/lens_flare.h.
struct FlareGhost
{
    float2 centreRed;
    float2 centreGreen;
    float2 centreBlue;
    float3 radius;
    float3 tint;
    float2 circleCentre;
    float circleRadius;
    float padding;
};

struct GhostVertex
{
    float4 position : SV_Position;
    float2 pixel : PIXEL;
    nointerpolation uint instance : INSTANCE;
};

// A quad around ghost `instance` of flareGhosts, or for instance flareGhostCount around the starburst, in the view's
// pixels; drawn into the half-size flare target.
[RootSignature(POST_ROOT_SIGNATURE)]
GhostVertex GhostVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    float2 centre = g_post.flarePosition;
    float extent = g_post.flareStarburstSize;
    if (instance < g_post.flareGhostCount) {
        StructuredBuffer<FlareGhost> ghosts = ResourceDescriptorHeap[g_post.flareGhosts];
        FlareGhost ghost = ghosts[instance];
        centre = ghost.centreGreen;
        float apart = max(length(ghost.centreRed - centre), length(ghost.centreBlue - centre));
        extent = max(max(abs(ghost.radius.x), abs(ghost.radius.y)), abs(ghost.radius.z)) + apart + 1.0;
    }
    float2 corner = float2((vertex == 1 || vertex == 4 || vertex == 5) ? 1.0 : -1.0, (vertex == 2 || vertex == 3 || vertex == 5) ? 1.0 : -1.0);
    GhostVertex output;
    output.pixel = centre + extent * corner;
    output.position = float4(output.pixel * g_post.invSize * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.instance = instance;
    return output;
}

float GhostChannel(Texture2D<float4> aperture, float2 pixel, float2 centre, float radius)
{
    float2 q = (pixel - centre) / radius;
    return all(abs(q) < 1.0) ? aperture.SampleLevel(g_postLinear, q * 0.5 + 0.5, 0).r : 0.0;
}

// Each ghost: the aperture's image per colour, within the front element's circle, times its tint and the light; the
// starburst: its image times flareStarburst (the light's power in the view spread over the texture) and the light.
[RootSignature(POST_ROOT_SIGNATURE)]
float4 GhostPS(GhostVertex input) : SV_Target
{
    float3 light = FlareLight();
    if (input.instance >= g_post.flareGhostCount) {
        Texture2D<float4> starburst = ResourceDescriptorHeap[g_post.flareStarburstView];
        float2 uv = (input.pixel - g_post.flarePosition) / (2.0 * g_post.flareStarburstSize) + 0.5;
        return float4(starburst.SampleLevel(g_postLinear, uv, 0).rgb * g_post.flareStarburst * light, 0.0);
    }
    StructuredBuffer<FlareGhost> ghosts = ResourceDescriptorHeap[g_post.flareGhosts];
    FlareGhost ghost = ghosts[input.instance];
    Texture2D<float4> aperture = ResourceDescriptorHeap[g_post.flareAperture];
    float3 shape = float3(GhostChannel(aperture, input.pixel, ghost.centreRed, ghost.radius.x), GhostChannel(aperture, input.pixel, ghost.centreGreen, ghost.radius.y),
        GhostChannel(aperture, input.pixel, ghost.centreBlue, ghost.radius.z));
    float entrance = saturate(ghost.circleRadius - length(input.pixel - ghost.circleCentre));
    return float4(shape * entrance * ghost.tint * light, 0.0);
}
