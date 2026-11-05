#include "flare.hlsli"

// The lens flare (src/post_effects.cpp, src/lens_flare.cpp), as Hullin, Eisemann, Seidel and Lee's "Physically-Based
// Real-Time Lens Flare Rendering" (SIGGRAPH 2011) builds it, with Lee and Eisemann's paraxial ghosts ("Practical
// Real-Time Lens-Flare Rendering", EGSR 2013): each ghost is the aperture's image, drawn where the lens's matrices put
// it, clipped by the front element and tinted by its coatings. The starburst is the far field of the aperture's
// Fraunhofer diffraction, the part of it a camera shows: each blade's straight edge sends a spike out perpendicular
// to it on both sides, whose light along it falls with the square of the angle. Its finer pattern (the rings and the
// fringes along the spikes) is far smaller than a pixel and averages out over the pixel and the spectrum.

static const float kPi = 3.14159265;
static const uint kApertureSize = 256;
static const float kApertureRotation = 0.26;
// Blades bow outward between their corners by this share of the way to the circumscribed circle, and the polygon's
// corners lie at kApertureFill of the texture's half-size, so its 2-texel soft edge stays inside.
static const float kBladeCurvature = 0.15;
static const float kApertureFill = 0.97;
// A spike is narrower than a pixel; it is drawn as a Gaussian this many pixels across (sigma), which the half-size
// flare target still resolves.
static const float kSpikeWidth = 1.5;
// The spikes' light per colour relative to 550 nm: an edge diffracts in proportion to the wavelength, weighed over the
// visible spectrum by linear sRGB's colour matching (Wyman, Sloan and Shirley 2013's fits of the CIE 1931 functions).
static const float3 kSpikeTint = float3(1.093, 0.983, 0.822);

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

// A quad around ghost `instance` of flareGhosts, or for instance flareGhostCount around the spikes, in the view's
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

// The diffraction spikes at `offset` pixels from the light, per unit of its light: for each blade's edge a line through
// the light along the edge's normal (ApertureMain's polygon), its light along it flareStarburst / d^2 at d pixels from
// the light, spread across it by a Gaussian of unit area. They start at the light's disc and fade out by
// flareStarburstSize.
float3 Spikes(float2 offset)
{
    float distance = length(offset);
    float reach = g_post.flareStarburstSize;
    if (distance >= reach)
        return 0.0;
    float sector = 2.0 * kPi / float(max(g_post.flareBlades, 3u));
    float across = 0.0;
    [loop]
    for (uint k = 0; k < max(g_post.flareBlades, 3u); ++k) {
        float angle = kApertureRotation + (float(k) + 0.5) * sector;
        float side = dot(offset, float2(-sin(angle), cos(angle)));
        across += exp(-0.5 * side * side / (kSpikeWidth * kSpikeWidth));
    }
    across /= kSpikeWidth * 2.5066283;
    float near = max(distance, max(g_post.flareRadius, 2.0 * kSpikeWidth));
    float fade = 1.0 - (distance / reach) * (distance / reach);
    return kSpikeTint * (g_post.flareStarburst * across * fade * fade / (near * near));
}

// Each ghost: the aperture's image per colour, within the front element's circle (its edge a little soft, as the ghost
// is out of focus), times its tint and the light; the starburst: the spikes times the light.
[RootSignature(POST_ROOT_SIGNATURE)]
float4 GhostPS(GhostVertex input) : SV_Target
{
    float3 light = FlareLight();
    if (input.instance >= g_post.flareGhostCount)
        return float4(Spikes(input.pixel - g_post.flarePosition) * light, 0.0);
    StructuredBuffer<FlareGhost> ghosts = ResourceDescriptorHeap[g_post.flareGhosts];
    FlareGhost ghost = ghosts[input.instance];
    Texture2D<float4> aperture = ResourceDescriptorHeap[g_post.flareAperture];
    float3 shape = float3(GhostChannel(aperture, input.pixel, ghost.centreRed, ghost.radius.x), GhostChannel(aperture, input.pixel, ghost.centreGreen, ghost.radius.y),
        GhostChannel(aperture, input.pixel, ghost.centreBlue, ghost.radius.z));
    float edge = max(0.015 * ghost.circleRadius, 1.0);
    float entrance = saturate((ghost.circleRadius - length(input.pixel - ghost.circleCentre)) / edge + 0.5);
    return float4(shape * entrance * ghost.tint * light, 0.0);
}
