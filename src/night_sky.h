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
    float satellites[gbuffer::kMaxSatellites][4];
    float meteorStarts[gbuffer::kMaxMeteors][4];
    float meteorAxes[gbuffer::kMaxMeteors][4];
    float meteorFluxes[gbuffer::kMaxMeteors][4];
};
static_assert(sizeof(NightSkyConstants) == 352);

NightSkyConstants PackNightSky(const gbuffer::SkyDraw& draw);

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
    // Writes `parts` (gbuffer::NightSkyPart), the stars' frame and clock, and the satellites `sun` lights and the meteors
    // in view now, at `meteorFrequency` times the shower's rate, into `draw`.
    void Write(uint32_t parts, const std::array<float, 3>& sun, float meteorFrequency, gbuffer::SkyDraw& draw) const;

private:
    // The stars' frame to the local frame, a unit quaternion (w, x, y, z).
    std::array<double, 4> m_rotation = {1.0, 0.0, 0.0, 0.0};
    std::array<double, 4> m_turn = {1.0, 0.0, 0.0, 0.0};
    std::optional<std::array<double, 3>> m_sun;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

}
