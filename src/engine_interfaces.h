#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

// IVEngineClient::ComputeLighting(point, normal or null, clamp to 1, colour out, ambient cube out or null), which
// GetLightForPoint also calls through the vtable; particles and client effects light themselves with it.
using ComputeLightingFn = void(__fastcall*)(void* engine, const float* point, const float* normal, bool clamp, float* color, float* boxColors);

// The engine's ComputeLighting, or null before the engine is ready.
ComputeLightingFn ComputeLightingFunction();

// Whether a material draws unlit (UnlitGeneric, UnlitTwoTexture, MonitorScreen), whose preview combos leave out vertex
// colour.
bool IsUnlitMaterial(void* material);

// The fog of the view being drawn (IMatRenderContext::GetFogMode and GetFogColor): its MaterialFogMode_t (0 without fog)
// and its colour as the game sets it (gamma bytes).
struct FogState {
    int mode = 0;
    std::array<unsigned char, 3> color = {};
};
std::optional<FogState> CurrentFog();

// Light styles (switchable and animated lights): a map light of style s < kLightStyles shines at the style's current
// value times its baked intensity, 1 for a lit switchable light, 0 for a dark one.
inline constexpr size_t kLightStyles = 128;

// The engine's current light style values; false when they could not be found.
bool LightStyleValues(std::array<float, kLightStyles>& values);

}
