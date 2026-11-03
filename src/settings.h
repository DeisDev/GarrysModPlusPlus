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
    AdditiveLight = 8,
    Reflections = 9,
    MotionVectors = 10,
    AmbientOcclusion = 11,
    // The radiance cache's voxels (shaders/deferred_lighting.hlsl).
    RadianceCache = 12,
    // RTXDI's sample per pixel (shaders/restir.hlsl): its light as a colour, and its reservoir's sample count.
    LightSample = 13,
    ReservoirM = 14,
    // NRD's validation overlay over the image (CommonSettings::enableValidation).
    DenoiserValidation = 15,
    Count
};

enum class AntiAliasing : uint32_t { Off = 0, Temporal = 1, RayReconstruction = 2, Count };

// Upscaling modes of the anti-aliasing upscaler (DLSS today), from native resolution (DLAA) to the lowest render
// resolution.
enum class UpscalingMode : uint32_t { Native = 0, Quality, Balanced, Performance, UltraPerformance, Count };

// Ray tracing quality presets; Custom keeps the detail settings as they are.
enum class QualityPreset : uint32_t { Low = 0, Medium, High, Ultra, Custom, Count };

// What static props the map marks as casting no shadows (mostly foliage) block: nothing, as in the game's baked
// lighting; the sun; the sun and the first bounce of indirect light; or all light. The lower levels are faster in grass.
enum class FoliageShadows : uint32_t { Off = 0, Sun, SunAndIndirect, All, Count };

// Where indirect light comes from: traced bounces, or bounce light baked into the map's lightmaps with the same light
// model while the map loads (cheaper; the sun and local lights are still traced).
enum class IndirectLighting : uint32_t { RayTraced = 0, Baked, Count };

// What self-illuminated surfaces light: nothing; what indirect rays meet (bounce light); or also the surfaces around
// them directly, as area lights with shadows.
enum class EmissiveLighting : uint32_t { Off = 0, Bounce, Full, Count };

// What the map's sun (light_environment, and env_sun where the map has one) is: the Sun, or on night maps the Moon, whose
// sky shows the stars. Auto takes it for the Moon while the map's own sky looks dark on screen.
enum class MapSun : uint32_t { Auto = 0, Sun, Moon, Count };

// How the main view's local lights are sampled: the light grid's strongest lights exactly and the rest sampled
// (Legacy); NVIDIA RTXDI's importance sampling from its ReGIR light grid (RIS); the same with samples reused across
// frames and neighbours (ReSTIR DI); or Auto: reused samples with TAA, importance sampling with DLSS Ray Reconstruction
// (as RTXPT pairs them).
enum class LightSampling : uint32_t { Legacy = 0, Ris, Restir, Auto, Count };

// What denoises the main view's indirect light and reflections without DLSS Ray Reconstruction: the built-in filters,
// or NVIDIA NRD's RELAX or REBLUR.
enum class DenoiserType : uint32_t { BuiltIn = 0, Relax, Reblur, Count };

// Options shown in the in-game menu. Read on the render thread; the menu edits them on the same thread.
struct Settings {
    bool realtimeLighting = true;
    DebugView debugView = DebugView::Final;
    bool sun = true;
    // The sun shines from env_sun's direction, which the sun editor turns, instead of light_environment's.
    bool sunFollowsEnvSun = true;
    bool mapLights = true;
    bool dynamicLights = true;
    bool projectedLights = true;
    bool indirectLight = true;
    bool denoiser = true;
    bool ambientOcclusion = true;
    bool reflections = true;
    // Reflections follow a GGX lobe where the material has a Phong exponent; mirrors everywhere without.
    bool glossyReflections = true;
    // Translucent surfaces lit from the ray-traced irradiance volume instead of baked light.
    bool translucentLighting = true;
    // Views drawn into textures (cameras, mirrors, scopes) lit like translucent surfaces.
    bool renderTargetLighting = false;
    bool volumetricLight = true;
    bool fog = true;
    // A physically based sky in the 2D skybox's colours replaces the skybox, on screen and in the lighting. It darkens to
    // night as the sun sets, as does the light of the sun and sky, down to nightBrightness of the map's sky.
    bool physicalSky = true;
    MapSun mapSun = MapSun::Auto;
    float nightBrightness = 0.05f;
    // What the physical sky shows at night, and how: stars at starAmount times Earth's count, down to visual magnitude
    // faintestStars, starBrightness times as bright, twinkling twinkleStrength times as strongly and twinkleSpeed times as
    // fast as by default; the Milky Way milkyWayBrightness times as bright; shooting stars shootingStarFrequency times
    // as often.
    bool stars = true;
    float starAmount = 1.0f;
    float faintestStars = 10.0f;
    float starBrightness = 1.0f;
    bool twinkling = true;
    float twinkleStrength = 1.0f;
    float twinkleSpeed = 1.0f;
    bool milkyWay = true;
    float milkyWayBrightness = 1.0f;
    bool shootingStars = true;
    float shootingStarFrequency = 1.0f;
    bool satellites = true;
    // Our sun disc replaces env_sun's sprite, scaled from the size it has (Lighting::RecordSky).
    bool sunDisc = true;
    float sunSize = 1.0f;
    float sunIntensity = 1.0f;
    float skyIntensity = 1.0f;
    float lightIntensity = 1.0f;
    float indirectIntensity = 1.0f;
    float projectedIntensity = 1.0f;
    float reflectionIntensity = 1.0f;
    // How much ambient occlusion darkens the baked indirect light (IndirectLighting::Baked), which holds VRAD's
    // occlusion by static geometry but not what moves. Traced light holds its own occlusion.
    float aoIntensity = 1.0f;
    // Scales the haze's density.
    float volumetricIntensity = 1.0f;
    AntiAliasing antiAliasing = AntiAliasing::Temporal;
    UpscalingMode upscaling = UpscalingMode::Quality;
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
    // Shadow rays per pixel over the sun's disc on the held weapon, which nothing denoises; a quarter as many go over
    // each exactly traced lamp. 1 aims at the centres, for hard shadows.
    int viewmodelShadowRays = 8;
    FoliageShadows foliageShadows = FoliageShadows::All;
    IndirectLighting indirectLighting = IndirectLighting::RayTraced;
    EmissiveLighting emissiveLighting = EmissiveLighting::Full;
    // Indirect light and ambient occlusion rays take alpha-tested surfaces from their opacity micromaps alone.
    bool fastAlphaTests = false;
    // The world-space radiance cache (NVIDIA SHaRC) for the light of indirect, light volume and reflection hits.
    bool radianceCache = true;
    LightSampling lightSampling = LightSampling::Auto;
    DenoiserType denoiserType = DenoiserType::Relax;
    bool opacityMicromaps = true;
    // The display the game window goes on, 1-based in the order Displays() lists them; 0 leaves it where the game
    // puts it (the primary display).
    int display = 0;

    bool operator==(const Settings&) const = default;
};

// Sets the ray tracing detail settings (the menu's Quality page) to a preset's values; QualityPreset::Custom leaves them.
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

enum class GpuPass : uint32_t {
    SceneUpdate,
    Surface,
    RadianceCache,
    Volume,
    DirectLight,
    LightSampling,
    AmbientOcclusion,
    IndirectTrace,
    Reflections,
    Accumulation,
    Denoiser,
    Lighting,
    Resolve,
    Count
};

// Why real-time lighting is not drawing, if it is on.
enum class RealtimeBlock : uint8_t { None, Unsupported, OtherThread };

// Figures the renderer reports for the menu.
struct Statistics {
    static constexpr size_t kFrameHistory = 1000;

    // Wall-clock time between presents, oldest first once the ring has wrapped.
    std::array<float, kFrameHistory> frameMs = {};
    size_t frameCount = 0;
    size_t nextFrame = 0;
    // All views; passMs are the main view's passes and otherViewsMs the 3D skybox's and viewmodel's whole runs.
    double lightingMs = 0.0;
    std::array<double, static_cast<size_t>(GpuPass::Count)> passMs = {};
    double otherViewsMs = 0.0;
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
    // The device has what the radiance cache needs (RaytracingSupport::radianceCache).
    bool radianceCacheAvailable = false;
    // Alpha-tested and translucent triangles with an opacity micromap, and the micromaps they share.
    uint32_t micromapTriangles = 0;
    uint32_t micromaps = 0;
    RealtimeBlock realtimeBlock = RealtimeBlock::None;
};

Statistics& GetStatistics();
// Records the time since the last call as a frame's; call once per presented frame.
void RecordFrameInterval();

}
