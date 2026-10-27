#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace gmodpp {

enum class DebugView : uint32_t {
    Final = 0,
    Albedo = 1,
    Normals = 2,
    Coverage = 3,
    DirectLight = 4,
    IndirectLight = 5,
    IndirectRaw = 6,
    SunVisibility = 7,
    GameFlashlight = 8,
    Reflections = 9,
    MotionVectors = 10,
    AmbientOcclusion = 11,
    Count
};

enum class FlashlightMode : uint32_t { RayTraced = 0, Game = 1 };

enum class AntiAliasing : uint32_t { Off = 0, Temporal = 1, RayReconstruction = 2, Count };

// DLSS quality modes, from native resolution (DLAA) to the lowest render resolution.
enum class DlssMode : uint32_t { Dlaa = 0, Quality, Balanced, Performance, UltraPerformance, Count };

// Ray tracing quality presets; Custom keeps the detail settings as they are.
enum class QualityPreset : uint32_t { Low = 0, Medium, High, Ultra, Custom, Count };

// What static props the map marks as casting no shadows (mostly foliage) block: nothing, as in the game's baked
// lighting; the sun; or the sun and the first bounce of indirect light. Later bounces always pass through them.
enum class FoliageShadows : uint32_t { Off = 0, Sun, SunAndIndirect, Count };

// Options shown in the in-game menu. Read on the render thread; the menu edits them on the same thread.
struct Settings {
    bool realtimeLighting = true;
    DebugView debugView = DebugView::Final;
    bool sun = true;
    bool mapLights = true;
    bool dynamicLights = true;
    bool projectedLights = true;
    bool indirectLight = true;
    bool denoiser = true;
    bool ambientOcclusion = true;
    bool reflections = true;
    // Translucent surfaces lit from the ray-traced irradiance volume instead of baked light.
    bool translucentLighting = true;
    bool volumetricLight = true;
    bool fog = true;
    FlashlightMode flashlightMode = FlashlightMode::RayTraced;
    float sunIntensity = 1.0f;
    float skyIntensity = 1.0f;
    float lightIntensity = 1.0f;
    float indirectIntensity = 1.0f;
    float projectedIntensity = 1.0f;
    float reflectionIntensity = 1.0f;
    // How much ambient occlusion darkens indirect light, which already holds the larger-scale occlusion.
    float aoIntensity = 1.0f;
    // Scales the haze's density.
    float volumetricIntensity = 1.0f;
    AntiAliasing antiAliasing = AntiAliasing::Temporal;
    DlssMode dlssMode = DlssMode::Quality;
    // Share of the reprojected history kept each frame; higher is smoother but slower to react.
    float taaHistoryWeight = 0.9f;
    float sharpening = 0.3f;

    // Ray tracing quality (ApplyPreset); the defaults are QualityPreset::High.
    QualityPreset qualityPreset = QualityPreset::High;
    // Local lights per pixel whose shadows are traced exactly; the others share sampledLightRays shadow rays.
    int exactLights = 4;
    int sampledLightRays = 2;
    int indirectRays = 1;
    // Diffuse bounces traced from surfaces off screen; on-screen hits reuse last frame's light.
    int indirectBounces = 2;
    int aoRays = 1;
    // Rays per update of each irradiance volume probe.
    int volumeRays = 32;
    int volumetricSteps = 16;
    FoliageShadows foliageShadows = FoliageShadows::SunAndIndirect;
    // Indirect light and ambient occlusion rays take alpha-tested surfaces from their opacity micromaps alone.
    bool fastAlphaTests = false;
    bool opacityMicromaps = true;

    bool operator==(const Settings&) const = default;
};

// Sets the ray tracing detail settings to a preset's values; QualityPreset::Custom leaves them.
void ApplyPreset(Settings& settings, QualityPreset preset);

Settings& GetSettings();

// The range the menu's slider offers for a float setting; values read from the settings file are clamped to it.
struct SettingRange {
    float minimum;
    float maximum;
};
SettingRange RangeOf(float Settings::* field);
SettingRange RangeOf(int Settings::* field);

// Plain key=value text; unknown keys are ignored and missing ones keep their current value.
bool LoadSettings(const std::filesystem::path& path, Settings& settings);
void SaveSettings(const std::filesystem::path& path, const Settings& settings);

// Saves pending menu changes, or reloads the file when something else changed it. Call periodically.
void SyncSettingsFile(const std::filesystem::path& path);
void MarkSettingsChanged();

enum class GpuPass : uint32_t { SceneUpdate, Surface, Volume, DirectLight, AmbientOcclusion, IndirectTrace, Accumulation, Denoiser, Reflections, Lighting, Resolve, Count };

// Figures the renderer reports for the menu.
struct Statistics {
    static constexpr size_t kFrameHistory = 1000;

    // Wall-clock time between presents, oldest first once the ring has wrapped.
    std::array<float, kFrameHistory> frameMs = {};
    size_t frameCount = 0;
    size_t nextFrame = 0;
    double lightingMs = 0.0;
    std::array<double, static_cast<size_t>(GpuPass::Count)> passMs = {};
    uint32_t mapLights = 0;
    uint32_t dynamicLights = 0;
    uint32_t projectedLights = 0;
    uint32_t sceneInstances = 0;
    bool rayReconstructionAvailable = false;
    // Resolution the ray-traced passes ran at in the last main view.
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t gbufferShaders = 0;
    uint32_t replacedShaders = 0;
    bool opacityMicromapsAvailable = false;
    // Alpha-tested and translucent triangles with an opacity micromap, and the micromaps they share.
    uint32_t micromapTriangles = 0;
    uint32_t micromaps = 0;
};

Statistics& GetStatistics();
void RecordFrameTime(float milliseconds);

}
