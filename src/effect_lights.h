#pragma once

#include "bsp.h"
#include "engine_interfaces.h"
#include "scene.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace gmodpp::effect_lights {

// Light from effects (Settings::effectLights): the engine's elights, which Source applies to models alone (HL2's muzzle
// flashes), the glowing particle systems the game simulated this frame, and the glow it drew last frame (glow_draws).
// They are flashes: lit exactly and directly, never kept in any history (Scene::SetDynamicLights).

// An engine light (dlight or elight) as a dynamic light, its colour times `strength`.
FrameLight FromEngineLight(const engine::DynamicLight& light, float strength);

// What the main view's effect lights are gathered for: the eye, the engine's dlights, the map's lights (bsp::Light::origin)
// and where the local player stands.
struct Context {
    std::array<float, 3> eye;
    std::span<const engine::DynamicLight> engineLights;
    std::span<const bsp::Float3> mapLights;
    std::optional<std::array<float, 3>> localPlayer;
};

// Appends the main view's flashes of effects to `lights`, at most `limit`, those that matter most from the eye first: the
// engine's elights, glowing particle systems, and the glow drawn last frame (glow_draws). Call once per main view.
void Collect(const Context& context, size_t limit, std::vector<FrameLight>& lights);

}
