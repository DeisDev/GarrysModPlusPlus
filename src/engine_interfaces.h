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
// Whether the client has fully joined the map (signon state FULL).
bool IsInGame();

// Entity index of the local player, or 0 when the engine is unavailable.
int LocalPlayerIndex();

// Queues console commands as if typed into the console.
void ExecuteCommand(const std::string& command);

// GMod's language, the gmod_language console variable ("en", "de", "pt-br": the folder names of
// garrysmod/resource/localization); empty when it is unset or can't be read.
std::string GameLanguage();

// A console variable's value as text (ConVar::GetString), or nothing when the console is not up yet, the variable does
// not exist or is never read as a string.
std::optional<std::string> ConVarString(const char* name);

// GMod's frame rate cap while its window is not focused (fps_max_nofocus, 20 by default), each frame: false sets it to
// 0, true puts back the value it had. Only changes made here are undone; until the first change it is left alone.
void CapUnfocusedFrames(bool cap);

// Reads a file through the game's filesystem (loose files, mounted content and addons), at most `maxBytes`.
bool ReadGameFile(const std::string& path, std::vector<std::byte>& data, size_t maxBytes = SIZE_MAX);

// dlight_t::flags (the SDK's public/dlight.h): the light leaves out lightmapped surfaces, or models.
inline constexpr uint32_t kDlightNoWorld = 0x1;
inline constexpr uint32_t kDlightNoModel = 0x2;

// An engine dynamic light (dlight_t): explosions, the light tool, Lua's DynamicLight. Colour is linear, in lightmap
// units; a zero outer angle means a point light. `identity` is its slot in the engine's list, which it keeps while it
// lives.
struct DynamicLight {
    float origin[3];
    float radius;
    float color[3];
    float direction[3];
    float innerAngle;
    float outerAngle;
    int key;
    uint32_t flags;
    uint64_t identity;
};

// The engine's active dynamic lights (IVEfx::CL_GetActiveDLights).
void ActiveDynamicLights(std::vector<DynamicLight>& lights);
// The engine's live entity lights (elights), which Source applies to models alone: HL2's muzzle flashes and some impact
// effects.
void ActiveEntityLights(std::vector<DynamicLight>& lights);

// Sets the ConVar gmodpp_lights, which client Lua reads to know the mod lights the scene (1) or not (0). It is registered
// with the client's ConVars the first time this is called after the client registered its own; never archived.
void PublishLightsActive(bool active);

// IVEngineClient::ComputeLighting(point, normal or null, clamp to 1, colour out, ambient cube out or null), which
// GetLightForPoint also calls through the vtable; particles and client effects light themselves with it.
using ComputeLightingFn = void(__fastcall*)(void* engine, const float* point, const float* normal, bool clamp, float* color, float* boxColors);

// The engine's ComputeLighting, or null before the engine is ready.
ComputeLightingFn ComputeLightingFunction();

// Whether a material draws unlit (UnlitGeneric, UnlitTwoTexture, MonitorScreen), whose preview combos leave out vertex
// colour.
bool IsUnlitMaterial(void* material);
// IMaterial::IsTranslucent: whether the material blends as it is drawn now (its render state or alpha modulation).
bool IsTranslucentMaterial(void* material);

// A material's vector variable as it is now ("$selfillumtint", which proxies set as entities are drawn), or nothing when
// the material or variable does not exist. `material` is its name ("models/weapons/w_physics"). Variables are found once;
// ForgetMaterials drops them when materials may have been unloaded (a map change).
std::optional<std::array<float, 3>> MaterialVector(const std::string& material, const char* variable);
void ForgetMaterials();

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
