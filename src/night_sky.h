#pragma once

#include "view_types.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace gmodpp {

// What the sky shows beyond its atmosphere on the GPU: the night sky (SkyDraw::nightSky and on) and the sun's or Moon's
// disc. Mirrors NightSkyConstants in shaders/night_sky.hlsli.
struct NightSkyConstants {
    float sunDirection[3];
    float sunRadius;
    float sunColor[3];
    // 0 none, 1 the Sun, 2 the Moon.
    uint32_t disc;
    uint32_t transmittanceView;
    uint32_t parts;
    uint32_t satelliteCount;
    uint32_t meteorCount;
    float starFrame[3][4];
    float time;
    uint32_t milkyWayView;
    float starDensity;
    float faintestStar;
    float starBrightness;
    float twinkleStrength;
    float twinkleSpeed;
    float milkyWayBrightness;
    float moonDirection[3];
    float moonIrradiance;
    float moonScale;
    float moonlessFloor;
    float mapLight;
    float starTwilight;
    float mapSunDirection[3];
    float discRadiance;
    float satellites[gbuffer::kMaxSatellites][4];
    float meteorStarts[gbuffer::kMaxMeteors][4];
    float meteorAxes[gbuffer::kMaxMeteors][4];
    float meteorFluxes[gbuffer::kMaxMeteors][4];
};
static_assert(sizeof(NightSkyConstants) == 400);

NightSkyConstants PackNightSky(const gbuffer::SkyDraw& draw);

// The Moon's irradiance in the atmosphere's units (sunlight of irradiance 1) at a phase, the share of its disc lit (1
// full): the full Moon's mean magnitude at opposition, -12.74 (NASA's Moon fact sheet), fainter by 0.026 |a| + 4e-9 a^4
// magnitudes at phase angle a in degrees (Allen's Astrophysical Quantities).
float MoonIrradiance(float phase);

// The moving parts of the night sky over the procedural sky (shaders/night_sky.hlsli): the stars' frame, which turns
// with the sun as Earth's turn carries the stars and the sun together, and the satellites and meteors in view.
class NightSky {
public:
    // A new map: the stars' frame is the local one again and the clock starts over.
    void Reset();
    // Turns the stars' frame with the sun from where it last was to `sun` (a unit direction) the shortest way, which for a
    // sun moving along a great circle is the sky's turn about that circle's axis.
    void FollowSun(const std::array<float, 3>& sun);
    // The last FollowSun's turn back: the rows of the turn from this frame's directions to last frame's (the identity
    // when it did not turn), and the angle it turned by in radians.
    std::array<std::array<float, 3>, 3> TurnBack() const;
    float TurnAngle() const;
    // Places the Moon of day maps among the stars at `phase` (the share of its disc lit, 1 full) from the sun, when it
    // has no place yet or the phase changed: along the sun's vertical circle, past the zenith away from the sun's side of
    // the sky by its elongation, so a full Moon is opposite the sun and a first quarter at the zenith while the sun sets.
    // From then on it turns with the stars, keeping its angle from the sun.
    void PlaceMoon(const std::array<float, 3>& sun, float phase);
    // The Moon's direction now, once placed.
    std::optional<std::array<float, 3>> Moon() const;
    // Writes `parts` (gbuffer::NightSkyPart), the stars' frame and clock, and the satellites `sun` lights and the meteors
    // in view now, at `meteorFrequency` times the shower's rate, into `draw`.
    void Write(uint32_t parts, const std::array<float, 3>& sun, float meteorFrequency, gbuffer::SkyDraw& draw) const;

private:
    // The stars' frame to the local frame, a unit quaternion (w, x, y, z).
    std::array<double, 4> m_rotation = {1.0, 0.0, 0.0, 0.0};
    std::array<double, 4> m_turn = {1.0, 0.0, 0.0, 0.0};
    // The Moon's direction in the stars' frame, and the phase it was placed at.
    std::optional<std::array<double, 3>> m_moon;
    float m_moonPhase = 0.0f;
    std::optional<std::array<double, 3>> m_sun;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

}
