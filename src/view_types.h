#pragma once

#include <windows.h>

#include <array>
#include <cstdint>

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
    // Debug views show the light alone, as if the albedo were white.
    bool lightOnly;
    // Native forward twins (shaders/forward_twin.hlsli): the GPU addresses of their frame constants and ray-traced
    // scene, 0 for none.
    std::uint64_t twinFrame;
    std::uint64_t twinScene;
};

}
