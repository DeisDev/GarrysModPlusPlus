#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gmodpp::engine {

// Resolves the engine and filesystem interfaces once their modules are loaded; safe to call every frame.
void TryInitialize();
bool Ready();

// Current map path as reported by the engine, e.g. "maps/gm_construct.bsp"; empty when no map is loaded.
std::string LevelName();

// Entity index of the local player, or 0 when the engine is unavailable.
int LocalPlayerIndex();

// Queues console commands as if typed into the console.
void ExecuteCommand(const std::string& command);

// Reads a file through the game's filesystem (loose files, mounted content and addons), at most `maxBytes`.
bool ReadGameFile(const std::string& path, std::vector<std::byte>& data, size_t maxBytes = SIZE_MAX);

// An engine dynamic light (dlight_t): muzzle flashes, explosions, the light tool. Colour is linear, in
// lightmap units; a zero outer angle means a point light.
struct DynamicLight {
    float origin[3];
    float radius;
    float color[3];
    float direction[3];
    float innerAngle;
    float outerAngle;
    int key;
};

// The engine's active dynamic lights (IVEfx::CL_GetActiveDLights).
void ActiveDynamicLights(std::vector<DynamicLight>& lights);

// Sets INT_RENDERPARM_ENABLE_FIXED_LIGHTING, which selects the stock shaders' LIGHTING_PREVIEW combos
// (2 = G-buffer outputs). Returns false when the material system is unavailable.
bool SetFixedLighting(int mode);

}
