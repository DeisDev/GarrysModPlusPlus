#pragma once

#include <windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace gmodpp::gbuffer {

// The views captured into the G-buffer: the main view, the 3D skybox (drawn before it) and the viewmodel.
enum class ViewKind { Main, Skybox, Viewmodel };

using Matrix4 = std::array<std::array<float, 4>, 4>;

// A flashlight or projected texture (env_projectedtexture, GMod lamps) as its shader pass received it, in
// Source's DoFlashlight terms (common_flashlight_fxc.h). texture = dot(row, (position, 1)) for each row.
struct ProjectedLight {
    std::array<std::array<float, 4>, 4> worldToTexture;
    std::array<float, 3> origin;
    float farZ;
    std::array<float, 3> color;
    std::array<float, 3> attenuation;
    // The game reads the cookie as sRGB (D3DSAMP_SRGBTEXTURE).
    bool cookieSrgb = false;
};

// The seconds the water's normal maps animate by (FrameConstants::waterTime): a steady clock that wraps every hour, which
// keeps float time precise.
inline float WaterClock()
{
    return static_cast<float>(std::fmod(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), 3600.0));
}

// The most water surfaces whose draws are kept (WaterSurfaceDraw).
inline constexpr size_t kMaxWaterDraws = 8;

// A water surface as the game last drew it: its fog colour (Water's pixel c6, its material's linear $fogcolor, which
// names its body of water), the vertex shader's bump transform rows (cBumpTexCoordTransform, applied to (u, v, 0, 1)),
// and when (WaterClock). Its normal map frame is ViewInputs::waterNormalMaps' entry.
struct WaterSurfaceDraw {
    std::array<float, 3> fog;
    std::array<std::array<float, 4>, 2> bumpRows;
    float time = 0.0f;
    // Its texture coordinates at a world point, (dot(uvS.xyz, p) + uvS.w, the same with uvT), as its vertices hold
    // them; `mapped` once they were read.
    std::array<float, 4> uvS = {};
    std::array<float, 4> uvT = {};
    bool mapped = false;
};

// The time and wind the game last drew a swaying ($treesway) model with, and when (QueryPerformanceCounter ticks).
struct SwayClock {
    float time = 0.0f;
    float wind[2] = {};
    int64_t ticks = 0;
    bool known = false;
};

// Source's range fog of the view's last opaque pass, in its pixel shaders' terms: params are g_FogParams (x start
// over range, z max density, w 1 / range), color is g_LinearFogColor.
struct Fog {
    std::array<float, 4> params = {};
    std::array<float, 3> color = {};
    bool known = false;
};

// env_sun as the client has it (C_Sun's DT_Sun, entities.cpp): the direction to the sun, whether it is on, its colour
// with the largest component 1 (m_clrRender, as C_Sun::OnDataChanged tints its sprite), and its sprite's half size
// (m_nSize).
struct EnvSun {
    std::array<float, 3> direction;
    bool on = false;
    std::array<float, 3> color = {1.0f, 1.0f, 1.0f};
    float size = 0.0f;
};

// What the night sky over the procedural sky shows (SkyDraw::nightSky; kNightStars and on in shaders/sky_draw.hlsl).
enum NightSkyPart : uint32_t { kNightStars = 1, kNightMilkyWay = 2, kNightSatellites = 4, kNightMeteors = 8 };
inline constexpr size_t kMaxSatellites = 8;
inline constexpr size_t kMaxMeteors = 2;

// A meteor's streak (MeteorStreak in shaders/night_sky.hlsli): along the great circle from the direction it started
// from around `axis`, from its tail's angle to its head's, and its irradiance in the atmosphere's units.
struct Meteor {
    std::array<float, 3> start = {};
    std::array<float, 3> axis = {};
    float head = 0.0f;
    float tail = 0.0f;
    float flux = 0.0f;
};

// What the game's 2D sky draws show in place of the game's (shaders/sky_draw.hlsl), as the last main view's lighting
// made it: the procedural sky (`sky`, the cube skyView) and the sun's disc (`sunDisc`), which replaces env_sun's sprite.
struct SkyDraw {
    bool sky = false;
    bool sunDisc = false;
    // The disc is the Moon: the map's sun is the Moon (Settings::mapSun).
    bool discIsMoon = false;
    UINT skyView = 0;
    UINT transmittanceView = 0;
    std::array<float, 3> sunDirection = {};
    std::array<float, 3> sunColor = {1.0f, 1.0f, 1.0f};
    // The disc's angular radius in radians.
    float sunRadius = 0.0f;
    // The direction env_sun's sprites are drawn toward (EnvSun::direction), hidden while the disc is drawn; nothing without
    // an env_sun.
    std::optional<std::array<float, 3>> spriteDirection;
    // The night sky over the procedural sky (NightSkyPart bits), the local frame to the stars' frame (its rows), the clock
    // stars twinkle by in seconds, and the satellites (direction, irradiance) and meteors in view.
    uint32_t nightSky = 0;
    std::array<std::array<float, 3>, 3> starFrame = {{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    float starTime = 0.0f;
    // The Milky Way's starlight, a cube in the stars' frame.
    UINT milkyWayView = 0;
    // How the stars look (NightSkyConstants in shaders/night_sky.hlsli): times Earth's count, the faintest magnitude
    // drawn, times their brightness, how strongly (0 for none) and fast they twinkle; and times the Milky Way's brightness.
    float starDensity = 1.0f;
    float faintestStar = 6.5f;
    float starBrightness = 1.0f;
    float twinkleStrength = 1.0f;
    float twinkleSpeed = 1.0f;
    float milkyWayBrightness = 1.0f;
    // The Moon of day maps (NightSkyConstants): its direction and irradiance (0 without one), the share of the sky its
    // light shows, the moonless night's floor, and the map's own sun's irradiance in the atmosphere's units.
    std::array<float, 3> moonDirection = {};
    float moonIrradiance = 0.0f;
    float moonScale = 0.0f;
    float moonlessFloor = 0.0f;
    float mapLight = 1.0f;
    // How much darker the sun's sky counts when the night sky is drawn against it (Settings::earlierStars).
    float starTwilight = 1.0f;
    // The map's sun (light_environment's).
    std::array<float, 3> mapSunDirection = {};
    std::array<std::array<float, 4>, kMaxSatellites> satellites = {};
    uint32_t satelliteCount = 0;
    std::array<Meteor, kMaxMeteors> meteors = {};
    uint32_t meteorCount = 0;
    // The disc's radiance in the procedural sky's units, which tells how much of it shows through clouds.
    float discRadiance = kUnknownDiscRadiance;
    // The clouds over the sky (shaders/clouds.hlsl): the last main view's history, the rows from a direction to its clip
    // space and its overscan, and the cube of them for other directions; kNoCloudView without clouds.
    UINT cloudHistoryView = kNoCloudView;
    UINT cloudCubeView = kNoCloudView;
    std::array<std::array<float, 4>, 4> cloudViewProj = {};
    float cloudOverscan = 0.0f;
    // Aerial perspective's change of the sky's light by the cloud shadows over the same widened view (Lighting::
    // RecordAerial), or kNoCloudView.
    UINT aerialSkyView = kNoCloudView;

    static constexpr UINT kNoCloudView = 0xFFFFFFFFu;
    // A disc whose radiance is not known yet shows through anything but opaque clouds.
    static constexpr float kUnknownDiscRadiance = 1e30f;
};

// The irradiance volume that lights what the game draws after the capture (shaders/probes.hlsl): kProbeGrid
// probes kProbeSpacing apart around the camera, as three volume textures holding a + b.n for red, green and
// blue.
inline constexpr UINT kProbeGrid[3] = {32, 32, 16};
inline constexpr float kProbeSpacing = 64.0f;

// What the forward-lit shaders read (shaders/forward_light.hlsli).
struct ForwardConstants {
    std::array<float, 3> eye;
    float tonemapScale;
    // The volume's first cell.
    std::array<int, 3> probeOrigin;
    // Unknown fog means none.
    Fog fog;
    // Lighting::LitFogScale: the fog's colour is scaled by the volume's average irradiance times this, or kept with 0.
    float litFogScale;
    // Lighting::FogDaylight: the fog's colour lit as the light is now.
    std::array<float, 3> fogDaylight;
    // Debug views show the light alone, as if the albedo were white.
    bool lightOnly;
    // Native forward twins (shaders/forward_twin.hlsli): the GPU addresses of their frame constants and ray-traced
    // scene, 0 for none.
    std::uint64_t twinFrame;
    std::uint64_t twinScene;
    // The main view's frame constants (Lighting::ForwardFrame) with its froxel volume, which the game's shaders read
    // their fog from (shaders/game_fog.hlsli), or 0.
    std::uint64_t fogFrame;
    // For the 3D skybox's draws: the light of open air, the same everywhere, in the probes' form (per colour a and b, the
    // irradiance a + b.n), in place of the irradiance volume, which holds the map's light around the eye and none of the
    // distant land the skybox stands for. They lie under the main view's jittered image, so they take its jitter.
    bool openAir = false;
    std::array<std::array<float, 4>, 3> openAirLight = {};
    bool jittered = false;
};

}
