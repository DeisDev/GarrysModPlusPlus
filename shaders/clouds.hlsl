#include "common.hlsli"
#include "atmosphere.hlsli"
#include "cube.hlsli"

// Volumetric clouds over the procedural sky (shaders/atmosphere.hlsli: lengths in kilometres, the planet's centre the
// origin, +z Source's up), after Schneider ("The Real-time Volumetric Cloudscapes of Horizon Zero Dawn", SIGGRAPH 2015;
// with Vos, "Nubis: Authoring Real-Time Volumetric Cloudscapes with the Decima Engine", SIGGRAPH 2017) and Hillaire
// ("Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite", SIGGRAPH 2016, section 5). Two layers of water
// cloud lie in shells around the planet, shaped after the look of the Photon shader pack's clouds (studied, not copied):
// a billowy two-dimensional noise cut at the coverage gives each place how strongly cloud forms there, which height
// carves away, so stronger places rise higher (domes with billowy outlines over flat bases), and two cellular noises
// erode them, sparing their dense cores. A sheet of cirrus lies above them. Water clouds scatter all the light they take out (single scattering albedo 1, Hillaire
// 2016 after Hess et al. 1998); their phase function is Jendersie and d'Eon's ("An Approximate Mie Scattering Function
// for Fog and Cloud Rendering", 2023) fit of Mie scattering by droplets of the set diameter; their multiple scattering is
// Wrenninge et al.'s octaves (Hillaire 2016, eq. 19-20); each step's light is integrated against its extinction (eq. 17);
// and the air between the eye and the clouds takes its share (section 5.9.1). Radiance is in the procedural sky's units
// (before the tonemap scale), so the clouds sit in the sky they are drawn over.
//
// input0: the slot's CloudConstants. Steps:
// 0: the shape noise (output0, kShapeSize cubed): Perlin-Worley noise, then Worley noise of three octaves at three
//    frequencies.
// 1: the detail noise (output0, kDetailSize cubed): Worley noise of three octaves at three frequencies.
// 2: the weather (output0, kWeatherSize square): coverage by region, cloud type, and the cirrus' streaks.
// 3: the cloud shadow map (output0, two square cascades side by side; FrameConstants::cloudShadowU and on): the sun's
//    transmittance through the clouds along the line through each texel; with input1 = 1 or 2 only the even or odd rows,
//    0 all of them.
// 4: the clouds' light (output0, kLightTexels x 1, one group) from the procedural sky (CloudConstants::skyView), last
//    frame's cloud cube (input1, the faces, or kNoView), the shadow map (input2, or kNoView) and the colour bands.
// 5: the cloud cube (output0, its faces) with the light (input1): the texels of each 2 x 2 block at traceOffset, blended
//    into what they held, or with input2 = 1 every texel anew.
// 6: the main view's trace with the light (input1) and the 3D skybox's distances (input2, or kNoView): one texel of each
//    traceBlock-square block of the history (CloudConstants::historySize) at traceOffset, the clouds seen (output0: their light with
//    the air's in front, rgb, and transmittance, a) and their depth (output1, kilometres; kNoCloud for none, kMasked where
//    the screen shows no sky around it).
// 7: the history (output0) from last frame's (input1), moved with the camera and the wind, and the trace (input2, input3):
//    traced texels blend their new light in, the others keep last frame's within the light traced around them; where the
//    screen shows no sky around them, the cube's clouds (FrameConstants::cloudCubeView).
// 8: the cirrus' strands (output0, kCirrusSize square, mip 0): veils along x, and fibres within them.
// 9: a mip of the strands (output0) as the mean of the 2 x 2 texels of the one above (input1), both unordered access views.
// 10: the clouds' shapes (output0, square, over each layer's weather tile; Shapes), unordered access.
// 11: their distribution (one group): each channel's cumulative histogram over every kHistogramStride-th texel of input1
//    (the shapes) into output0 (kHistogramBins x 4).
// 12: the shapes (output0) equalised by their distribution (input1), so each channel is spread evenly over [0, 1] and
//    the share of the sky a coverage cuts out of it is that coverage.
// Clouds seen and the cube hold (light, transmittance), over the sky: the sky behind times the transmittance plus the light.

struct CloudLayer
{
    // Its bottom and top as radii from the planet's centre.
    float bottom;
    float top;
    float coverage;
    // 0 stratus, 0.5 stratocumulus, 1 cumulus.
    float type;
    // Per kilometre where the cloud is densest.
    float extinction;
    // Cycles per kilometre of the shape and detail noise and of the weather; the first two are whole multiples of the
    // last, so the wind's offset wraps with the weather.
    float shapeFrequency;
    float detailFrequency;
    float weatherFrequency;
    float erosion;
    // The wind's offset in weather tiles (xy) and how far the shapes have evolved in shape tiles (z), both wrapped.
    float3 offset;
    // How far the billows' (xy, in shape tiles) and the fine detail's (zw, in detail tiles) noise has drifted, ahead of
    // the wind, so the clouds' shapes change as they go.
    float4 erosionOffset;
    // March steps for a ray straight up (twice as many for the longest rays, toward the horizon), and which layer it is
    // (0 the low one, 1 the middle one).
    uint steps;
    uint field;
    float2 padding;
};

// Mirrors CloudConstants in src/clouds.h.
struct CloudConstants
{
    // The low (cumulus) and middle (altocumulus) layers.
    CloudLayer layers[2];
    // The cirrus: its radius, coverage, optical depth straight through where thickest and cycles per kilometre, the
    // wind's offset in weather tiles, and the wind's direction, which its streaks lie along.
    float cirrusRadius;
    float cirrusCoverage;
    float cirrusDepth;
    float cirrusFrequency;
    float2 cirrusOffset;
    float2 cirrusAxis;
    // What is drawn (kLayer...), and the light (the Sun, or the Moon) toward lightDirection: its irradiance in the sky's
    // units over the sky's scale (the Moon's share of the night's sky; 0 for none), and its colour (luminance 1).
    uint layerMask;
    float3 lightDirection;
    float lightScale;
    float3 lightTint;
    // Jendersie and d'Eon's phase function: g of the Henyey-Greenstein peak, g and alpha of Draine's, and Draine's weight.
    float4 phase;
    // Multiple scattering: each octave's scattering and extinction and its phase function's anisotropy, times the last's.
    float octaveScatter;
    float octaveEccentricity;
    uint octaves;
    uint lightSteps;
    uint shapeNoiseView;
    uint detailNoiseView;
    uint weatherView;
    uint transmittanceView;
    // The procedural sky (a cube), the colour bands (shaders/sky_atmosphere.hlsl) and the shadow map's lerp to its
    // transmittance (1 for the clouds' own).
    uint skyView;
    uint bandsView;
    float shadowStrength;
    uint cubeTexels;
    // The history's rows from a direction to its clip space (the view's, widened by the overscan), this frame's and the
    // last's, and this frame's back from its clip space (x, y, w = 1) to a direction; the history's size, the overscan
    // (its share of the screen added on each side), whether last frame's history holds anything, the texel of each 2 x 2
    // block traced this frame, and where the clouds moved since then in the map's units.
    float4 viewProj[4];
    float4 prevViewProj[4];
    float4 clipToDirection[3];
    uint2 historySize;
    float overscan;
    uint historyValid;
    uint2 traceOffset;
    float2 windStep;
    // The cirrus' strands (step 8, with mips) and their cycles per kilometre, a whole multiple of cirrusFrequency so the
    // wind's offset wraps with it.
    uint cirrusView;
    float cirrusStrandFrequency;
    // The clouds' shapes (steps 10 to 12), and the side of the history's blocks a texel of which is traced each frame.
    uint shapesView;
    uint traceBlock;
};

static const uint kLayerLow = 1;
static const uint kLayerMiddle = 2;
static const uint kLayerCirrus = 4;

static const uint kShapeSize = 128;
static const uint kDetailSize = 64;
static const uint kWeatherSize = 512;
static const uint kCirrusSize = 1024;
// How far the flow bends the cirrus, in tiles per unit of flow: some 300 m across the strands' 10 km tile, a gentle bend
// along a veil's few kilometres, and 900 m across the weather's 30 km streaks (our choice).
static const float kCirrusWarp = 0.03;
// How far above the coverage's cut the cirrus' field reaches full depth (our choice).
static const float kCirrusEdge = 0.25;

// The clouds' light (step 4): the light's irradiance above the air in the sky's units (rgb) and the sky's scale (a); the
// clear sky's mean radiance over the upper hemisphere (rgb) and the cloudy sky's irradiance on a surface facing up over
// the clear sky's (a); the ground's radiance under the eye (rgb) and the cloud shadows' mean (a).
static const uint kLightTexels = 3;
static const uint kLightIrradiance = 0;
static const uint kLightSky = 1;
static const uint kLightGround = 2;

// The colour bands of shaders/sky_atmosphere.hlsl; their last texel's y is the scale that makes the clear sky as bright
// as the game's.
static const uint kSkyBands = 32;

// Source's units are inches (Valve Developer Community, "Dimensions"): kilometres per unit.
static const float kKilometresPerUnit = 2.54e-5;

// Depth markers of the trace (step 6).
static const float kNoCloud = -1.0;
static const float kMasked = -2.0;

// Our choices: beyond this distance the detail noise is left out (it is far smaller than a pixel there); light is
// marched toward the light at most this far, its first step this long (a cumulus at full density lets light in only
// some 12 m, so the steps must resolve its sunlit skin and its creases); the cloud above a point is sampled this far up
// for the sky's light; a ray stops once this little light gets through; ice crystals scatter with this anisotropy; the
// cube's traced texels and the history's take in this share of a new trace; light is scattered more than once only as
// far as the medium is optically thick over this many kilometres.
static const float kDetailDistance = 30.0;
static const float kMultipleScatteringLength = 0.05;
static const float kLightDistance = 3.0;
static const float kFirstLightStep = 0.01;
static const float kSkyLightDistance = 0.5;
static const float kOpaque = 0.005;
static const float kIceAsymmetry = 0.75;
// The march through cloud (MarchLayer): steps of this optical depth, at least this long (km), back to the stride after
// this many empty fine steps (more than the eight that cover the stride stepped back over), and at most this many
// samples more than the stride's.
static const float kStepDepth = 0.5;
static const float kMinStep = 0.003;
// Behind this much of the view's transmittance a sample shows at most that share of the pixel, and its light is marched in
// half the steps without the fine detail (our choice: some 60% of a dense cloud's samples lie behind it).
static const float kCheapLight = 0.1;
static const uint kEmptyRun = 10;
static const uint kFineSamples = 64;
static const float kCubeBlend = 0.5;
static const float kHistoryBlend = 0.25;
// The longest rays, toward the horizon, take this many times a layer's steps.
static const float kHorizonSteps = 2.0;
// The clouds' shapes (LayerDensity), our choices after Photon's: how far the weather's regional coverage moves a layer's
// coverage up or down (the middle layer's more, so altocumulus come in patches) and its type field the low layer's type;
// how strongly cloud forms per standard deviation of the billows above the coverage's edge (Strength); the share of the thickness where height reaches 1 (only the strongest cores rise above it);
// the width of stratus' edges in the noise; how deeply the middle layer's cells are cut; and the erosion's weights for
// cumulus and stratus (billows, fine detail) at the default Detail (Photon's, for its noise, times 1 / 0.7: ours erodes
// less at the same weight), and the middle layer's fine detail.
static const float kLowRegions = 0.3;
static const float kMiddleRegions = 0.8;
static const float kTypeRegions = 0.4;
static const float kStrength = 0.25;
static const float kTopShare = 0.8;
static const float kStratusEdge = 0.1;
static const float kCellCarve = 1.2;
// How many times finer than the middle layer's billows its cells are read: about 300 m at Size 1, some 2.5 degrees seen
// from 7 km, where altocumulus' cloudlets are 1 to 5 degrees wide (WMO International Cloud Atlas). Read at twice, they
// were twice that and clumped into cumulus-like masses (2026-11-03).
static const float kCellTiles = 4.0;
static const float2 kCumulusErosion = float2(0.47, 0.57);
static const float2 kStratusErosion = float2(0.1, 0.14);
static const float kMiddleErosion = 0.43;
// The shapes' distribution (steps 11 and 12): bins, and every how many texels each way it counts.
static const uint kHistogramBins = 256;
static const uint kHistogramStride = 4;

CloudConstants LoadClouds()
{
    StructuredBuffer<CloudConstants> clouds = ResourceDescriptorHeap[g_pass.input0];
    return clouds[0];
}

float Remap(float value, float low, float high, float newLow, float newHigh)
{
    return newLow + (value - low) / (high - low) * (newHigh - newLow);
}

// A point of the map in the clouds' space: the map's origin stands kEyeHeight above the ground, as the procedural sky's
// eye does.
float3 CloudSpace(float3 world)
{
    return float3(world.xy * kKilometresPerUnit, kGroundRadius + kEyeHeight + world.z * kKilometresPerUnit);
}

// The main view's eye in the clouds' space, kept above the ground: a map may reach below z = -7874 units, which would put
// it inside the planet.
float3 CloudEye()
{
    float3 eye = CloudSpace(g_frame.eyePosition);
    eye.z = max(eye.z, kGroundRadius + 0.002);
    return eye;
}

// Both crossings of a line with the sphere of `radius` around the planet's centre, nearest first.
bool SphereRoots(float3 origin, float3 direction, float radius, out float nearest, out float farthest)
{
    float b = dot(origin, direction);
    float discriminant = b * b - (dot(origin, origin) - radius * radius);
    nearest = 0.0;
    farthest = 0.0;
    if (discriminant < 0.0)
        return false;
    float root = sqrt(discriminant);
    nearest = -b - root;
    farthest = -b + root;
    return true;
}

// Where a ray is inside the shell between the radii `bottom` and `top`, the first time: none when it meets the ground or
// leaves the shell behind first.
bool ShellSegment(float3 origin, float3 direction, float bottom, float top, out float start, out float end)
{
    start = 0.0;
    end = 0.0;
    float nearTop, farTop;
    if (!SphereRoots(origin, direction, top, nearTop, farTop) || farTop <= 0.0)
        return false;
    float nearBottom, farBottom;
    bool crossesBottom = SphereRoots(origin, direction, bottom, nearBottom, farBottom);
    float r = length(origin);
    if (r < bottom) {
        float nearGround, farGround;
        if (SphereRoots(origin, direction, kGroundRadius, nearGround, farGround) && nearGround > 1e-3)
            return false;
        start = farBottom;
        end = farTop;
    } else {
        start = r <= top ? 0.0 : nearTop;
        end = crossesBottom && nearBottom > start ? nearBottom : farTop;
    }
    return end > start;
}

// ---- Noise (steps 0 to 2): tiling lattices, whose cells wrap at `period` cells per tile.

uint LatticeHash(int3 cell, int3 period, uint seed)
{
    uint3 wrapped = uint3((cell % period + period) % period);
    return Hash(wrapped.x + Hash(wrapped.y + Hash(wrapped.z + seed)));
}

float3 UnitHash3(uint hash)
{
    return float3(hash & 0x3FFu, (hash >> 10) & 0x3FFu, (hash >> 20) & 0x3FFu) / 1024.0;
}

static const float3 kGradients[12] = {float3(1, 1, 0), float3(-1, 1, 0), float3(1, -1, 0), float3(-1, -1, 0), float3(1, 0, 1), float3(-1, 0, 1),
    float3(1, 0, -1), float3(-1, 0, -1), float3(0, 1, 1), float3(0, -1, 1), float3(0, 1, -1), float3(0, -1, -1)};

// Perlin's improved gradient noise (2002), in about -1 to 1, at `p` in cells.
float GradientNoise(float3 p, int3 period, uint seed)
{
    int3 cell = int3(floor(p));
    float3 f = p - floor(p);
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float corners[8];
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        int3 corner = int3(i & 1, (i >> 1) & 1, i >> 2);
        corners[i] = dot(kGradients[LatticeHash(cell + corner, period, seed) % 12], f - float3(corner));
    }
    float4 x = lerp(float4(corners[0], corners[2], corners[4], corners[6]), float4(corners[1], corners[3], corners[5], corners[7]), u.x);
    float2 y = lerp(x.xz, x.yw, u.y);
    return lerp(y.x, y.y, u.z);
}

// Octaves of gradient noise over a tile at `p` in [0, 1), the first `period` cells across, in about 0 to 1.
float GradientFbm(float3 p, int3 period, uint octaves, uint seed)
{
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (uint octave = 0; octave < octaves; ++octave) {
        int3 cells = period << octave;
        sum += amplitude * GradientNoise(p * float3(cells), cells, seed + octave);
        total += amplitude;
        amplitude *= 0.5;
    }
    return saturate(sum / total + 0.5);
}

// Worley's cellular noise over a tile at `p` in [0, 1), `cells` cells across: 1 at a cell's point, 0 a cell away.
float CellNoise(float3 p, int cells, uint seed)
{
    float3 q = p * cells;
    int3 cell = int3(floor(q));
    float nearest = 1.0;
    [unroll]
    for (int z = -1; z <= 1; ++z) {
        [unroll]
        for (int y = -1; y <= 1; ++y) {
            [unroll]
            for (int x = -1; x <= 1; ++x) {
                int3 neighbour = cell + int3(x, y, z);
                float3 offset = float3(neighbour) + UnitHash3(LatticeHash(neighbour, cells, seed)) - q;
                nearest = min(nearest, dot(offset, offset));
            }
        }
    }
    return 1.0 - sqrt(nearest);
}

// Three octaves of cellular noise from `cells` cells across (Schneider 2015).
float CellFbm(float3 p, int cells, uint seed)
{
    return CellNoise(p, cells, seed) * 0.625 + CellNoise(p, cells * 2, seed + 1) * 0.25 + CellNoise(p, cells * 4, seed + 2) * 0.125;
}

// Thin lines along the zero crossings of gradient noise `cells` cells across a tile, at `p` in tiles: 1 on a line, 0 where
// the noise is a quarter of its range off it.
float Ridges(float2 p, int2 cells, uint seed)
{
    float ridge = saturate(1.0 - abs(GradientNoise(float3(p * float2(cells), 0.5), int3(cells, 1), seed)) * 4.0);
    return ridge * ridge;
}

// A slow divergence-free flow over a tile at `uv`: the curl of one octave of gradient noise `cells` cells across, by
// central differences, about one unit per cell. The curl weighs each octave of noise by its frequency: the curl of three
// octaves bent the cirrus' veils every 2.5 km, like noodles, and flow cells half the tile across 400 m made swirls like
// marble (2026-11-03).
float2 CurlFlow(float2 uv, int cells, uint seed)
{
    const float step = 0.5 / kCirrusSize;
    int3 period = int3(cells, cells, 1);
    float right = GradientNoise(float3((uv + float2(step, 0.0)) * float(cells), 0.5), period, seed);
    float left = GradientNoise(float3((uv - float2(step, 0.0)) * float(cells), 0.5), period, seed);
    float up = GradientNoise(float3((uv + float2(0.0, step)) * float(cells), 0.5), period, seed);
    float down = GradientNoise(float3((uv - float2(0.0, step)) * float(cells), 0.5), period, seed);
    return float2(up - down, left - right) / (2.0 * step * float(cells));
}

// The cirrus' strands over a tile at `uv` in [0, 1), along the lines of a slow divergence-free flow (the curl of noise,
// as the Photon shader pack's cirrus streaks do), so they curve and fan out: noise for veils a few kilometres long and
// about a kilometre wide (r, spread over the range as the weather's is) and fibres some 100 m apart within them (g)
// ("white, delicate filaments ... a fibrous (hair-like) appearance, or a silky sheen", WMO International Cloud Atlas).
// Fibres alone, in patches about as long as wide, looked like brush strokes, and filaments alone a few hundred metres
// wide like noodles (2026-11-03).
float2 CirrusStrands(float2 uv)
{
    float2 bent = uv + kCirrusWarp * CurlFlow(uv, 2, 0x1b873593u);
    float veils = saturate(GradientFbm(float3(bent, 0.5), int3(3, 12, 1), 4, 0x85ebca77u) * 2.0 - 0.5);
    return float2(veils, Ridges(bent, int2(3, 96), 0xc2b2ae3du));
}

// ---- The clouds' shapes (steps 10 to 12).

// Billowy noise over a tile at `uv`: gradient noise grown into the cells of cellular noise (Schneider 2015's
// Perlin-Worley, in two dimensions), `cells` cells across at its coarsest.
float Billows(float2 uv, int cells, uint seed)
{
    float3 p = float3(uv, 0.5);
    float worley = CellNoise(p, cells, seed) * 0.625 + CellNoise(p, cells * 2, seed + 1u) * 0.25 + CellNoise(p, cells * 4, seed + 2u) * 0.125;
    return Remap(GradientFbm(p, int3(cells / 2, cells / 2, 1), 5, seed + 3u), 0.0, 1.0, worley, 1.0);
}

// The shapes at `uv` in [0, 1): the low layer's billows (r, about 2.5 km cells over its 40 km tile) and its smooth
// noise for stratus (g), and the middle layer's billows (b) and the finer billows that cut them into cells (a, read at
// kCellTiles times the frequency).
float4 Shapes(float2 uv)
{
    return float4(Billows(uv, 16, 0x2f1b6c3du), GradientFbm(float3(uv, 0.5), int3(8, 8, 1), 5, 0x7a4c91e3u), Billows(uv, 16, 0x5a8e2f41u),
        Billows(uv, 16, 0x1d9f6b27u));
}

groupshared uint g_histogram[4 * kHistogramBins];

void WriteDistribution(uint index)
{
    for (uint bin = index; bin < 4 * kHistogramBins; bin += 64)
        g_histogram[bin] = 0;
    GroupMemoryBarrierWithGroupSync();
    RWTexture2D<unorm float4> shapes = ResourceDescriptorHeap[g_pass.input1];
    uint width, height;
    shapes.GetDimensions(width, height);
    uint across = width / kHistogramStride;
    uint samples = across * (height / kHistogramStride);
    for (uint s = index; s < samples; s += 64) {
        float4 value = shapes[uint2(s % across, s / across) * kHistogramStride];
        [unroll]
        for (uint c = 0; c < 4; ++c)
            InterlockedAdd(g_histogram[c * kHistogramBins + min(uint(value[c] * kHistogramBins), kHistogramBins - 1)], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index >= 4)
        return;
    RWTexture2D<float> distribution = ResourceDescriptorHeap[g_pass.output0];
    uint below = 0;
    for (uint b = 0; b < kHistogramBins; ++b) {
        below += g_histogram[index * kHistogramBins + b];
        distribution[uint2(b, index)] = float(below) / float(samples);
    }
}

// A channel's value where `value` of it lies below, between its bin's ends.
float Equalised(RWTexture2D<float> distribution, uint channel, float value)
{
    uint bin = min(uint(value * kHistogramBins), kHistogramBins - 1);
    float below = bin > 0 ? distribution[uint2(bin - 1, channel)] : 0.0;
    return lerp(below, distribution[uint2(bin, channel)], saturate(value * kHistogramBins - float(bin)));
}

// ---- Density.

float Linstep(float low, float high, float value) { return saturate((value - low) / (high - low)); }

// A normal distribution's quantile of `value`, by the logistic approximation (scale 1.702).
float NormalQuantile(float value)
{
    value = clamp(value, 1e-4, 1.0 - 1e-4);
    return log(value / (1.0 - value)) / 1.702;
}

// How strongly cloud forms where the billows, spread evenly over [0, 1], stand at `billows` and `coverage` of the sky is
// cloud: by how many standard deviations a normally distributed noise (as Photon's billowy noise is, nearly) would stand
// above the edge the coverage cuts. It rises steeply from the edge, so outlines are sharp, and slowly in the few strongest
// places, so most clouds stay low. A power of the evenly spread value rose slowly from the edge and left outlines blurred
// (2026-11-03).
float Strength(float billows, float coverage)
{
    float edge = 1.0 - coverage;
    return billows > edge ? saturate(kStrength * (NormalQuantile(billows) - NormalQuantile(edge))) : 0.0;
}

// x (2 - x): a cheap curve like a square root.
float Dampen(float value)
{
    value = saturate(value);
    return value * (2.0 - value);
}

// Soft saturation, harder edges the larger `hardness`.
float Lift(float value, float hardness) { return value * (1.0 + hardness) / (1.0 + hardness * value); }

// A layer's cloud density (0 to 1) at `p`, `altitude` above the ground and `height` up the layer, without the fine
// detail unless `detail`. How strongly cloud forms there comes from the billows cut at the coverage (most of it weak, so
// most clouds stay low), the middle layer's cut into cells, the low layer's blended toward a stratus sheet by type; height
// carves it away, so only strong places rise high and every height keeps the billows' outline, over a flat base; the
// cellular noises erode it, the more where it is weak, so cores stay dense; edges harden with height, and bases are thin.
float LayerDensity(CloudConstants clouds, CloudLayer layer, float3 p, float altitude, float height, bool detail)
{
    if (height <= 0.0 || height >= 1.0)
        return 0.0;
    float h = height / kTopShare;
    bool middle = layer.field != 0;
    float2 uv = p.xy * layer.weatherFrequency + layer.offset.xy;
    Texture2D<float4> weatherTexture = ResourceDescriptorHeap[clouds.weatherView];
    Texture2D<float4> shapes = ResourceDescriptorHeap[clouds.shapesView];
    float4 weather = weatherTexture.SampleLevel(g_linearWrap, uv, 0.0);
    float4 shape = shapes.SampleLevel(g_linearWrap, uv, 0.0);
    float coverage = saturate(layer.coverage + (weather.r - 0.5) * (middle ? kMiddleRegions : kLowRegions));
    if (coverage <= 0.0)
        return 0.0;
    float stratus = middle ? 0.0 : saturate(1.0 - layer.type - (weather.g - 0.5) * kTypeRegions);
    float cloud = Strength(middle ? shape.b : shape.r, coverage);
    if (middle) {
        float cells = shapes.SampleLevel(g_linearWrap, uv * kCellTiles, 0.0).a;
        cloud -= kCellCarve * (1.0 - cells) * (1.0 - cells) * Dampen(1.0 - cloud);
    }
    if (stratus > 0.0) {
        float sheet = smoothstep(1.0 - coverage - kStratusEdge, 1.0 - coverage + kStratusEdge, shape.g);
        sheet = 0.5 * sheet + sheet * Linstep(0.4, 0.8, shape.r);
        sheet /= sheet + 1.0;
        cloud = lerp(cloud, sheet, stratus);
        cloud *= lerp(1.0, Dampen(saturate(2.0 * h) * Linstep(0.0, 0.1, h) * Linstep(0.0, 0.6, 1.0 - h)), stratus);
    }
    cloud -= smoothstep(0.2, 1.0, h) * (0.6 - 0.3 * stratus);
    cloud *= smoothstep(0.0, 0.2, h);
    if (cloud <= 0.0)
        return 0.0;
    float2 erosion = (middle ? float2(0.0, kMiddleErosion) : lerp(kCumulusErosion, kStratusErosion, float2(stratus * stratus, stratus))) * layer.erosion;
    if (erosion.x > 0.0) {
        Texture3D<float4> shapeTexture = ResourceDescriptorHeap[clouds.shapeNoiseView];
        float3 shapePoint = float3(p.xy * layer.shapeFrequency + layer.erosionOffset.xy, altitude * layer.shapeFrequency + layer.offset.z);
        float billows = shapeTexture.SampleLevel(g_linearWrap, shapePoint, 0.0).g;
        cloud -= erosion.x * billows * billows * Dampen(1.0 - cloud);
        if (cloud <= 0.0)
            return 0.0;
    }
    if (detail && erosion.y > 0.0) {
        Texture3D<float4> detailTexture = ResourceDescriptorHeap[clouds.detailNoiseView];
        float3 detailPoint = float3(p.xy * layer.detailFrequency + layer.erosionOffset.zw, altitude * layer.detailFrequency);
        float fine = detailTexture.SampleLevel(g_linearWrap, detailPoint, 0.0).r;
        // Ragged toward the base, crisp above.
        float fade = middle ? 1.0 : 0.6 + 0.2 * smoothstep(0.85, 1.0, 1.0 - h) - 0.35 * smoothstep(0.05, 0.5, h);
        cloud -= erosion.y * fine * fine * Dampen(1.0 - cloud) * fade;
    }
    cloud = max(cloud, 0.0);
    float2 hardness = middle ? float2(3.0, 14.0) : lerp(float2(3.0, 12.0), float2(2.0, 7.0), stratus * stratus);
    cloud = Lift(cloud, lerp(hardness.x, hardness.y, saturate(h)));
    return cloud * (0.1 + 0.9 * smoothstep(0.2, 0.7, h));
}

// The density at `p` in the layer, with its altitude and height worked out.
float DensityAt(CloudConstants clouds, CloudLayer layer, float3 p, bool detail)
{
    float r = length(p);
    return LayerDensity(clouds, layer, p, r - kGroundRadius, (r - layer.bottom) / (layer.top - layer.bottom), detail);
}

// The cirrus' optical depth straight through it above `p`, seen over `footprint` kilometres: the weather's long streaks
// along the wind, the strands' veils and their fibres, cut together at its coverage, so few clouds are wisps along the
// streaks' crests with fibrous edges, thicker along their fibres. Cut apart and multiplied, few clouds broke into short
// dashes; with the streaks weighing most, they ran unbroken like contrails (2026-11-03). The strands' mip follows the
// footprint, so the fibres average out where a pixel spans many.
float CirrusDepth(CloudConstants clouds, float3 p, float footprint)
{
    if (clouds.cirrusCoverage <= 0.0)
        return 0.0;
    float2 along = float2(dot(p.xy, clouds.cirrusAxis), dot(p.xy, float2(-clouds.cirrusAxis.y, clouds.cirrusAxis.x)));
    Texture2D<float4> weatherTexture = ResourceDescriptorHeap[clouds.weatherView];
    Texture2D<float4> strands = ResourceDescriptorHeap[clouds.cirrusView];
    float streaks = weatherTexture.SampleLevel(g_linearWrap, along * clouds.cirrusFrequency + clouds.cirrusOffset, 0.0).b;
    float2 coordinates = along * clouds.cirrusStrandFrequency + clouds.cirrusOffset * (clouds.cirrusStrandFrequency / clouds.cirrusFrequency);
    float lod = log2(max(footprint * clouds.cirrusStrandFrequency * kCirrusSize, 1.0));
    float2 strand = strands.SampleLevel(g_linearWrap, coordinates, lod).rg;
    float cut = 1.0 - clouds.cirrusCoverage;
    float cover = smoothstep(cut, cut + kCirrusEdge, streaks * 0.5 + strand.r * 0.4 + strand.g * 0.1);
    return clouds.cirrusDepth * cover * (0.5 + 0.5 * strand.g);
}

// ---- Light.

struct CloudLight
{
    float3 irradiance;
    float3 sky;
    float3 ground;
};

CloudLight LoadLight(uint view)
{
    Texture2D<float4> texels = ResourceDescriptorHeap[view];
    CloudLight light;
    light.irradiance = texels.Load(int3(kLightIrradiance, 0, 0)).rgb;
    light.sky = texels.Load(int3(kLightSky, 0, 0)).rgb;
    light.ground = texels.Load(int3(kLightGround, 0, 0)).rgb;
    return light;
}

float HenyeyGreenstein(float cosine, float g)
{
    float d = 1.0 + g * g - 2.0 * g * cosine;
    return (1.0 - g * g) / (4.0 * 3.14159265 * d * sqrt(max(d, 1e-8)));
}

// Draine's phase function (2003): Henyey-Greenstein's shaped by (1 + alpha cos^2) and normalised.
float DrainePhase(float cosine, float g, float alpha)
{
    return HenyeyGreenstein(cosine, g) * (1.0 + alpha * cosine * cosine) / (1.0 + alpha * (1.0 + 2.0 * g * g) / 3.0);
}

// Jendersie and d'Eon's (2023, eq. 3) blend of a Henyey-Greenstein peak and Draine's bulk, blended toward an even phase
// function by `eccentricity` for the octaves of multiple scattering. Wrenninge et al. scale a Henyey-Greenstein lobe's g
// instead; this blend has no single g, and light scattered many times leaves evenly.
float CloudPhase(float cosine, float4 phase, float eccentricity)
{
    float droplets = lerp(HenyeyGreenstein(cosine, phase.x), DrainePhase(cosine, phase.y, phase.z), phase.w);
    return lerp(1.0 / (4.0 * 3.14159265), droplets, eccentricity);
}

// The octaves' phase functions toward the eye times their scattering, and their extinctions, at most eight (the first
// four in [0]).
struct Octaves
{
    float4 phases[2];
    float4 extinctions[2];
};

Octaves OctavesToward(CloudConstants clouds, float cosine)
{
    Octaves octaves = (Octaves)0;
    float scatter = 1.0;
    float eccentricity = 1.0;
    [unroll]
    for (uint n = 0; n < 8; ++n) {
        if (n < clouds.octaves) {
            octaves.phases[n / 4][n % 4] = scatter * CloudPhase(cosine, clouds.phase, eccentricity);
            octaves.extinctions[n / 4][n % 4] = scatter;
        }
        scatter *= clouds.octaveScatter;
        eccentricity *= clouds.octaveEccentricity;
    }
    return octaves;
}

// The light the octaves scatter toward the eye from light that crossed `depth` toward the point, per unit of irradiance,
// the octaves past the first times `multiple`: an optically thin medium scatters light about once, and the octaves would
// add light to thin wisps regardless.
float OctaveLight(Octaves octaves, float depth, float multiple)
{
    float total = dot(octaves.phases[0], exp(-octaves.extinctions[0] * depth)) + dot(octaves.phases[1], exp(-octaves.extinctions[1] * depth));
    float single = octaves.phases[0].x * exp(-octaves.extinctions[0].x * depth);
    return single + (total - single) * multiple;
}

// The light's irradiance at `p` through the air above, none where the planet hides it.
float3 LightAt(CloudConstants clouds, CloudLight light, float3 p, Texture2D<float4> transmittance)
{
    if (SphereDistance(p, clouds.lightDirection, kGroundRadius) >= 0.0)
        return 0.0;
    float r = length(p);
    return light.irradiance * AtmosphereTransmittance(transmittance, g_linearClamp, r, dot(p, clouds.lightDirection) / r);
}

// The optical depth from `p` toward the light through its layer, to where the light enters it or kLightDistance, in
// `steps` steps that lengthen away from `p` (Hillaire 2016, 5.5.2), here by a constant ratio from kFirstLightStep,
// jittered within them.
float LightDepth(CloudConstants clouds, CloudLayer layer, float3 p, float jitter, uint steps, bool detail)
{
    float3 direction = clouds.lightDirection;
    float nearest, farthest;
    SphereRoots(p, direction, layer.top, nearest, farthest);
    float span = min(farthest, kLightDistance);
    if (SphereRoots(p, direction, layer.bottom, nearest, farthest) && nearest > 0.0)
        span = min(span, nearest);
    if (span <= 0.0)
        return 0.0;
    float from = 0.0;
    float to = min(kFirstLightStep, span / steps);
    float ratio = pow(span / to, 1.0 / max(float(steps) - 1.0, 1.0));
    float depth = 0.0;
    [loop]
    for (uint j = 0; j < steps; ++j) {
        float3 q = p + direction * lerp(from, to, jitter);
        depth += DensityAt(clouds, layer, q, detail && j < steps / 2) * (to - from);
        from = to;
        to *= ratio;
    }
    return depth * layer.extinction;
}

// The share of the sky's light that reaches `p` through the cloud above it, from two samples up to kSkyLightDistance or
// the layer's top, carried in by the octaves of multiple scattering as sunlight is.
float SkyVisibility(CloudConstants clouds, CloudLayer layer, Octaves octaves, float3 p)
{
    float r = length(p);
    float span = min(layer.top - r, kSkyLightDistance);
    if (span <= 0.0)
        return 1.0;
    float3 up = p / r;
    float depth = (DensityAt(clouds, layer, p + up * (0.25 * span), false) + DensityAt(clouds, layer, p + up * (0.75 * span), false)) *
        (0.5 * span * layer.extinction);
    float4 first = octaves.extinctions[0];
    float4 second = octaves.extinctions[1];
    return (dot(first, exp(-first * depth)) + dot(second, exp(-second * depth))) / max(dot(first, 1.0) + dot(second, 1.0), 1e-6);
}

// ---- Tracing.

struct CloudTrace
{
    // The light the clouds scatter toward the eye, before the air in front, and the transmittance through them.
    float3 light;
    float transmittance;
    // The mean distance of the clouds seen, each point weighted by the transmittance to it (Hillaire 2016, eq. 21).
    float depthSum;
    float depthWeight;
};

// Marches a ray through a layer from `start` to `end`, `stepScale` times its steps across empty air. A cloud seen is
// lit by its outer few mean free paths, about 12 m at full density, finer than the steps: lit at a point anywhere in a
// step, dense clouds took the light of their dark insides and looked flat grey. So, as Schneider (2015) does, the march
// steps back once it meets cloud and goes on in steps of kStepDepth optical depth (at least kMinStep), back to the stride
// after kEmptyRun empty steps of an eighth of it.
void MarchLayer(CloudConstants clouds, CloudLayer layer, CloudLight light, float3 eye, float3 direction, float start, float end, float jitter,
    float stepScale, Octaves octaves, Texture2D<float4> transmittance, inout CloudTrace trace)
{
    float thickness = layer.top - layer.bottom;
    float span = end - start;
    uint steps = max(uint(round(lerp(1.0, kHorizonSteps, saturate((span / thickness - 1.0) / 10.0)) * float(layer.steps) * stepScale)), 4u);
    float stride = span / steps;
    float lightJitter = frac(jitter + 0.61803399);
    float t = start + jitter * stride;
    bool fine = false;
    uint empty = 0;
    [loop]
    for (uint i = 0; i < steps + kFineSamples && t < end && trace.transmittance > kOpaque; ++i) {
        float3 p = eye + direction * t;
        float r = length(p);
        float height = (r - layer.bottom) / thickness;
        float density = LayerDensity(clouds, layer, p, r - kGroundRadius, height, t < kDetailDistance);
        if (density <= 0.0) {
            if (fine && ++empty >= kEmptyRun)
                fine = false;
            t += fine ? stride / 8.0 : stride;
            continue;
        }
        if (!fine) {
            fine = true;
            empty = 0;
            t = max(t - stride, start);
            continue;
        }
        empty = 0;
        float extinction = density * layer.extinction;
        float ds = clamp(kStepDepth / extinction, kMinStep, stride);
        bool skin = trace.transmittance > kCheapLight;
        float depth = LightDepth(clouds, layer, p, lightJitter, skin ? clouds.lightSteps : max(clouds.lightSteps / 2, 2u), skin && t < kDetailDistance);
        // Ambient light: half the sphere sees the sky through the cloud above, half the ground.
        float3 radiance = LightAt(clouds, light, p, transmittance) * OctaveLight(octaves, depth, 1.0 - exp(-extinction * kMultipleScatteringLength)) +
            0.5 * (light.sky * SkyVisibility(clouds, layer, octaves, p) + light.ground);
        // Scattering equals extinction: (S - S exp(-extinction ds)) / extinction is S / extinction (1 - exp(-extinction ds)).
        float stepTransmittance = exp(-extinction * ds);
        trace.light += trace.transmittance * radiance * (1.0 - stepTransmittance);
        trace.depthSum += trace.transmittance * t;
        trace.depthWeight += trace.transmittance;
        trace.transmittance *= stepTransmittance;
        t += ds;
    }
}

// The cirrus sheet along a ray `pixelAngle` radians across: single scattering of the light it takes in, which it is too
// thin to dim much, and of the sky's.
void AddCirrus(CloudConstants clouds, CloudLight light, float3 eye, float3 direction, float cosine, float pixelAngle, Texture2D<float4> transmittance,
    inout CloudTrace trace)
{
    float nearest, farthest;
    if (!SphereRoots(eye, direction, clouds.cirrusRadius, nearest, farthest) || farthest <= 0.0)
        return;
    float t = nearest > 0.0 ? nearest : farthest;
    float ground = SphereDistance(eye, direction, kGroundRadius);
    if (ground >= 0.0 && ground < t)
        return;
    float3 p = eye + direction * t;
    float3 up = p / length(p);
    // The pixel's footprint on the sheet, along its longer side: stretched toward the horizon.
    float slant = max(abs(dot(up, direction)), 0.02);
    float depth = CirrusDepth(clouds, p, t * pixelAngle / slant);
    if (depth <= 0.0)
        return;
    float opacity = 1.0 - exp(-depth / slant);
    float3 radiance = LightAt(clouds, light, p, transmittance) * HenyeyGreenstein(cosine, kIceAsymmetry) + 0.5 * (light.sky + light.ground);
    trace.light += trace.transmittance * radiance * opacity;
    trace.depthSum += trace.transmittance * t;
    trace.depthWeight += trace.transmittance;
    trace.transmittance *= 1.0 - opacity;
}

// The clouds along a ray from `eye` `pixelAngle` radians across, the layers nearest first.
CloudTrace TraceClouds(CloudConstants clouds, CloudLight light, float3 eye, float3 direction, float jitter, float stepScale, float pixelAngle,
    Texture2D<float4> transmittance)
{
    CloudTrace trace = (CloudTrace)0;
    trace.transmittance = 1.0;
    float cosine = dot(direction, clouds.lightDirection);
    Octaves octaves = OctavesToward(clouds, cosine);
    float starts[2] = {0.0, 0.0};
    float ends[2] = {0.0, 0.0};
    bool crosses[2];
    [unroll]
    for (uint k = 0; k < 2; ++k)
        crosses[k] = (clouds.layerMask & (1u << k)) != 0 && ShellSegment(eye, direction, clouds.layers[k].bottom, clouds.layers[k].top, starts[k], ends[k]);
    uint first = crosses[1] && (!crosses[0] || starts[1] < starts[0]) ? 1 : 0;
    [loop]
    for (uint n = 0; n < 2; ++n) {
        uint index = n == 0 ? first : 1 - first;
        if (crosses[index])
            MarchLayer(clouds, clouds.layers[index], light, eye, direction, starts[index], ends[index], jitter, stepScale, octaves, transmittance, trace);
    }
    // A ray stopped where little gets through goes no further: what lies behind is hidden. The sun's disc, thousands of
    // times brighter than the sky, still showed through the light left at the stop.
    if (trace.transmittance <= kOpaque)
        trace.transmittance = 0.0;
    else if ((clouds.layerMask & kLayerCirrus) != 0)
        AddCirrus(clouds, light, eye, direction, cosine, pixelAngle, transmittance, trace);
    return trace;
}

// The transmittance of the air from `eye` to `distance` along a ray that does not meet the ground (Bruneton 2017): the
// transmittance to the top of the atmosphere from the eye over that from the far point.
float3 AirTransmittance(float3 eye, float3 direction, float distance, Texture2D<float4> transmittance)
{
    float3 far = eye + direction * distance;
    float rEye = length(eye);
    float rFar = length(far);
    float3 fromEye = AtmosphereTransmittance(transmittance, g_linearClamp, rEye, dot(eye, direction) / rEye);
    float3 fromFar = AtmosphereTransmittance(transmittance, g_linearClamp, rFar, dot(far, direction) / rFar);
    return saturate(fromEye / max(fromFar, 1e-6));
}

// The clouds as seen through the air in front of them (Hillaire 2016, 5.9.1) over `sky`, the sky's radiance that way: the
// air passes `air` of their light, and in front of them adds the share of the sky's light the air makes, about
// sky (1 - air), which they hide behind them in turn. Returns (light, transmittance), and the depth in kilometres.
float4 SeenClouds(CloudTrace trace, float3 eye, float3 direction, float3 sky, Texture2D<float4> transmittance, out float depth)
{
    depth = trace.depthWeight > 0.0 ? trace.depthSum / trace.depthWeight : kNoCloud;
    if (depth < 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float3 air = AirTransmittance(eye, direction, depth, transmittance);
    return float4(min(air * trace.light + sky * (1.0 - air) * (1.0 - trace.transmittance), 65504.0), trace.transmittance);
}

// The world direction through a texel of the history at `uv`, which spans the screen widened by the overscan on each side.
float3 HistoryDirection(CloudConstants clouds, float2 uv)
{
    float3 clip = float3(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0);
    return normalize(float3(dot(clouds.clipToDirection[0].xyz, clip), dot(clouds.clipToDirection[1].xyz, clip), dot(clouds.clipToDirection[2].xyz, clip)));
}

// Where a direction falls in a history whose clip space `rows` give, or false behind it.
bool HistoryUv(float4 rows[4], float3 direction, out float2 uv)
{
    float4 clip = float4(dot(rows[0].xyz, direction), dot(rows[1].xyz, direction), 0.0, dot(rows[3].xyz, direction));
    uv = clip.xy / max(clip.w, 1e-6) * float2(0.5, -0.5) + 0.5;
    return clip.w > 0.0;
}

// `history` at `uv` filtered by a Catmull-Rom spline in five bilinear taps (Jimenez, "Filmic SMAA", SIGGRAPH 2016; the
// four corner taps' small weights are dropped). The history is reprojected every frame as the wind moves the clouds, and
// bilinear filtering, applied again each time, blurred their detail away (2026-11-03).
float4 SampleCatmullRom(Texture2D<float4> history, float2 uv, float2 size)
{
    float2 position = uv * size;
    float2 centre = floor(position - 0.5) + 0.5;
    float2 f = position - centre;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 at0 = (centre - 1.0) / size;
    float2 at12 = (centre + w2 / w12) / size;
    float2 at3 = (centre + 2.0) / size;
    float4 sum = history.SampleLevel(g_linearClamp, float2(at12.x, at0.y), 0.0) * (w12.x * w0.y) +
        history.SampleLevel(g_linearClamp, float2(at0.x, at12.y), 0.0) * (w0.x * w12.y) +
        history.SampleLevel(g_linearClamp, at12, 0.0) * (w12.x * w12.y) +
        history.SampleLevel(g_linearClamp, float2(at3.x, at12.y), 0.0) * (w3.x * w12.y) +
        history.SampleLevel(g_linearClamp, float2(at12.x, at3.y), 0.0) * (w12.x * w3.y);
    return max(sum / (w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y), 0.0);
}

// The march's jitter for a texel of the history: interleaved gradient noise over the history's texels (Jimenez 2014),
// advanced by the golden ratio each time the texel is traced again (every `block` squared frames), so its traces spread
// evenly over a step. By the trace's texels, the texels of a block took the same jitter and erred alike, a lattice of
// blocks on bright clouds; advanced every frame instead, each block kept the same offsets, a finer lattice (2026-11-03).
float TraceNoise(uint2 texel, uint block)
{
    float noise = frac(52.9829189 * frac(dot(float2(texel), float2(0.06711056, 0.00583715))));
    return frac(noise + 0.61803399 * float(g_frame.frameIndex / (block * block)));
}

// Whether the screen shows the 2D sky anywhere around a history texel (a trace texel each side): pixels the depth leaves
// empty that the 3D skybox (input2, its distance per pixel, 0 for none) does not cover, and the overscan. A 6 x 6 grid
// over the area is checked.
bool SeesSky(CloudConstants clouds, uint2 texel)
{
    float2 low = (float2(texel) - float(clouds.traceBlock)) / float2(clouds.historySize);
    float2 high = (float2(texel) + float(clouds.traceBlock) + 1.0) / float2(clouds.historySize);
    float2 screenLow = (low * (1.0 + 2.0 * clouds.overscan) - clouds.overscan) * float2(g_frame.size);
    float2 screenHigh = (high * (1.0 + 2.0 * clouds.overscan) - clouds.overscan) * float2(g_frame.size);
    if (any(screenLow < 0.0) || any(screenHigh > float2(g_frame.size)))
        return true;
    uint2 skyboxSize = 0;
    if (g_pass.input2 != kNoView) {
        Texture2D<float> skybox = ResourceDescriptorHeap[g_pass.input2];
        skybox.GetDimensions(skyboxSize.x, skyboxSize.y);
    }
    [loop]
    for (uint k = 0; k < 36; ++k) {
        float2 at = lerp(screenLow, screenHigh, (float2(k % 6, k / 6) + 0.5) / 6.0);
        int2 pixel = int2(at);
        if (DepthAt(pixel) < 1.0)
            continue;
        if (g_pass.input2 == kNoView)
            return true;
        Texture2D<float> skybox = ResourceDescriptorHeap[g_pass.input2];
        int2 source = min(int2((float2(pixel) + 0.5) * float2(skyboxSize) / float2(g_frame.size)), int2(skyboxSize) - 1);
        if (skybox.Load(int3(source, 0)) <= 0.0)
            return true;
    }
    return false;
}

// ---- Steps.

groupshared float3 g_skySums[64];
groupshared float4 g_clearSums[64];
groupshared float3 g_cloudySums[64];
groupshared float g_shadowSums[64];

void WriteLight(CloudConstants clouds, uint index)
{
    // 256 directions even in solid angle over the upper hemisphere (16 rings even in z, 16 azimuths), and 256 texels of
    // the shadow map's coarse cascade.
    TextureCube<float4> sky = ResourceDescriptorHeap[clouds.skyView];
    float3 skySum = 0.0;
    float4 clearSum = 0.0;
    float3 cloudySum = 0.0;
    float shadowSum = 0.0;
    [loop]
    for (uint s = 0; s < 4; ++s) {
        uint k = index + 64 * s;
        float z = (k / 16 + 0.5) / 16.0;
        float azimuth = (k % 16 + 0.5 + 0.5 * (k / 16)) / 16.0 * 2.0 * 3.14159265;
        float across = sqrt(1.0 - z * z);
        float3 direction = float3(across * cos(azimuth), across * sin(azimuth), z);
        float3 clear = sky.SampleLevel(g_linearClamp, direction, 0.0).rgb;
        float3 cloudy = clear;
        if (g_pass.input1 != kNoView) {
            Texture2DArray<float4> cube = ResourceDescriptorHeap[g_pass.input1];
            uint3 texel = CubeTexel(direction, clouds.cubeTexels);
            float4 seen = cube.Load(int4(texel, 0));
            cloudy = clear * seen.a + seen.rgb;
        }
        skySum += clear;
        clearSum += float4(clear * z, z);
        cloudySum += cloudy * z;
        if (g_pass.input2 != kNoView) {
            Texture2D<float> shadow = ResourceDescriptorHeap[g_pass.input2];
            uint width, height;
            shadow.GetDimensions(width, height);
            shadowSum += shadow.Load(int3(height + (k % 16 + 0.5) * height / 16, (k / 16 + 0.5) * height / 16, 0));
        } else {
            shadowSum += 1.0;
        }
    }
    g_skySums[index] = skySum;
    g_clearSums[index] = clearSum;
    g_cloudySums[index] = cloudySum;
    g_shadowSums[index] = shadowSum;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32; stride > 0; stride /= 2) {
        if (index < stride) {
            g_skySums[index] += g_skySums[index + stride];
            g_clearSums[index] += g_clearSums[index + stride];
            g_cloudySums[index] += g_cloudySums[index + stride];
            g_shadowSums[index] += g_shadowSums[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index != 0)
        return;
    Texture2D<float4> bands = ResourceDescriptorHeap[clouds.bandsView];
    float scale = bands.Load(int3(kSkyBands, 0, 0)).y;
    float3 irradiance = scale * clouds.lightScale * clouds.lightTint;
    float3 skyMean = g_skySums[0] / 256.0;
    // The sky's irradiance on a surface facing up is pi times its cosine-weighted mean radiance.
    float3 clearMean = g_clearSums[0].rgb / g_clearSums[0].w;
    float3 cloudyMean = g_cloudySums[0] / g_clearSums[0].w;
    float shadow = g_shadowSums[0] / 256.0;
    Texture2D<float4> transmittance = ResourceDescriptorHeap[clouds.transmittanceView];
    float3 direction = clouds.lightDirection;
    float3 sunlit = irradiance * AtmosphereTransmittance(transmittance, g_linearClamp, kGroundRadius + kEyeHeight, direction.z) * max(direction.z, 0.0) * shadow;
    float3 ground = kGroundAlbedo / 3.14159265 * (sunlit + 3.14159265 * cloudyMean);
    float clearLuminance = Luminance(clearMean);
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    output[uint2(kLightIrradiance, 0)] = float4(irradiance, scale);
    output[uint2(kLightSky, 0)] = float4(skyMean, clearLuminance > 0.0 ? Luminance(cloudyMean) / clearLuminance : 1.0);
    output[uint2(kLightGround, 0)] = float4(ground, shadow);
}

// The shadow map's texel `id`: the line toward the sun through its place on the plane across the sun, from where it
// leaves the ground through every layer.
float ShadowTransmittance(CloudConstants clouds, uint2 id, uint size)
{
    uint cascade = id.x / size;
    float scale = cascade == 0 ? g_frame.cloudShadowFineScale : g_frame.cloudShadowCoarseScale;
    float2 plane = ((float2(id.x % size, id.y) + 0.5) / size - 0.5) / scale;
    float3 world = g_frame.cloudShadowU.xyz * (plane.x - g_frame.cloudShadowU.w) + g_frame.cloudShadowV.xyz * (plane.y - g_frame.cloudShadowV.w);
    float3 point0 = CloudSpace(world);
    float3 sun = g_frame.sunDirection;
    float nearest, farthest;
    float3 origin = SphereRoots(point0, sun, kGroundRadius, nearest, farthest) ? point0 + sun * farthest : point0 - sun * dot(point0, sun);
    float depth = 0.0;
    [unroll]
    for (uint k = 0; k < 2; ++k) {
        CloudLayer layer = clouds.layers[k];
        float start, end;
        if ((clouds.layerMask & (1u << k)) == 0 || !ShellSegment(origin, sun, layer.bottom, layer.top, start, end))
            continue;
        // Steps for the low layer, half as many for the middle one, through what may be tens of kilometres at low sun.
        uint steps = k == 0 ? 32 : 16;
        float stride = (end - start) / steps;
        float sum = 0.0;
        [loop]
        for (uint i = 0; i < steps; ++i)
            sum += DensityAt(clouds, layer, origin + sun * (start + (i + 0.5) * stride), cascade == 0);
        depth += sum * stride * layer.extinction;
    }
    if ((clouds.layerMask & kLayerCirrus) != 0) {
        if (SphereRoots(origin, sun, clouds.cirrusRadius, nearest, farthest) && farthest > 0.0) {
            float3 p = origin + sun * farthest;
            float slant = max(dot(p / length(p), sun), 0.02);
            depth += CirrusDepth(clouds, p, kKilometresPerUnit / (scale * size * slant)) / slant;
        }
    }
    return lerp(1.0, exp(-depth), clouds.shadowStrength);
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (g_pass.step == 0) {
        RWTexture3D<unorm float4> output = ResourceDescriptorHeap[g_pass.output0];
        float3 p = (float3(id) + 0.5) / kShapeSize;
        float worley = CellFbm(p, 4, 0x6c8e9cf5u);
        float perlin = GradientFbm(p, int3(4, 4, 4), 5, 0x7f4a7c15u);
        // Perlin noise grown by Worley noise into billows (Schneider 2015).
        output[id] = float4(Remap(perlin, 0.0, 1.0, worley, 1.0), worley, CellFbm(p, 8, 0x5bd1e995u), CellFbm(p, 16, 0x27d4eb2fu));
        return;
    }
    if (g_pass.step == 1) {
        RWTexture3D<unorm float4> output = ResourceDescriptorHeap[g_pass.output0];
        float3 p = (float3(id) + 0.5) / kDetailSize;
        output[id] = float4(CellFbm(p, 2, 0x165667b1u), CellFbm(p, 4, 0xd3a2646cu), CellFbm(p, 8, 0xfd7046c5u), 1.0);
        return;
    }
    if (g_pass.step == 2) {
        RWTexture2D<unorm float4> output = ResourceDescriptorHeap[g_pass.output0];
        float3 p = float3((float2(id.xy) + 0.5) / kWeatherSize, 0.5);
        // Coverage and type over a few cells a tile; the cirrus' streaks far longer along x than across it.
        float coverage = GradientFbm(p, int3(4, 4, 1), 5, 0x9e3779b9u);
        float type = GradientFbm(p, int3(3, 3, 1), 4, 0x85ebca6bu);
        // The streaks bend along a flow as the strands do: straight, they ran across the sky like contrails (2026-11-03).
        float2 bent = p.xy + kCirrusWarp * CurlFlow(p.xy, 2, 0x2545f491u);
        float streaks = GradientFbm(float3(bent, 0.5), int3(2, 24, 1), 5, 0xc2b2ae35u);
        // Spread the sums of octaves, which gather around a half, over the range.
        output[id.xy] = float4(saturate(float3(coverage, type, streaks) * 2.0 - 0.5), 1.0);
        return;
    }
    if (g_pass.step == 8) {
        RWTexture2D<unorm float4> output = ResourceDescriptorHeap[g_pass.output0];
        output[id.xy] = float4(CirrusStrands((float2(id.xy) + 0.5) / kCirrusSize), 0.0, 1.0);
        return;
    }
    if (g_pass.step == 9) {
        RWTexture2D<unorm float4> above = ResourceDescriptorHeap[g_pass.input1];
        RWTexture2D<unorm float4> output = ResourceDescriptorHeap[g_pass.output0];
        uint width, height;
        output.GetDimensions(width, height);
        if (all(id.xy < uint2(width, height)))
            output[id.xy] = 0.25 * (above[id.xy * 2] + above[id.xy * 2 + uint2(1, 0)] + above[id.xy * 2 + uint2(0, 1)] + above[id.xy * 2 + uint2(1, 1)]);
        return;
    }
    if (g_pass.step == 10 || g_pass.step == 12) {
        RWTexture2D<unorm float4> output = ResourceDescriptorHeap[g_pass.output0];
        uint width, height;
        output.GetDimensions(width, height);
        if (any(id.xy >= uint2(width, height)))
            return;
        if (g_pass.step == 10) {
            output[id.xy] = Shapes((float2(id.xy) + 0.5) / float2(width, height));
            return;
        }
        RWTexture2D<float> distribution = ResourceDescriptorHeap[g_pass.input1];
        float4 value = output[id.xy];
        [unroll]
        for (uint c = 0; c < 4; ++c)
            value[c] = Equalised(distribution, c, value[c]);
        output[id.xy] = value;
        return;
    }
    if (g_pass.step == 11) {
        WriteDistribution(index);
        return;
    }
    CloudConstants clouds = LoadClouds();
    if (g_pass.step == 3) {
        RWTexture2D<float> output = ResourceDescriptorHeap[g_pass.output0];
        uint width, height;
        output.GetDimensions(width, height);
        uint2 texel = g_pass.input1 == 0 ? id.xy : uint2(id.x, id.y * 2 + g_pass.input1 - 1);
        if (all(texel < uint2(width, height)))
            output[texel] = ShadowTransmittance(clouds, texel, height);
        return;
    }
    if (g_pass.step == 4) {
        WriteLight(clouds, index);
        return;
    }
    CloudLight light = LoadLight(g_pass.input1);
    Texture2D<float4> transmittance = ResourceDescriptorHeap[clouds.transmittanceView];
    TextureCube<float4> sky = ResourceDescriptorHeap[clouds.skyView];
    float3 eye = CloudEye();
    if (g_pass.step == 5) {
        RWTexture2DArray<float4> output = ResourceDescriptorHeap[g_pass.output0];
        bool everyTexel = g_pass.input2 == 1;
        // Its own 2 x 2 blocks, at the main view's offset in steps of 2 x 2 texels for 4 x 4 blocks.
        uint2 texel = everyTexel ? id.xy : id.xy * 2 + clouds.traceOffset / (clouds.traceBlock / 2);
        if (any(texel >= clouds.cubeTexels))
            return;
        float3 direction = normalize(FaceDirection(id.z, (float2(texel) + 0.5) * (2.0 / clouds.cubeTexels) - 1.0));
        uint seed = Hash(texel.x + texel.y * 4096u + id.z * 16777216u + Hash(g_frame.frameIndex));
        CloudTrace trace = TraceClouds(clouds, light, eye, direction, Random(seed), 0.5, 2.0 / clouds.cubeTexels, transmittance);
        float depth;
        float4 seen = SeenClouds(trace, eye, direction, sky.SampleLevel(g_linearClamp, direction, 0.0).rgb, transmittance, depth);
        uint3 at = uint3(texel, id.z);
        output[at] = everyTexel ? seen : lerp(output[at], seen, kCubeBlend);
        return;
    }
    if (g_pass.step == 6) {
        RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
        RWTexture2D<float> depthOutput = ResourceDescriptorHeap[g_pass.output1];
        uint2 texel = id.xy * clouds.traceBlock + clouds.traceOffset;
        uint2 size;
        output.GetDimensions(size.x, size.y);
        if (any(id.xy >= size))
            return;
        if (any(texel >= clouds.historySize) || !SeesSky(clouds, texel)) {
            output[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
            depthOutput[id.xy] = kMasked;
            return;
        }
        float2 uv = (float2(texel) + 0.5) / float2(clouds.historySize);
        float3 direction = HistoryDirection(clouds, uv);
        float pixelAngle = length(HistoryDirection(clouds, uv + float2(1.0 / clouds.historySize.x, 0.0)) - direction);
        CloudTrace trace = TraceClouds(clouds, light, eye, direction, TraceNoise(texel, clouds.traceBlock), 1.0, pixelAngle, transmittance);
        float depth;
        output[id.xy] = SeenClouds(trace, eye, direction, sky.SampleLevel(g_linearClamp, direction, 0.0).rgb, transmittance, depth);
        depthOutput[id.xy] = depth;
        return;
    }

    // Step 7.
    uint2 texel = id.xy;
    if (any(texel >= clouds.historySize))
        return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_pass.output0];
    Texture2D<float4> trace = ResourceDescriptorHeap[g_pass.input2];
    Texture2D<float> traceDepth = ResourceDescriptorHeap[g_pass.input3];
    uint2 traceSize;
    trace.GetDimensions(traceSize.x, traceSize.y);
    int2 center = int2(texel / clouds.traceBlock);
    float4 current = trace.Load(int3(center, 0));
    float depth = traceDepth.Load(int3(center, 0));
    float3 direction = HistoryDirection(clouds, (float2(texel) + 0.5) / float2(clouds.historySize));
    // Where the screen shows no sky the cube's clouds stand in, for other views' sky draws and sky that comes into view.
    if (depth <= kMasked) {
        output[texel] = CloudsToward(direction);
        return;
    }
    // The light traced around it: its bounds over the trace's 3 x 3 texels that see the sky.
    float4 low = 65504.0;
    float4 high = -65504.0;
    bool around = false;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            int2 at = clamp(center + int2(x, y), int2(0, 0), int2(traceSize) - 1);
            if (traceDepth.Load(int3(at, 0)) <= kMasked)
                continue;
            float4 value = trace.Load(int3(at, 0));
            low = min(low, value);
            high = max(high, value);
            around = true;
        }
    }
    // Where it was last frame: as far as the clouds traced here, moved back by the wind (straight ahead without them).
    float distance = depth > 0.0 ? depth / kKilometresPerUnit : 1e6;
    float3 position = g_frame.eyePosition + direction * distance - float3(clouds.windStep, 0.0);
    float2 previousUv;
    bool valid = clouds.historyValid != 0 && HistoryUv(clouds.prevViewProj, normalize(position - g_frame.prevEyePosition), previousUv) &&
        all(previousUv >= 0.0) && all(previousUv <= 1.0);
    float4 previous = current;
    if (valid) {
        Texture2D<float4> history = ResourceDescriptorHeap[g_pass.input1];
        previous = SampleCatmullRom(history, previousUv, float2(clouds.historySize));
        if (around)
            previous = clamp(previous, low, high);
    }
    bool fresh = all(texel % clouds.traceBlock == clouds.traceOffset);
    output[texel] = fresh && valid ? lerp(previous, current, kHistoryBlend) : previous;
}
