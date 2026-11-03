#pragma once

#include "settings.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <utility>

namespace gmodpp {

// Mirrors CloudLayer in shaders/clouds.hlsl.
struct CloudLayerConstants {
    float bottom;
    float top;
    float coverage;
    float type;
    float extinction;
    float shapeFrequency;
    float detailFrequency;
    float weatherFrequency;
    float erosion;
    float offset[3];
    float erosionOffset[4];
    uint32_t steps;
    uint32_t field;
    float padding[2];
};
static_assert(sizeof(CloudLayerConstants) == 80);

// Mirrors CloudConstants in shaders/clouds.hlsl.
struct CloudConstants {
    CloudLayerConstants layers[2];
    float cirrusRadius;
    float cirrusCoverage;
    float cirrusDepth;
    float cirrusFrequency;
    float cirrusOffset[2];
    float cirrusAxis[2];
    uint32_t layerMask;
    float lightDirection[3];
    float lightScale;
    float lightTint[3];
    float phase[4];
    float octaveScatter;
    float octaveEccentricity;
    uint32_t octaves;
    uint32_t lightSteps;
    uint32_t shapeNoiseView;
    uint32_t detailNoiseView;
    uint32_t weatherView;
    uint32_t transmittanceView;
    uint32_t skyView;
    uint32_t bandsView;
    float shadowStrength;
    uint32_t cubeTexels;
    float viewProj[4][4];
    float prevViewProj[4][4];
    float clipToDirection[3][4];
    uint32_t historySize[2];
    float overscan;
    uint32_t historyValid;
    uint32_t traceOffset[2];
    float windStep[2];
    uint32_t cirrusView;
    float cirrusStrandFrequency;
    uint32_t shapesView;
    float padding;
};
static_assert(sizeof(CloudConstants) == 512);

// What CloudConstants::layerMask draws (kLayer... in shaders/clouds.hlsl).
inline constexpr uint32_t kCloudLayerLow = 1;
inline constexpr uint32_t kCloudLayerMiddle = 2;
inline constexpr uint32_t kCloudLayerCirrus = 4;

// Source's units are inches (Valve Developer Community, "Dimensions"): kilometres per unit (kKilometresPerUnit in
// shaders/clouds.hlsl).
inline constexpr float kCloudKilometresPerUnit = 2.54e-5f;

// Where the clouds have drifted: each layer's wind offset in its weather tiles, how far its billows have risen in shape
// tiles and how far its erosion noises have drifted ahead of the wind, all wrapped; and, with Settings::changingWeather,
// the weather, which
// holds for about Settings::weatherDuration and then turns into one of those that can follow it, picked by chance (as
// Nubis' weather scheduler does, Schneider and Vos, SIGGRAPH 2017). All moved on by a clock of their own.
class CloudWeather {
public:
    // Moves the clouds and the weather on by the time since the last call (at most a quarter of a second) with the
    // settings' wind. Returns how far the low layer moved, in the map's units.
    std::array<float, 2> Advance(const Settings& settings);
    // The settings' layers, cirrus and droplets in `constants` (the changing weather's layers while it changes), drifted
    // as they are now.
    void Write(const Settings& settings, CloudConstants& constants) const;
    // The changing weather: the weather it shows and the one it turns to (the same while it holds).
    std::pair<Weather, Weather> Now() const { return {m_from, m_to}; }

private:
    WeatherValues Values(const Settings& settings) const;

    // The low layer, the middle one and the cirrus: xy the wind's offset, z the shapes' evolution.
    std::array<std::array<double, 3>, 3> m_offsets = {};
    // The low and middle layers' erosion noises: the billows' drift (xy, shape tiles) and the fine detail's (zw, detail
    // tiles).
    std::array<std::array<double, 4>, 2> m_erosion = {};
    std::optional<std::chrono::steady_clock::time_point> m_last;
    // The changing weather, from the weather it held to the one it turns to (the same while it holds), and the seconds
    // it has held or turned for; m_changing once it has started.
    bool m_changing = false;
    Weather m_from = Weather::FairWeather;
    Weather m_to = Weather::FairWeather;
    double m_elapsed = 0.0;
    std::mt19937 m_random{std::random_device{}()};
};

}
