#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gmodpp {

// A colour picked in the menu: sRGB components from 0 to 1.
using Color = std::array<float, 3>;

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

// How much the performance overlay shows, and the corner of the screen it sits in.
enum class OverlayDetail : uint32_t { Minimal = 0, Normal, Full, Extra, Count };
enum class OverlayCorner : uint32_t { TopLeft = 0, TopRight, BottomLeft, BottomRight, Count };

// Options shown in the in-game menu. Read on the render thread; the menu edits them on the same thread.
struct Settings {
    bool realtimeLighting = true;
    DebugView debugView = DebugView::Final;
    bool sun = true;
    // The sun shines from env_sun's direction, which the sun editor turns, instead of light_environment's.
    bool sunFollowsEnvSun = true;
    // The sun shines from sunElevation degrees above the horizon and sunAzimuth degrees around it (Hammer's yaw: 0 along
    // +x, 90 along +y), whatever the map says; the sky, the sun disc and the night follow it.
    bool sunDirectionOverride = false;
    float sunElevation = 45.0f;
    float sunAzimuth = 0.0f;
    // The sun takes sunColor's hue at the map sun's brightness.
    bool sunColorOverride = false;
    Color sunColor = {1.0f, 1.0f, 1.0f};
    // The sun's disc for soft shadows, times the real sun's.
    float sunShadowSoftness = 1.0f;
    // The sky's light is skyLightColor at skyLightBrightness, as light_environment's _ambient gives them (Hammer's
    // brightness: the linear colour times brightness / 255), in place of the map's.
    bool skyLightOverride = false;
    Color skyLightColor = {0.6f, 0.7f, 0.9f};
    float skyLightBrightness = 50.0f;
    bool mapLights = true;
    // Map lights' colour is multiplied by this.
    Color lightTint = {1.0f, 1.0f, 1.0f};
    // The light glowing surfaces cast on others, times this; their own look stays.
    float emissiveIntensity = 1.0f;
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
    // The map's fog in the open air (not water's) takes this colour, distances and density, also where the map has none.
    bool fogOverride = false;
    Color fogColor = {0.6f, 0.65f, 0.7f};
    float fogStart = 500.0f;
    float fogEnd = 8000.0f;
    float fogMaxDensity = 1.0f;
    // A physically based sky in the 2D skybox's colours replaces the skybox, on screen and in the lighting. It darkens to
    // night as the sun sets, as does the light of the sun and sky: under a full Moon to moonlitNight of the map's sky and
    // light, without one to moonlessNight.
    bool physicalSky = true;
    MapSun mapSun = MapSun::Auto;
    float moonlitNight = 0.05f;
    float moonlessNight = 0.01f;
    // What the physical sky shows at night, and how: stars at starAmount times Earth's count, down to visual magnitude
    // faintestStars (by default 6.5, the naked eye's limit under a dark sky), starBrightness times as bright, twinkling
    // twinkleStrength times as strongly and twinkleSpeed times as fast as by default; the Milky Way milkyWayBrightness
    // times as bright; shooting stars shootingStarFrequency times as often.
    bool stars = true;
    // The night sky (stars, the Milky Way, satellites and meteors) comes out as it would with the sun this many degrees
    // lower.
    float earlierStars = 0.0f;
    float starAmount = 1.0f;
    float faintestStars = 6.5f;
    float starBrightness = 1.0f;
    bool twinkling = true;
    float twinkleStrength = 1.0f;
    float twinkleSpeed = 1.0f;
    bool milkyWay = true;
    float milkyWayBrightness = 1.0f;
    bool shootingStars = true;
    float shootingStarFrequency = 1.0f;
    bool satellites = true;
    // The Moon of day maps, at moonPhase (the share of its disc lit, 1 full) from the sun; it lights the night.
    bool moon = true;
    float moonPhase = 1.0f;
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
    // Colour grading of the 3D view after the game's post-processing, before the HUD: exposure in stops, contrast
    // around middle grey, saturation, and white balance (warmth toward a warmer light's colour, tint toward magenta).
    bool grading = false;
    float exposure = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float warmth = 0.0f;
    float tint = 0.0f;

    // The always-visible performance overlay.
    bool performanceOverlay = false;
    OverlayDetail overlayDetail = OverlayDetail::Normal;
    OverlayCorner overlayCorner = OverlayCorner::TopRight;

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

// The settings in effect: the global ones with the current map's overrides (config namespace). Read-only: the menu
// changes settings through config::Commit.
const Settings& GetSettings();

// The range the menu's slider offers for a float setting; values read from files are clamped to it.
struct SettingRange {
    float minimum;
    float maximum;
};
SettingRange RangeOf(float Settings::* field);
SettingRange RangeOf(int Settings::* field);

// What settings are grouped by: the menu's pages and what a preset or an exported file holds.
enum class Section : uint32_t { General = 0, Lighting, Sky, Quality, Image, Overlay, Count };
using SectionMask = uint32_t;
constexpr SectionMask SectionBit(Section section) { return 1u << static_cast<uint32_t>(section); }
// The sections presets and exported files can hold; General and the overlay belong to this computer.
constexpr SectionMask kShareableSections =
    SectionBit(Section::Lighting) | SectionBit(Section::Sky) | SectionBit(Section::Quality) | SectionBit(Section::Image);

using SettingMember = std::variant<bool Settings::*, float Settings::*, int Settings::*, Color Settings::*, DebugView Settings::*, AntiAliasing Settings::*,
    UpscalingMode Settings::*, QualityPreset Settings::*, FoliageShadows Settings::*, IndirectLighting Settings::*, EmissiveLighting Settings::*,
    LightSampling Settings::*, DenoiserType Settings::*, MapSun Settings::*, OverlayDetail Settings::*, OverlayCorner Settings::*>;

// A setting's place in the settings table (kFields in settings.cpp).
using FieldId = uint16_t;
inline constexpr FieldId kNoField = 0xFFFF;
FieldId FieldOf(SettingMember member);
// Whether `field` holds the same value in both settings.
bool SameValue(const Settings& a, const Settings& b, FieldId field);
// The fields of `sections`, in table order.
std::vector<FieldId> FieldsIn(SectionMask sections);

// Settings live in gmodplusplus: settings.ini holds the global ones, maps\<map>.ini only what a map sets differently,
// and presets\<name>.ini the sections a preset was saved with. All are key=value text; unknown keys are ignored.
namespace config {

// Global: the settings of every map. Map: what applies on the current map, where a change sets the map's own value.
enum class Scope { Global, Map };

// Follows the level `levelName` (the engine's "maps/<map>.bsp", empty outside a map) to its map's settings, saves
// pending changes and reloads files changed outside the game. Call periodically and when a level starts loading.
void Sync(std::string_view levelName);

// The current map ("gm_construct"), or empty outside a map.
const std::string& MapName();
const Settings& Global();
// Whether the current map sets `field`, and how many fields it sets.
bool MapSets(FieldId field);
size_t MapSetCount();
// Whether a field may differ between maps; the display, debug view and the overlay can't.
bool PerMap(FieldId field);
Section SectionOf(FieldId field);

// The settings as `scope` shows them: the global ones, or those in effect on the map.
Settings View(Scope scope);
// Records the fields that differ between `before` and `after` (both views of `scope`) in `scope`.
void Commit(Scope scope, const Settings& before, const Settings& after);
// Puts fields back: globally to their defaults, on the map to the global value.
void Reset(Scope scope, std::span<const FieldId> fields);
void ResetSections(Scope scope, SectionMask sections);

// The `sections` of what `scope` shows in the settings files' format, under a comment line naming the mod.
std::string Export(Scope scope, SectionMask sections);
// Applies the settings `text` holds to `scope`. Returns the sections it held, or nothing when it held no setting.
std::optional<SectionMask> Import(Scope scope, std::string_view text);
// The sections a settings text holds, or 0.
SectionMask SectionsIn(std::string_view text);

// Saved presets by name (UTF-8), sorted.
std::vector<std::string> Presets();
// Whether `name` can name a preset file.
bool ValidPresetName(std::string_view name);
bool SavePreset(std::string_view name, Scope scope, SectionMask sections);
// The sections the preset holds, or nothing when it can't be read.
std::optional<SectionMask> PresetSections(std::string_view name);
std::optional<SectionMask> LoadPreset(std::string_view name, Scope scope);
bool RenamePreset(std::string_view from, std::string_view to);
bool DeletePreset(std::string_view name);

// Reads and writes a settings file chosen by the player (export and import).
bool WriteTextFile(const std::wstring& path, std::string_view text);
std::optional<std::string> ReadTextFile(const std::wstring& path);

}

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
    // The map's light before the menu's overrides, from the last main view, for the overrides to start from: the
    // direction to the sun (env_sun's or light_environment's), the sun's and sky's linear colours, and whether the map
    // has a sun.
    std::array<float, 3> sunDirection = {0.0f, 0.0f, 1.0f};
    std::array<float, 3> mapSunColor = {};
    std::array<float, 3> mapSkyColor = {};
    bool mapHasSun = false;
    // The game's fog in the open air before the fog override: sRGB colour, start, end and maximum density.
    Color gameFogColor = {};
    float gameFogStart = 0.0f;
    float gameFogEnd = 0.0f;
    float gameFogMaxDensity = 1.0f;
    bool gameFogKnown = false;
};

Statistics& GetStatistics();
// Records the time since the last call as a frame's; call once per presented frame.
void RecordFrameInterval();

}
