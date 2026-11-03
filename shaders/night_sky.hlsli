#ifndef GMODPP_NIGHT_SKY_HLSLI
#define GMODPP_NIGHT_SKY_HLSLI

#include "atmosphere.hlsli"
#include "cube.hlsli"
#include "dither.hlsli"

// What the sky shows beyond its atmosphere, in the 2D sky's draw (shaders/sky_draw.hlsl) and to rays that leave the map
// (SkyBeyond in shaders/reflection.hlsli): the night sky over the procedural sky (stars, the Milky Way, satellites and
// meteors as light in the atmosphere's units, atmosphere.hlsli, scaled by the procedural sky's gain and the air's
// transmittance) and the sun's or Moon's disc. Stars and the Milky Way are given in the stars' frame, which turns with
// the sun (src/night_sky.cpp); satellites and meteors come in the local frame.

static const uint kDiscSun = 1;
static const uint kDiscMoon = 2;

static const uint kNightStars = 1;
static const uint kNightMilkyWay = 2;
static const uint kNightSatellites = 4;
static const uint kNightMeteors = 8;

// Mirrors NightSkyConstants in src/night_sky.h.
struct NightSkyConstants
{
    float3 sunDirection;
    // The disc's angular radius in radians.
    float sunRadius;
    // env_sun's colour, its largest component 1.
    float3 sunColor;
    // The disc drawn: none, the Sun or, on night maps, the Moon (kDisc...).
    uint disc;
    uint transmittanceView;
    // What the night sky shows (kNight...), and how many satellites and meteors are in view.
    uint parts;
    uint satelliteCount;
    uint meteorCount;
    // The local frame to the stars' frame: its rows.
    float4 starFrame[3];
    // The clock stars twinkle by, in seconds.
    float time;
    // The Milky Way's starlight in the stars' frame in S10 units, a cube (MilkyWayStarlight).
    uint milkyWayView;
    // How the stars look (StarLook): times Earth's count, the faintest magnitude drawn, times their brightness, how
    // strongly (0 for none) and how fast they twinkle; and times the Milky Way's brightness.
    float starDensity;
    float faintestStar;
    float starBrightness;
    float twinkleStrength;
    float twinkleSpeed;
    float milkyWayBrightness;
    // The Moon of day maps (MoonDisc): its direction and irradiance in the atmosphere's units (0 without one); how much
    // of the sky its light shows (the procedural sky adds the clear sky it lights times this); the moonless night's floor
    // (times the sky with the map's sun); the irradiance of the map's own sun in the atmosphere's units (1, or the full
    // Moon's on night maps); and how much darker the sun's sky counts for the night sky's gain (Settings::earlierStars),
    // for shaders/sky_atmosphere.hlsl's step 4.
    float3 moonDirection;
    float moonIrradiance;
    float moonScale;
    float moonlessFloor;
    float mapLight;
    float starTwilight;
    // The map's sun (light_environment's), whose clear sky the moonless floor is, and the disc's radiance in the
    // procedural sky's units, which tells how much of it shows through clouds (DiscThroughClouds).
    float3 mapSunDirection;
    float discRadiance;
    // Satellites: their directions (xyz) and irradiance (w).
    float4 satellites[8];
    // Meteors (MeteorStreak): the direction each started from (xyz) and its head's angle along its path (w); the axis its
    // path turns about (xyz) and its tail's angle (w); its irradiance (x).
    float4 meteorStarts[2];
    float4 meteorAxes[2];
    float4 meteorFluxes[2];
};

// Points of light are Gaussians this many pixels wide (sigma), whose integral is their irradiance.
static const float kPointFootprint = 0.6;
// The brightest star (Sirius, -1.46).
static const float kBrightestStar = -1.5;
// The number of stars brighter than visual magnitude m over the whole sky, 10^(kStarCountSlope m + kStarCountOffset),
// fitted to the counts of the Bright Star Catalogue (to magnitude 6.5) and Tycho-2 (to 10) between magnitudes 1 and 10.
static const float kStarCountSlope = 0.487;
static const float kStarCountOffset = 0.740;

// Stars are drawn in two layers of cells over a cube's faces, each cell holding up to kStarCandidates stars, each with
// the chance its solid angle gives it: a coarse one for the stars bright enough for their glow to reach past a cell of
// the fine one, and a fine one for the rest down to the faintest that show against a dark sky. At Earth's count a fine
// cell holds 0.83 stars at most, so the candidates allow up to 4.8 times as many (Star amount), and more stars only add
// to those there were.
static const uint kStarCandidates = 4;
struct StarLayer {
    uint cells;
    float brightest;
    float faintest;
    uint seed;
};
static const StarLayer kBrightStars = {16, kBrightestStar, 4.0, 0x2c1b3c6du};
static const StarLayer kFaintStars = {400, 4.0, 10.0, 0x297a2d39u};

// Spectral classes O to M: the share of naked-eye stars in each (cumulative; the Bright Star Catalogue's stars to
// magnitude 6.5) and the effective temperatures they span: dwarfs' for O to F (Pecaut and Mamajek 2013), giants' for G
// to M, which most naked-eye stars of those classes are.
static const float kClassShares[7] = {0.006, 0.195, 0.403, 0.543, 0.681, 0.939, 1.0};
static const float2 kClassTemperatures[7] = {float2(33000.0, 41000.0), float2(10500.0, 31000.0), float2(7300.0, 9700.0), float2(6000.0, 7200.0),
    float2(4900.0, 5900.0), float2(3900.0, 5000.0), float2(3200.0, 3900.0)};

// Twinkling: the scintillation of a star seen with the naked eye, (sigma_I / I) at the zenith (our choice: no measured
// value for the eye was found), growing with the air mass to the power kScintillationExponent (variance as the 11/6
// power for apertures smaller than the shadows; Dravins et al. 1998, Osborn et al. 2015), and how often it changes. Real
// scintillation flickers at tens to hundreds of hertz, faster than the temporal anti-aliasing could keep; these rates
// survive it.
static const float kScintillationZenith = 0.15;
static const float kScintillationExponent = 11.0 / 12.0;
static const float kMaxScintillation = 0.8;
static const float2 kTwinkleRates = float2(1.5, 3.5);

// The Milky Way's integrated starlight in S10 units (stars of magnitude 10 per square degree; 6.60e-12 in the
// atmosphere's units per steradian), from Leinert et al.'s (1998) smooth model: 260 at the galactic centre, 100 in the
// plane at longitudes 120 and 240 degrees (a Gaussian in longitude between them, our choice), 50 at latitude 30 degrees
// (an exponential in latitude, kStarlightLatitude) and 20 toward the poles. Star clouds and dust lanes along the plane
// (our choice) gather it into the band seen.
static const float kS10Radiance = 6.60e-12;
static const float kStarlightCentre = 260.0;
static const float kStarlightPlane = 100.0;
static const float kStarlightPoles = 20.0;
static const float kStarlightLatitude = 0.534;
// The galactic frame in the stars' frame: the band crosses the sky at 62.87 degrees to the stars' equator, as the
// galactic plane does to the celestial equator, with the centre on the band's highest point while the stars' frame is
// the local one (a map's first night).
static const float3 kGalacticPole = float3(0.0, 0.88993, 0.45607);
static const float3 kGalacticCentre = float3(0.0, -0.45607, 0.88993);
static const float3 kGalacticLongitude = float3(1.0, 0.0, 0.0);

float Unit(uint hash)
{
    return float(hash >> 8) * (1.0 / 16777216.0);
}

float CumulativeStars(float magnitude)
{
    return pow(10.0, kStarCountSlope * magnitude + kStarCountOffset);
}

// The chromaticity of a black body (Kim et al. 2002's cubic spline of the Planckian locus, 1667 K to 25000 K) as
// linear sRGB (IEC 61966-2-1) of luminance 1.
float3 BlackbodyColor(float temperature)
{
    float t = 1.0 / clamp(temperature, 1667.0, 25000.0);
    float x = t >= 1.0 / 4000.0 ? ((-0.2661239e9 * t - 0.2343580e6) * t + 0.8776956e3) * t + 0.179910
                                : ((-3.0258469e9 * t + 2.1070379e6) * t + 0.2226347e3) * t + 0.240390;
    float y = t >= 1.0 / 2222.0 ? ((-1.1063814 * x - 1.34811020) * x + 2.18555832) * x - 0.20219683
            : t >= 1.0 / 4000.0 ? ((-0.9549476 * x - 1.37418593) * x + 2.09137015) * x - 0.16748867
                                : ((3.0817580 * x - 5.87338670) * x + 3.75112997) * x - 0.37001483;
    float3 xyz = float3(x / y, 1.0, (1.0 - x - y) / y);
    float3x3 toSrgb = {3.2406, -1.5372, -0.4986, -0.9689, 1.8758, 0.0415, 0.0557, -0.2040, 1.0570};
    return max(mul(toSrgb, xyz), 0.0);
}

float3 StarColor(float classPick, float withinClass)
{
    uint spectralClass = 0;
    [unroll]
    for (uint i = 0; i < 6; ++i)
        spectralClass += classPick >= kClassShares[i] ? 1 : 0;
    float2 range = kClassTemperatures[spectralClass];
    return BlackbodyColor(exp(lerp(log(range.x), log(range.y), withinClass)));
}

// Kasten and Young's (1989) relative air mass toward a direction `cosine` to the zenith.
float AirMass(float cosine)
{
    float zenith = degrees(acos(saturate(cosine)));
    return 1.0 / (saturate(cosine) + 0.50572 * pow(96.07995 - zenith, -1.6364));
}

// How strongly stars in a direction twinkle (the local frame's z is up).
float Scintillation(float3 direction)
{
    return min(kScintillationZenith * pow(AirMass(direction.z), kScintillationExponent), kMaxScintillation);
}

// How the stars toward a direction look (NightSkyConstants): times Earth's count, the faintest magnitude drawn, times
// their brightness, the clock they twinkle by (sped up) and how strongly they twinkle there.
struct StarLook
{
    float density;
    float faintest;
    float brightness;
    float time;
    float scintillation;
};

// A star's brightness over time, around 1: smooth noise at its own rate and phase.
float Twinkle(uint hash, float time, float scintillation)
{
    float t = time * lerp(kTwinkleRates.x, kTwinkleRates.y, Unit(hash)) + Unit(DitherHash(hash)) * 256.0;
    uint step = uint(t);
    float from = Unit(DitherHash(hash + step * 0x9e3779b9u));
    float to = Unit(DitherHash(hash + (step + 1) * 0x9e3779b9u));
    // Uniform noise in [-1, 1] blended this way has a standard deviation of about 0.5.
    float noise = lerp(from, to, smoothstep(0.0, 1.0, frac(t))) * 2.0 - 1.0;
    return max(1.0 + 2.0 * scintillation * noise, 0.0);
}

// The light of the stars in a cell of a layer toward `direction`: each candidate is there with its share of the chance
// the cell's solid angle and the layer's `density` (stars per steradian at Earth's count) give it.
float3 StarsInCell(float3 direction, uint face, int2 cell, StarLayer layer, float density, float sigma, StarLook look)
{
    float size = 2.0 / layer.cells;
    float2 corner = float2(cell) * size - 1.0;
    float2 centre = corner + 0.5 * size;
    float chance = density * look.density * size * size / pow(1.0 + dot(centre, centre), 1.5) / kStarCandidates;
    uint cellHash = DitherHash(((face * layer.cells + uint(cell.y)) * layer.cells + uint(cell.x)) ^ layer.seed);
    float3 light = 0.0;
    [loop]
    for (uint candidate = 0; candidate < kStarCandidates; ++candidate) {
        uint hash = DitherHash(cellHash + candidate * 0x9e3779b9u);
        if (Unit(hash) >= chance)
            continue;
        uint h1 = DitherHash(hash);
        uint h2 = DitherHash(h1);
        float3 star = normalize(FaceDirection(face, corner + float2(Unit(h1), Unit(h2)) * size));
        // |direction - star| is the angle between them to well under a pixel's precision, where the cosine is not.
        float3 offset = direction - star;
        float footprint = exp(-dot(offset, offset) / (2.0 * sigma * sigma));
        if (footprint <= 0.0)
            continue;
        uint h3 = DitherHash(h2);
        float brightest = CumulativeStars(layer.brightest);
        float magnitude = (log10(lerp(brightest, CumulativeStars(layer.faintest), Unit(h3))) - kStarCountOffset) / kStarCountSlope;
        if (magnitude > look.faintest)
            continue;
        uint h4 = DitherHash(h3);
        uint h5 = DitherHash(h4);
        float3 color = StarColor(Unit(h4), Unit(h5));
        float twinkle = Twinkle(DitherHash(h5), look.time, look.scintillation);
        light += MagnitudeFlux(magnitude) * look.brightness * twinkle * footprint / (2.0 * 3.14159265 * sigma * sigma) * color;
    }
    return light;
}

// The light of a layer's stars toward `direction`: those of its cell and the eight around it, across the cube's edges.
// Around a cube's corner, where three faces meet, the cell beyond both edges is one of the others and is skipped. A
// footprint wider than a quarter of a cell (a blurred reflection) reaches stars past those cells: the layer fades out by
// half a cell, its stars' light then small beside the sky's in so wide a footprint.
float3 StarLayerLight(float3 direction, StarLayer layer, float sigma, StarLook look)
{
    float fade = saturate(2.0 - 4.0 * sigma * layer.cells / (0.5 * 3.14159265));
    if (fade <= 0.0 || look.faintest <= layer.brightest)
        return 0.0;
    uint face;
    float2 uv = CubeFaceUv(direction, face);
    int cells = int(layer.cells);
    int2 cell = min(int2((uv * 0.5 + 0.5) * cells), cells - 1);
    float density = (CumulativeStars(layer.faintest) - CumulativeStars(layer.brightest)) / (4.0 * 3.14159265);
    float3 light = 0.0;
    [loop]
    for (int i = 0; i < 9; ++i) {
        int2 neighbour = cell + int2(i % 3, i / 3) - 1;
        bool2 outside = or(neighbour < 0, neighbour >= cells);
        if (all(outside))
            continue;
        uint neighbourFace = face;
        if (any(outside)) {
            float3 across = FaceDirection(face, (float2(neighbour) + 0.5) * (2.0 / cells) - 1.0);
            float2 acrossUv = CubeFaceUv(across, neighbourFace);
            neighbour = min(int2((acrossUv * 0.5 + 0.5) * cells), cells - 1);
        }
        light += StarsInCell(direction, neighbourFace, neighbour, layer, density, sigma, look);
    }
    return light * fade;
}

float3 StarLight(float3 direction, float sigma, StarLook look)
{
    float3 light = 0.0;
    // One loop over the layers keeps the code small where it is inlined.
    [loop]
    for (uint i = 0; i < 2; ++i) {
        StarLayer layer = kBrightStars;
        if (i == 1)
            layer = kFaintStars;
        light += StarLayerLight(direction, layer, sigma, look);
    }
    return light;
}

float ValueNoise(float3 position)
{
    int3 cell = int3(floor(position));
    float3 f = position - floor(position);
    f = f * f * (3.0 - 2.0 * f);
    float corners[8];
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        int3 corner = cell + int3(i & 1, (i >> 1) & 1, i >> 2);
        corners[i] = Unit(DitherHash(uint(corner.x) * 0x8da6b343u ^ uint(corner.y) * 0xd8163841u ^ uint(corner.z) * 0xcb1ab31fu));
    }
    float4 x = lerp(float4(corners[0], corners[2], corners[4], corners[6]), float4(corners[1], corners[3], corners[5], corners[7]), f.x);
    float2 y = lerp(x.xz, x.yw, f.y);
    return lerp(y.x, y.y, f.z);
}

// Fractal noise in [0, 1] over the sphere, features about 1 / `scale` radians across.
float FractalNoise(float3 direction, float scale)
{
    float sum = 0.0;
    float amplitude = 0.5;
    [loop]
    for (uint octave = 0; octave < 5; ++octave) {
        sum += amplitude * ValueNoise(direction * scale + octave * 17.0);
        scale *= 2.0;
        amplitude *= 0.5;
    }
    return sum / 0.96875;
}

// The Milky Way's integrated starlight toward a direction in the stars' frame in S10 units: the smooth starlight,
// gathered near the plane into clouds of stars and dimmed by dust lanes. Drawn once into a cube (shaders/sky_atmosphere.hlsl,
// step 6), which the night sky samples.
float3 MilkyWayStarlight(float3 direction)
{
    float latitude = asin(clamp(dot(direction, kGalacticPole), -1.0, 1.0));
    float longitude = atan2(dot(direction, kGalacticLongitude), dot(direction, kGalacticCentre));
    float plane = kStarlightPlane + (kStarlightCentre - kStarlightPlane) * exp(-longitude * longitude / (radians(40.0) * radians(40.0)));
    float smooth = kStarlightPoles + (plane - kStarlightPoles) * exp(-abs(latitude) / kStarlightLatitude);
    float nearPlane = exp(-abs(latitude) / radians(6.0));
    float clouds = lerp(1.0, lerp(0.4, 2.6, FractalNoise(direction, 12.0)), nearPlane);
    float dust = FractalNoise(direction + 3.7, 18.0) * exp(-abs(latitude) / radians(2.5)) * lerp(0.5, 1.6, exp(-abs(longitude) / radians(60.0)));
    float starlight = smooth * clouds * exp(-2.2 * dust);
    // Integrated starlight's colour (a 4800 K black body, our choice), reddened by dust.
    return starlight * BlackbodyColor(4800.0 - 600.0 * dust);
}

// A point of light toward `source`: its irradiance spread over a Gaussian of `sigma` radians.
float PointFootprint(float3 direction, float3 source, float sigma)
{
    float3 offset = direction - source;
    return exp(-dot(offset, offset) / (2.0 * sigma * sigma)) / (2.0 * 3.14159265 * sigma * sigma);
}

// A meteor's streak toward `direction`: the light of its last moments along its path, from the tail at angle `tail` to
// the head at `head` along the great circle from `start` around `axis`, brightest at the head and fading toward the tail,
// spread across the path by a Gaussian of `sigma` radians; `flux` in all.
float MeteorStreak(float3 direction, float3 start, float3 axis, float head, float tail, float flux, float sigma)
{
    float across = dot(direction, axis);
    float along = atan2(dot(direction, cross(axis, start)), dot(direction, start));
    float span = max(head - tail, sigma);
    float t = saturate((along - tail) / span);
    // The ends fade over a footprint, so the head does not alias.
    float ends = saturate((head + sigma - along) / (2.0 * sigma)) * saturate((along - tail + sigma) / (2.0 * sigma));
    return 2.0 * flux * t / span * ends * exp(-across * across / (2.0 * sigma * sigma)) / (2.50662827 * sigma);
}

// How bright the full Moon is across its disc (`offset` from its centre in radii, x along the horizon), relative to its
// highlands: evenly bright to its edge, as the lunar soil scatters light back toward the sun (Lommel-Seeliger at zero
// phase), with darker maria about 0.55 as bright as the highlands.
float MoonAlbedo(float2 offset)
{
    float maria = smoothstep(0.5, 0.62, FractalNoise(float3(offset, 0.37), 1.6));
    return lerp(1.0, 0.55, maria);
}

// The Moon's angular radius (1896 arcseconds across at opposition, NASA's Moon fact sheet): its surface is as bright as
// its light over a disc this size, whatever size it is drawn at.
static const float kMoonAngularRadius = 4.596e-3;

// The Moon of day maps toward a direction, after the tonemap scale, drawn as wide as the sun's disc, and `coverage`, the
// share of a footprint `edge` radians wide inside it (it hides the stars behind it). The sun lights the sphere at its
// phase (Lommel-Seeliger, so a full Moon is even to its edge) and the maria (MoonAlbedo). Its surface is as bright as
// the real Moon's (its irradiance over the lit share of its disc) at the gain the sky there is shown with (`logGain`),
// as bright as the target holds at most: pale by day, bright at night. The air colours it, and the ground hides it.
float3 MoonDisc(NightSkyConstants night, float3 direction, float edge, float logGain, float tonemapScale, Texture2D<float4> transmittance,
    SamplerState linearClamp, out float coverage)
{
    coverage = 0.0;
    if (night.moonIrradiance <= 0.0)
        return 0.0;
    float3 moon = night.moonDirection;
    float angle = atan2(length(cross(direction, moon)), dot(direction, moon));
    float3 eye = float3(0.0, 0.0, kGroundRadius + kEyeHeight);
    if (angle >= night.sunRadius + edge || SphereDistance(eye, direction, kGroundRadius) >= 0.0)
        return 0.0;
    coverage = saturate((night.sunRadius - angle) / max(edge, 1e-7) + 0.5);
    float3 across = abs(moon.z) < 0.999 ? normalize(cross(float3(0.0, 0.0, 1.0), moon)) : float3(1.0, 0.0, 0.0);
    float3 up = cross(moon, across);
    float2 offset = float2(dot(direction, across), dot(direction, up)) / night.sunRadius;
    // The edge's footprint reaches past the disc, where the sphere has no surface: it is shaded as the edge is. There the
    // shading's terms both go to 0, and a sign flipping between jittered samples made the rim flicker.
    float2 onSphere = offset * min(1.0, 0.995 / max(length(offset), 1e-6));
    float towardEye = sqrt(1.0 - dot(onSphere, onSphere));
    float3 normal = onSphere.x * across + onSphere.y * up - towardEye * moon;
    float lit = dot(normal, night.sunDirection);
    // At most as bright as the target holds, as `shown` is.
    float shade = lit > 0.0 ? min(2.0 * lit / (lit + towardEye), 1.0) : 0.0;
    float litShare = 0.5 - 0.5 * dot(moon, night.sunDirection);
    float radiance = night.moonIrradiance / (3.14159265 * kMoonAngularRadius * kMoonAngularRadius * max(litShare, 0.01));
    float shown = min(radiance * exp2(logGain) * tonemapScale, 1.0);
    float3 color = AtmosphereTransmittance(transmittance, linearClamp, eye.z, moon.z);
    color /= max(max(color.r, max(color.g, color.b)), 1e-6);
    return color * (MoonAlbedo(offset) * shade * shown * coverage);
}

// The sun's disc toward a direction, after the tonemap scale. It is thousands of times brighter than the sky, so it is
// shown as bright as the target holds, in the colour the atmosphere leaves it (env_sun's tint times the transmittance
// toward it), with Allen's limb darkening at 550 nm (I / I0 = 0.3 + 0.93 mu - 0.23 mu^2; Cox, Allen's Astrophysical
// Quantities, 2000), or as the Moon (MoonAlbedo). Its edge covers the share of a footprint `edge` radians wide inside it,
// and the ground hides it below the horizon.
float3 SunDisc(NightSkyConstants night, float3 direction, float edge, Texture2D<float4> transmittance, SamplerState linearClamp)
{
    float3 sun = night.sunDirection;
    float angle = atan2(length(cross(direction, sun)), dot(direction, sun));
    float coverage = saturate((night.sunRadius - angle) / max(edge, 1e-7) + 0.5);
    float3 eye = float3(0.0, 0.0, kGroundRadius + kEyeHeight);
    if (coverage <= 0.0 || SphereDistance(eye, direction, kGroundRadius) >= 0.0)
        return 0.0;
    float3 color = night.sunColor * AtmosphereTransmittance(transmittance, linearClamp, eye.z, sun.z);
    color /= max(max(color.r, max(color.g, color.b)), 1e-6);
    if (night.disc == kDiscMoon) {
        float3 across = abs(sun.z) < 0.999 ? normalize(cross(float3(0.0, 0.0, 1.0), sun)) : float3(1.0, 0.0, 0.0);
        float2 offset = float2(dot(direction, across), dot(direction, cross(sun, across))) / night.sunRadius;
        return color * (MoonAlbedo(offset) * coverage);
    }
    float r = saturate(angle / night.sunRadius);
    float mu = sqrt(1.0 - r * r);
    return color * ((0.3 + 0.93 * mu - 0.23 * mu * mu) * coverage);
}

// How much of the disc (SunDisc) shows through clouds that let `transmittance` of it through: shown as bright as the
// target holds, it dims only where so little of its radiance gets through that the target holds it.
float DiscThroughClouds(NightSkyConstants night, float transmittance, float tonemapScale)
{
    return saturate(transmittance * night.discRadiance * tonemapScale);
}

// The log2 of the gain the night sky is drawn with (NightSkyLight, MoonDisc) behind clouds that let `transmittance` through.
float LogGainThroughClouds(float logGain, float transmittance)
{
    return logGain + log2(max(transmittance, 1e-30));
}

// The night sky's light toward a direction above the horizon, before the tonemap scale: its light (in the atmosphere's
// units) through the air, times the procedural sky's gain over the physical sky there (`logGain`, log2), so stars keep
// their real contrast against the sky they are seen on and fade out as it brightens. Points of light are Gaussians
// `sigma` radians wide. Nothing shows where even the brightest star would stay under an 8-bit target's step.
float3 NightSkyLight(NightSkyConstants night, float3 direction, float logGain, float sigma, float tonemapScale, Texture2D<float4> transmittance,
    SamplerState linearClamp)
{
    float3 eye = float3(0.0, 0.0, kGroundRadius + kEyeHeight);
    if (SphereDistance(eye, direction, kGroundRadius) >= 0.0)
        return 0.0;
    float gain = exp2(logGain);
    float brightest = MagnitudeFlux(kBrightestStar) * max(max(night.starBrightness, night.milkyWayBrightness), 1.0);
    if (gain * brightest / (2.0 * 3.14159265 * sigma * sigma) * tonemapScale < 1.0 / 512.0)
        return 0.0;
    float3 stars = float3(dot(night.starFrame[0].xyz, direction), dot(night.starFrame[1].xyz, direction), dot(night.starFrame[2].xyz, direction));
    float3 light = 0.0;
    if (night.parts & kNightStars) {
        StarLook look;
        look.density = night.starDensity;
        look.faintest = night.faintestStar;
        look.brightness = night.starBrightness;
        look.time = night.time * night.twinkleSpeed;
        look.scintillation = Scintillation(direction) * night.twinkleStrength;
        light += StarLight(stars, sigma, look);
    }
    if (night.parts & kNightMilkyWay) {
        TextureCube<float4> milkyWay = ResourceDescriptorHeap[night.milkyWayView];
        light += milkyWay.SampleLevel(linearClamp, stars, 0.0).rgb * (kS10Radiance * night.milkyWayBrightness);
    }
    [loop]
    for (uint i = 0; i < night.satelliteCount; ++i)
        light += night.satellites[i].w * PointFootprint(direction, night.satellites[i].xyz, sigma);
    [loop]
    for (uint j = 0; j < night.meteorCount; ++j) {
        float4 start = night.meteorStarts[j];
        float4 axis = night.meteorAxes[j];
        light += MeteorStreak(direction, start.xyz, axis.xyz, start.w, axis.w, night.meteorFluxes[j].x, sigma);
    }
    return gain * AtmosphereTransmittance(transmittance, linearClamp, eye.z, direction.z) * light;
}

#endif
