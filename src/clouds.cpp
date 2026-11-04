#include "clouds.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>

namespace gmodpp {
namespace {

// The procedural sky's planet and eye (kGroundRadius and kEyeHeight in shaders/atmosphere.hlsli), in kilometres: the map's
// origin stands kEyeHeight above the ground (CloudSpace in shaders/clouds.hlsl).
constexpr float kGroundRadius = 6360.0f;
constexpr float kEyeHeight = 0.2f;
// A cumulus' extinction where densest, per kilometre: Hess et al. (1998) measured 0.05 to 0.12 per metre (Hillaire 2016,
// section 5.2); we take 0.08.
constexpr float kCumulusExtinction = 80.0f;
// Our choices of scale at Size 1, after the Photon shader pack's (studied, not copied): the textures of the weather (where
// clouds may form, and their type) and the clouds' shapes tile every kWeatherTile km, the middle layer's every
// kMiddleScale of that (read through hexagons, so the sky does not repeat), and within each layer's tile the erosion's
// billows (the shape volume's cellular noise) repeat kShapeTiles times
// (about 1.1 km) and its fine detail kDetailTiles times (about 200 m for the low layer, 330 m for the middle one), whole
// numbers so they wrap with it. The middle layer's clouds are half as dense. The cirrus' bands repeat every kCirrusTile
// km and its strands kCirrusStrandTiles times within that (about 10 m a texel).
constexpr float kWeatherTile = 40.0f;
constexpr float kMiddleScale = 0.5f;
constexpr float kShapeTiles[2] = {36.0f, 18.0f};
constexpr float kDetailTiles[2] = {200.0f, 60.0f};
constexpr float kMiddleDensity = 0.5f;
constexpr float kCirrusTile = 30.0f;
constexpr float kCirrusStrandTiles = 3.0f;
// The erosion's weights at the default Detail are shaders/clouds.hlsl's (kCumulusErosion and on).
constexpr float kDefaultDetail = 0.7f;
// Wind grows with height: the middle layer and the cirrus drift this many times as fast as the low layer (our choice).
constexpr double kMiddleWind = 1.5;
constexpr double kCirrusWind = 2.5;
// How fast the shapes change at Evolution 1: the billows rise kEvolutionSpeed kilometres a second through their noise,
// and the erosion's billows and fine detail drift kBillowDrift and kDetailDrift times the wind's speed ahead of it
// (Photon's 0.2 and 0.4).
constexpr double kEvolutionSpeed = 0.001;
constexpr double kBillowDrift = 0.2;
constexpr double kDetailDrift = 0.4;
// A change of weather takes this share of a weather's duration (our choice).
constexpr double kChangeShare = 1.0 / 3.0;
// Multiple scattering (Hillaire 2016, section 5.8): octaves, and how much of the droplets' anisotropy each octave keeps of
// the last's (shaders/clouds.hlsl blends it toward an even phase function). By our estimate, with three octaves a thick
// cloud facing the sun was about a sixth as bright as a white surface reflecting 70% of the light; eight bring it within
// about a factor of two.
constexpr uint32_t kOctaves = 8;
constexpr float kOctaveEccentricity = 0.5f;

// The hex lattice the clouds are read through (HexTilesAt in shaders/clouds.hlsl): hexagons kLayerHexSpacing and
// kCirrusHexSpacing tiles apart, whose hashes repeat every kHexPeriod of them along the lattice's axes, (1, 0) and
// (1/2, sqrt(3)/2) times the spacing.
constexpr double kHexPeriod = 256.0;
constexpr double kLayerHexSpacing = 0.25;
constexpr double kCirrusHexSpacing = 0.5;

double Wrap(double value)
{
    return value - std::floor(value);
}

// Wraps the wind's offset, in tiles, by whole periods of the hex lattice, so the clouds read through it stay the same.
void WrapHexes(std::array<double, 3>& offset, double spacing)
{
    const double rowHeight = spacing * std::numbers::sqrt3 / 2.0;
    const double row = offset[1] / rowHeight;
    const double along = offset[0] / spacing - 0.5 * row;
    const double wrappedRow = row - kHexPeriod * std::floor(row / kHexPeriod);
    const double wrappedAlong = along - kHexPeriod * std::floor(along / kHexPeriod);
    offset[0] = spacing * (wrappedAlong + 0.5 * wrappedRow);
    offset[1] = rowHeight * wrappedRow;
}

// The weathers a weather can turn into (our choices, after common sequences: fair-weather cumulus growing and fading,
// cirrus and altocumulus ahead of a front thickening to overcast, a deck breaking up).
std::span<const Weather> Turns(Weather weather)
{
    using enum Weather;
    static constexpr Weather clear[] = {FairWeather, Cirrus};
    static constexpr Weather fair[] = {Clear, Scattered, Cirrus};
    static constexpr Weather scattered[] = {FairWeather, Broken, Towering, MackerelSky};
    static constexpr Weather broken[] = {Scattered, Overcast};
    static constexpr Weather overcast[] = {Broken};
    static constexpr Weather mackerel[] = {Cirrus, Broken, Scattered};
    static constexpr Weather cirrus[] = {Clear, MackerelSky, FairWeather};
    static constexpr Weather towering[] = {Scattered, Broken};
    switch (weather) {
    case Clear: return clear;
    case Scattered: return scattered;
    case Broken: return broken;
    case Overcast: return overcast;
    case MackerelSky: return mackerel;
    case Cirrus: return cirrus;
    case Towering: return towering;
    default: return fair;
    }
}

// Jendersie and d'Eon's (2023, eq. 4-7) fit of Mie scattering by water droplets `diameter` micrometres across (5 to 50):
// the Henyey-Greenstein peak's g, Draine's g and alpha, and Draine's weight.
std::array<float, 4> DropletPhase(float diameter)
{
    return {std::exp(-0.0990567f / (diameter - 1.67154f)), std::exp(-2.20679f / (diameter + 3.91029f) - 0.428934f),
        std::exp(3.62489f - 8.29288f / (diameter + 5.52825f)), std::exp(-0.599085f / (diameter - 0.641583f) - 0.665888f)};
}

}

std::array<float, 2> CloudWeather::Advance(const Settings& settings)
{
    const auto now = std::chrono::steady_clock::now();
    const double seconds = m_last ? std::clamp(std::chrono::duration<double>(now - *m_last).count(), 0.0, 0.25) : 0.0;
    m_last = now;
    const double angle = static_cast<double>(settings.windDirection) * std::numbers::pi / 180.0;
    const double distance = static_cast<double>(settings.windSpeed) * 1e-3 * seconds;
    const double along[2] = {std::cos(angle) * distance, std::sin(angle) * distance};
    const double size = settings.cloudSize;
    const double weatherTiles[2] = {kWeatherTile * size, kWeatherTile * kMiddleScale * size};
    const double winds[2] = {1.0, kMiddleWind};
    const double evolution = static_cast<double>(settings.cloudEvolution);
    // Features stay where the noise has them, so the offsets move against the wind, and down for billows that rise.
    for (size_t layer = 0; layer < 2; ++layer) {
        const double shapeTile = weatherTiles[layer] / kShapeTiles[layer];
        const double detailTile = weatherTiles[layer] / kDetailTiles[layer];
        for (size_t axis = 0; axis < 2; ++axis) {
            const double moved = along[axis] * winds[layer];
            m_offsets[layer][axis] -= moved / weatherTiles[layer];
            m_erosion[layer][axis] = Wrap(m_erosion[layer][axis] - moved * (1.0 + kBillowDrift * evolution) / shapeTile);
            m_erosion[layer][2 + axis] = Wrap(m_erosion[layer][2 + axis] - moved * (1.0 + kDetailDrift * evolution) / detailTile);
        }
        WrapHexes(m_offsets[layer], kLayerHexSpacing);
        m_offsets[layer][2] = Wrap(m_offsets[layer][2] - evolution * kEvolutionSpeed * seconds / shapeTile);
    }
    // The cirrus is sampled along the wind (CirrusDepth in shaders/clouds.hlsl).
    m_offsets[2][0] -= distance * kCirrusWind / (kCirrusTile * size);
    WrapHexes(m_offsets[2], kCirrusHexSpacing);

    // The changing weather starts from the chosen one, holds it, then turns into another over a share of its duration.
    if (!settings.changingWeather) {
        m_changing = false;
    } else {
        if (!m_changing) {
            m_from = m_to = settings.weather == Weather::Custom ? Weather::FairWeather : settings.weather;
            m_elapsed = 0.0;
            m_changing = true;
        }
        m_elapsed += seconds;
        const double hold = static_cast<double>(settings.weatherDuration) * 60.0;
        if (m_from == m_to && m_elapsed >= hold) {
            const std::span<const Weather> turns = Turns(m_from);
            m_to = turns[std::uniform_int_distribution<size_t>(0, turns.size() - 1)(m_random)];
            m_elapsed = 0.0;
        } else if (m_from != m_to && m_elapsed >= hold * kChangeShare) {
            m_from = m_to;
            m_elapsed = 0.0;
        }
    }
    return {static_cast<float>(along[0] / kCloudKilometresPerUnit), static_cast<float>(along[1] / kCloudKilometresPerUnit)};
}

WeatherValues CloudWeather::Values(const Settings& settings) const
{
    if (!m_changing)
        return WeatherOf(Weather::Custom, settings);
    const WeatherValues from = WeatherOf(m_from, settings);
    if (m_from == m_to)
        return from;
    const WeatherValues to = WeatherOf(m_to, settings);
    const double share = std::clamp(m_elapsed / (static_cast<double>(settings.weatherDuration) * 60.0 * kChangeShare), 0.0, 1.0);
    const float t = static_cast<float>(share * share * (3.0 - 2.0 * share));
    const auto mix = [t](float a, float b) { return a + (b - a) * t; };
    return {mix(from.coverage, to.coverage), mix(from.type, to.type), mix(from.altitude, to.altitude), mix(from.thickness, to.thickness),
        mix(from.midCoverage, to.midCoverage), mix(from.highCoverage, to.highCoverage), mix(from.highOpacity, to.highOpacity)};
}

void CloudWeather::Write(const Settings& settings, CloudConstants& constants) const
{
    const WeatherValues weather = Values(settings);
    const float ground = kGroundRadius + kEyeHeight;
    const auto layer = [&](CloudLayerConstants& out, size_t index, float altitude, float thickness, float coverage, float type, float extinction,
                           float scale, int steps) {
        out.bottom = ground + altitude * 1e-3f;
        out.top = out.bottom + thickness * 1e-3f;
        out.coverage = coverage;
        out.type = type;
        out.extinction = extinction;
        out.weatherFrequency = 1.0f / (kWeatherTile * scale * settings.cloudSize);
        out.shapeFrequency = kShapeTiles[index] * out.weatherFrequency;
        out.detailFrequency = kDetailTiles[index] * out.weatherFrequency;
        out.erosion = settings.cloudDetail / kDefaultDetail;
        for (size_t i = 0; i < 3; ++i)
            out.offset[i] = static_cast<float>(m_offsets[index][i]);
        for (size_t i = 0; i < 4; ++i)
            out.erosionOffset[i] = static_cast<float>(m_erosion[index][i]);
        out.steps = static_cast<uint32_t>(steps);
        out.field = static_cast<uint32_t>(index);
    };
    layer(constants.layers[0], 0, weather.altitude, weather.thickness, weather.coverage, weather.type, kCumulusExtinction * settings.cloudDensity, 1.0f,
        settings.cloudSteps);
    // The middle layer's type is not used: its clouds are altocumulus (LayerDensity in shaders/clouds.hlsl).
    layer(constants.layers[1], 1, settings.midCloudAltitude, settings.midCloudThickness, weather.midCoverage, 1.0f,
        kCumulusExtinction * kMiddleDensity * settings.cloudDensity, kMiddleScale, std::max(settings.cloudSteps / 2, 8));
    const float angle = settings.windDirection * std::numbers::pi_v<float> / 180.0f;
    constants.cirrusRadius = ground + settings.highCloudAltitude * 1e-3f;
    constants.cirrusCoverage = weather.highCoverage;
    constants.cirrusDepth = weather.highOpacity;
    constants.cirrusFrequency = 1.0f / (kCirrusTile * settings.cloudSize);
    constants.cirrusStrandFrequency = kCirrusStrandTiles * constants.cirrusFrequency;
    constants.cirrusOffset[0] = static_cast<float>(m_offsets[2][0]);
    constants.cirrusOffset[1] = static_cast<float>(m_offsets[2][1]);
    constants.cirrusAxis[0] = std::cos(angle);
    constants.cirrusAxis[1] = std::sin(angle);
    constants.layerMask = (weather.coverage > 0.0f ? kCloudLayerLow : 0u) | (settings.midClouds && weather.midCoverage > 0.0f ? kCloudLayerMiddle : 0u) |
        (settings.highClouds && weather.highCoverage > 0.0f && weather.highOpacity > 0.0f ? kCloudLayerCirrus : 0u);
    const std::array<float, 4> phase = DropletPhase(settings.cloudDropletSize);
    std::copy(phase.begin(), phase.end(), constants.phase);
    constants.octaveScatter = settings.cloudMultipleScattering;
    constants.octaveEccentricity = kOctaveEccentricity;
    constants.octaves = kOctaves;
    constants.lightSteps = static_cast<uint32_t>(settings.cloudLightSteps);
    constants.shadowStrength = settings.cloudShadows ? settings.cloudShadowStrength : 0.0f;
}

}
