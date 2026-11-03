#include "night_sky.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gmodpp {
namespace {

using Vector = std::array<double, 3>;
using Quaternion = std::array<double, 4>;

// The procedural sky's planet and eye (kGroundRadius and kEyeHeight in shaders/atmosphere.hlsli), in kilometres, and the
// Sun's visual magnitude (kSunMagnitude there), against which the atmosphere's units are counted.
constexpr double kGroundRadius = 6360.0;
constexpr double kEyeHeight = 0.2;
constexpr double kSunMagnitude = -26.74;
constexpr double kFullMoonMagnitude = -12.74;
// Earth's gravitational parameter GM in km^3/s^2.
constexpr double kEarthGm = 398600.4418;

// Satellites in circular orbits among the stars, kSatelliteHeights km up, of standard magnitudes kSatelliteMagnitudes
// (at 1000 km, half lit: phase angle 90 degrees): Starlink's are 5.9, its VisorSats' and OneWeb's 7.2 (Mallama 2021),
// older rocket bodies brighter.
constexpr uint32_t kSatellites = 48;
constexpr double kSatelliteHeights[2] = {450.0, 1200.0};
constexpr double kSatelliteInclinations[2] = {30.0, 100.0};
constexpr double kSatelliteMagnitudes[2] = {4.5, 7.2};

// Meteors of a shower like the Perseids (IMO's working list: zenithal hourly rate 100, population index 2.6):
// kShowerRate an hour with its radiant at the zenith down to magnitude kFaintestMeteor, kPopulationIndex times as many
// each magnitude fainter, fewer as its radiant sinks, none below the horizon. Each lasts kMeteorDurations seconds,
// moving kMeteorSpeeds degrees a second where it is 90 degrees from the radiant (slower closer to it; IMO gives 3 to 25),
// brightest halfway, and leaves a streak of the last kMeteorPersistence seconds of its path.
constexpr double kShowerRate = 100.0;
constexpr double kFaintestMeteor = 6.5;
constexpr double kBrightestMeteor = -4.0;
constexpr double kPopulationIndex = 2.6;
constexpr double kMeteorDurations[2] = {0.2, 1.0};
constexpr double kMeteorSpeeds[2] = {10.0, 25.0};
constexpr double kMeteorPersistence = 0.05;
// Meteors start this high above the horizon at least.
constexpr double kMeteorElevation = 10.0;
// Each kMeteorSlot seconds holds at most one meteor's start.
constexpr double kMeteorSlot = 2.0;
// The shower's radiant in the stars' frame, 60 degrees up while the stars' frame is the local one.
constexpr Vector kRadiant = {0.5, 0.0, 0.8660254};
// Twinkling's clock wraps every hour, which keeps it precise as a float.
constexpr double kStarClockPeriod = 3600.0;

double Dot(const Vector& a, const Vector& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vector Cross(const Vector& a, const Vector& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vector Scale(const Vector& v, double s) { return {v[0] * s, v[1] * s, v[2] * s}; }

Vector Subtract(const Vector& a, const Vector& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

Vector Normalize(const Vector& v) { return Scale(v, 1.0 / std::sqrt(Dot(v, v))); }

Quaternion Multiply(const Quaternion& a, const Quaternion& b)
{
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3], a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
        a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1], a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}

Vector Rotate(const Quaternion& q, const Vector& v)
{
    const Vector axis = {q[1], q[2], q[3]};
    const Vector t = Scale(Cross(axis, v), 2.0);
    const Vector u = Cross(axis, t);
    return {v[0] + q[0] * t[0] + u[0], v[1] + q[0] * t[1] + u[1], v[2] + q[0] * t[2] + u[2]};
}

// The same integer hash as DitherHash in shaders/dither.hlsli, and a number in [0, 1) from it.
uint32_t Hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

double Unit(uint32_t hash) { return static_cast<double>(hash >> 8) / 16777216.0; }

double Lerp(const double (&range)[2], double t) { return range[0] + (range[1] - range[0]) * t; }

double MagnitudeFlux(double magnitude) { return std::pow(10.0, -0.4 * (magnitude - kSunMagnitude)); }

// Whether a ray from `origin` along a unit direction meets the planet.
bool MeetsGround(const Vector& origin, const Vector& direction)
{
    const double b = Dot(origin, direction);
    const double c = Dot(origin, origin) - kGroundRadius * kGroundRadius;
    return b < 0.0 && b * b - c >= 0.0;
}

void WriteSatellites(const Quaternion& frame, const Vector& sun, double seconds, gbuffer::SkyDraw& draw)
{
    const Vector eye = {0.0, 0.0, kGroundRadius + kEyeHeight};
    std::array<std::array<float, 4>, kSatellites> seen = {};
    size_t count = 0;
    for (uint32_t i = 0; i < kSatellites; ++i) {
        uint32_t hash = Hash(i ^ 0x51ed270bu);
        const auto next = [&] { return Unit(hash = Hash(hash)); };
        const double radius = kGroundRadius + Lerp(kSatelliteHeights, next());
        const double inclination = Lerp(kSatelliteInclinations, next()) * std::numbers::pi / 180.0;
        const double node = next() * 2.0 * std::numbers::pi;
        const double angle = next() * 2.0 * std::numbers::pi + std::sqrt(kEarthGm / (radius * radius * radius)) * seconds;
        const double standardMagnitude = Lerp(kSatelliteMagnitudes, next());
        // Along its orbit, tilted about the line of nodes and turned to its node, among the stars.
        const Vector inPlane = {radius * std::cos(angle), radius * std::sin(angle) * std::cos(inclination), radius * std::sin(angle) * std::sin(inclination)};
        const Vector orbit = {inPlane[0] * std::cos(node) - inPlane[1] * std::sin(node), inPlane[0] * std::sin(node) + inPlane[1] * std::cos(node), inPlane[2]};
        const Vector position = Rotate(frame, orbit);
        const Vector toSatellite = Subtract(position, eye);
        // Below the eye's horizon, or in Earth's shadow.
        if (toSatellite[2] <= 0.0 || MeetsGround(position, sun))
            continue;
        const double range = std::sqrt(Dot(toSatellite, toSatellite));
        const Vector direction = Scale(toSatellite, 1.0 / range);
        // A diffuse sphere's brightness at phase angle a (between the sun and the eye, seen from it), relative to a = 90
        // degrees: (sin a + (pi - a) cos a) / pi over 1 / pi.
        const double phase = std::acos(std::clamp(-Dot(sun, direction), -1.0, 1.0));
        const double lit = std::sin(phase) + (std::numbers::pi - phase) * std::cos(phase);
        if (lit <= 0.0)
            continue;
        const double magnitude = standardMagnitude + 5.0 * std::log10(range / 1000.0) - 2.5 * std::log10(lit);
        seen[count++] = {static_cast<float>(direction[0]), static_cast<float>(direction[1]), static_cast<float>(direction[2]),
            static_cast<float>(MagnitudeFlux(magnitude))};
    }
    const size_t shown = std::min(count, gbuffer::kMaxSatellites);
    std::partial_sort(seen.begin(), seen.begin() + shown, seen.begin() + count, [](const auto& a, const auto& b) { return a[3] > b[3]; });
    std::copy_n(seen.begin(), shown, draw.satellites.begin());
    draw.satelliteCount = static_cast<uint32_t>(shown);
}

// `frequency` times the shower's rate: Settings::shootingStarFrequency's range keeps a slot's chance below 1.
void WriteMeteors(const Quaternion& frame, double seconds, double frequency, gbuffer::SkyDraw& draw)
{
    const Vector radiant = Rotate(frame, kRadiant);
    if (radiant[2] <= 0.0)
        return;
    const double chance = kShowerRate * frequency / 3600.0 * kMeteorSlot * radiant[2];
    const int64_t current = static_cast<int64_t>(std::floor(seconds / kMeteorSlot));
    // A meteor that starts late in the last slot may still be crossing the sky.
    for (int64_t slot = std::max<int64_t>(current - 1, 0); slot <= current && draw.meteorCount < gbuffer::kMaxMeteors; ++slot) {
        uint32_t hash = Hash(static_cast<uint32_t>(slot) ^ 0x6c8e9cf5u);
        const auto next = [&] { return Unit(hash = Hash(hash)); };
        if (Unit(hash) >= chance)
            continue;
        const double start = (static_cast<double>(slot) + next()) * kMeteorSlot;
        const double duration = Lerp(kMeteorDurations, next());
        const double t = (seconds - start) / duration;
        const double faintness = std::log(std::max(next(), 1e-9)) / std::log(kPopulationIndex);
        const double magnitude = std::max(kFaintestMeteor + faintness, kBrightestMeteor);
        const double z = std::sin(kMeteorElevation * std::numbers::pi / 180.0) + (1.0 - std::sin(kMeteorElevation * std::numbers::pi / 180.0)) * next();
        const double azimuth = next() * 2.0 * std::numbers::pi;
        const double speed = Lerp(kMeteorSpeeds, next()) * std::numbers::pi / 180.0;
        if (t < 0.0 || t > 1.0)
            continue;
        const double across = std::sqrt(1.0 - z * z);
        const Vector from = {across * std::cos(azimuth), across * std::sin(azimuth), z};
        const Vector away = Cross(radiant, from);
        const double sine = std::sqrt(Dot(away, away));
        if (sine < 1e-3)
            continue;
        // Meteors seen near the radiant come toward the eye and cross less of the sky.
        const double path = speed * sine * duration;
        const double head = path * t;
        gbuffer::Meteor& meteor = draw.meteors[draw.meteorCount++];
        const Vector axis = Scale(away, 1.0 / sine);
        meteor.start = {static_cast<float>(from[0]), static_cast<float>(from[1]), static_cast<float>(from[2])};
        meteor.axis = {static_cast<float>(axis[0]), static_cast<float>(axis[1]), static_cast<float>(axis[2])};
        meteor.head = static_cast<float>(head);
        meteor.tail = static_cast<float>(std::max(head - path / duration * kMeteorPersistence, 0.0));
        meteor.flux = static_cast<float>(MagnitudeFlux(magnitude) * 4.0 * t * (1.0 - t));
    }
}

}

NightSkyConstants PackNightSky(const gbuffer::SkyDraw& draw)
{
    NightSkyConstants constants = {};
    std::copy(draw.sunDirection.begin(), draw.sunDirection.end(), constants.sunDirection);
    constants.sunRadius = draw.sunRadius;
    std::copy(draw.sunColor.begin(), draw.sunColor.end(), constants.sunColor);
    constants.disc = draw.sunDisc ? (draw.discIsMoon ? 2u : 1u) : 0u;
    constants.transmittanceView = draw.transmittanceView;
    constants.parts = draw.nightSky;
    constants.satelliteCount = draw.satelliteCount;
    constants.meteorCount = draw.meteorCount;
    for (size_t row = 0; row < 3; ++row)
        std::copy(draw.starFrame[row].begin(), draw.starFrame[row].end(), constants.starFrame[row]);
    constants.time = draw.starTime;
    constants.milkyWayView = draw.milkyWayView;
    constants.starDensity = draw.starDensity;
    constants.faintestStar = draw.faintestStar;
    constants.starBrightness = draw.starBrightness;
    constants.twinkleStrength = draw.twinkleStrength;
    constants.twinkleSpeed = draw.twinkleSpeed;
    constants.milkyWayBrightness = draw.milkyWayBrightness;
    std::copy(draw.moonDirection.begin(), draw.moonDirection.end(), constants.moonDirection);
    constants.moonIrradiance = draw.moonIrradiance;
    constants.moonScale = draw.moonScale;
    constants.moonlessFloor = draw.moonlessFloor;
    constants.mapLight = draw.mapLight;
    constants.starTwilight = draw.starTwilight;
    std::copy(draw.mapSunDirection.begin(), draw.mapSunDirection.end(), constants.mapSunDirection);
    constants.discRadiance = draw.discRadiance;
    for (size_t i = 0; i < draw.satelliteCount; ++i)
        std::copy(draw.satellites[i].begin(), draw.satellites[i].end(), constants.satellites[i]);
    for (size_t i = 0; i < draw.meteorCount; ++i) {
        const gbuffer::Meteor& meteor = draw.meteors[i];
        std::copy(meteor.start.begin(), meteor.start.end(), constants.meteorStarts[i]);
        constants.meteorStarts[i][3] = meteor.head;
        std::copy(meteor.axis.begin(), meteor.axis.end(), constants.meteorAxes[i]);
        constants.meteorAxes[i][3] = meteor.tail;
        constants.meteorFluxes[i][0] = meteor.flux;
    }
    return constants;
}

float MoonIrradiance(float phase)
{
    const double elongation = std::acos(std::clamp(1.0 - 2.0 * phase, -1.0, 1.0));
    const double phaseAngle = 180.0 - elongation * 180.0 / std::numbers::pi;
    const double magnitude = kFullMoonMagnitude + 0.026 * phaseAngle + 4e-9 * std::pow(phaseAngle, 4.0);
    return static_cast<float>(MagnitudeFlux(magnitude));
}

void NightSky::Reset()
{
    m_rotation = {1.0, 0.0, 0.0, 0.0};
    m_turn = {1.0, 0.0, 0.0, 0.0};
    m_moon.reset();
    m_sun.reset();
    m_start = std::chrono::steady_clock::now();
}

void NightSky::FollowSun(const std::array<float, 3>& sun)
{
    const Vector to = Normalize({sun[0], sun[1], sun[2]});
    m_turn = {1.0, 0.0, 0.0, 0.0};
    if (m_sun) {
        // The shortest turn from `from` to `to`: about their cross product by the angle between them, as the unit
        // quaternion (1 + cos, sin axis) normalized. Opposite directions have no shortest turn; the frame keeps still.
        const Vector& from = *m_sun;
        const Vector axis = Cross(from, to);
        const Quaternion turn = {1.0 + Dot(from, to), axis[0], axis[1], axis[2]};
        const double length = std::sqrt(turn[0] * turn[0] + Dot(axis, axis));
        if (length > 1e-9) {
            m_turn = {turn[0] / length, turn[1] / length, turn[2] / length, turn[3] / length};
            Quaternion rotation = Multiply(m_turn, m_rotation);
            const double norm = std::sqrt(rotation[0] * rotation[0] + rotation[1] * rotation[1] + rotation[2] * rotation[2] + rotation[3] * rotation[3]);
            for (double& component : rotation)
                component /= norm;
            m_rotation = rotation;
        }
    }
    m_sun = to;
}

std::array<std::array<float, 3>, 3> NightSky::TurnBack() const
{
    const Quaternion back = {m_turn[0], -m_turn[1], -m_turn[2], -m_turn[3]};
    std::array<std::array<float, 3>, 3> rows = {};
    for (size_t column = 0; column < 3; ++column) {
        Vector axis = {};
        axis[column] = 1.0;
        const Vector image = Rotate(back, axis);
        for (size_t row = 0; row < 3; ++row)
            rows[row][column] = static_cast<float>(image[row]);
    }
    return rows;
}

float NightSky::TurnAngle() const
{
    return static_cast<float>(2.0 * std::acos(std::clamp(m_turn[0], -1.0, 1.0)));
}

void NightSky::PlaceMoon(const std::array<float, 3>& sun, float phase)
{
    if (m_moon && std::abs(phase - m_moonPhase) < 1e-5f)
        return;
    const Vector toSun = Normalize({sun[0], sun[1], sun[2]});
    const double across = std::sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1]);
    const Vector side = across > 1e-6 ? Vector{toSun[0] / across, toSun[1] / across, 0.0} : Vector{1.0, 0.0, 0.0};
    const double angle = std::asin(std::clamp(toSun[2], -1.0, 1.0)) + std::acos(std::clamp(1.0 - 2.0 * phase, -1.0, 1.0));
    const Vector local = {side[0] * std::cos(angle), side[1] * std::cos(angle), std::sin(angle)};
    m_moon = Rotate({m_rotation[0], -m_rotation[1], -m_rotation[2], -m_rotation[3]}, local);
    m_moonPhase = phase;
}

std::optional<std::array<float, 3>> NightSky::Moon() const
{
    if (!m_moon)
        return std::nullopt;
    const Vector moon = Rotate(m_rotation, *m_moon);
    return std::array<float, 3>{static_cast<float>(moon[0]), static_cast<float>(moon[1]), static_cast<float>(moon[2])};
}

void NightSky::Write(uint32_t parts, const std::array<float, 3>& sun, float meteorFrequency, gbuffer::SkyDraw& draw) const
{
    draw.nightSky = parts;
    // The shaders turn local directions into the stars' frame by the inverse turn, whose rows are the images of the stars'
    // axes.
    for (size_t row = 0; row < 3; ++row) {
        Vector axis = {};
        axis[row] = 1.0;
        const Vector image = Rotate(m_rotation, axis);
        for (size_t column = 0; column < 3; ++column)
            draw.starFrame[row][column] = static_cast<float>(image[column]);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    draw.starTime = static_cast<float>(std::fmod(seconds, kStarClockPeriod));
    draw.satelliteCount = 0;
    draw.meteorCount = 0;
    const Vector sunDirection = Normalize({sun[0], sun[1], sun[2]});
    if (parts & gbuffer::kNightSatellites)
        WriteSatellites(m_rotation, sunDirection, seconds, draw);
    if (parts & gbuffer::kNightMeteors)
        WriteMeteors(m_rotation, seconds, meteorFrequency, draw);
}

}
