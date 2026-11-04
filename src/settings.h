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
    // Drawn by the display mapping over the final image (src/display_output.cpp): the frame's luminance in nits as a
    // false colour, and its pixels brighter than the display's peak.
    Luminance = 16,
    OverPeak = 17,
    Count
};

// Whether a debug view replaces the lighting's output (1-15), rather than showing the final image (0, 16, 17).
inline bool IsLightingDebugView(DebugView view) { return view != DebugView::Final && view < DebugView::Luminance; }

enum class AntiAliasing : uint32_t { Off = 0, Temporal = 1, RayReconstruction = 2, Fsr = 3, Count };

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
enum class DenoiserType : uint32_t { BuiltIn = 0, Relax, Reblur, RayRegeneration, Count };

// How lit colour after the game's tonemap scale is fitted into the 8-bit image, which clips each channel at white: as
// Source does (Off), by rolling off the highlights alone, or by a filmic curve (AgX) over the whole range
// (shaders/tone_curve.hlsli).
enum class ToneCurve : uint32_t { Off = 0, Highlights, Filmic, Count };

// The resolution the main view's clouds are kept at: a quarter, half or all of the screen's across.
enum class CloudResolution : uint32_t { Quarter = 0, Half, Full, Count };

// A mix of the three cloud layers (WeatherOf); Custom takes the cloud settings as they are.
enum class Weather : uint32_t { Clear = 0, FairWeather, Scattered, Broken, Overcast, MackerelSky, Cirrus, Towering, Custom, Count };

// How much the performance overlay shows, and the corner of the screen it sits in.
// NVIDIA Reflex's low latency mode (src/streamline.h).
enum class ReflexMode : uint32_t { Off = 0, On, Boost, Count };
// What generates frames between rendered ones: DLSS Frame Generation (Streamline) or AMD FSR Frame Generation.
enum class FrameGeneration : uint32_t { Off = 0, Dlss, Fsr, Count };
// Where depth of field focuses: on what the crosshair points at, or at Settings::dofFocusDistance.
enum class DofFocus : uint32_t { Auto = 0, Manual, Count };
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
    // The aerosols' glow around the sun in the physical sky, times this.
    float sunGlow = 1.0f;
    // Volumetric clouds in the physical sky (shaders/clouds.hlsl). With changingWeather the weather turns from one to
    // another on its own (CloudWeather), each holding about weatherDuration minutes; otherwise `weather` sets the
    // cloudCoverage to highCloudOpacity values (ApplyWeather), and Custom keeps them as they are. The low layer covers
    // cloudCoverage of the sky (1 overcast) with clouds of cloudType (0 stratus, 0.5 stratocumulus, 1 cumulus) from
    // cloudAltitude metres above the map, the tallest cloudThickness metres tall, cloudDensity times as dense as a
    // cumulus, cloudSize times as large, their edges eroded by cloudDetail. Their droplets are cloudDropletSize
    // micrometres across (their phase function), and each order of scattering passes cloudMultipleScattering of the light
    // on. They shade the map (cloudShadows) at cloudShadowStrength of their transmittance's effect. The defaults are
    // Weather::FairWeather's.
    bool clouds = true;
    bool changingWeather = true;
    float weatherDuration = 15.0f;
    Weather weather = Weather::FairWeather;
    float cloudCoverage = 0.2f;
    float cloudType = 1.0f;
    float cloudDensity = 1.0f;
    float cloudSize = 1.0f;
    float cloudDetail = 0.7f;
    float cloudAltitude = 1200.0f;
    float cloudThickness = 900.0f;
    float cloudDropletSize = 20.0f;
    float cloudMultipleScattering = 0.5f;
    bool cloudShadows = true;
    float cloudShadowStrength = 1.0f;
    // The wind carries the clouds windSpeed metres a second toward windDirection degrees (Hammer's yaw), higher layers
    // faster, and their shapes change cloudEvolution times as fast as by default.
    float windSpeed = 8.0f;
    float windDirection = 30.0f;
    float cloudEvolution = 1.0f;
    // A thin middle layer (altocumulus) and a sheet of cirrus above, highCloudOpacity its optical depth where thickest.
    bool midClouds = true;
    float midCloudCoverage = 0.0f;
    float midCloudAltitude = 4500.0f;
    float midCloudThickness = 600.0f;
    bool highClouds = true;
    float highCloudCoverage = 0.15f;
    float highCloudAltitude = 9000.0f;
    float highCloudOpacity = 0.2f;
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
    // The tone curve of the lit 3D view and the drawn sky; Highlights rolls them off toward white by highlightRolloff
    // (0 clips, 1 the full shoulder).
    ToneCurve toneCurve = ToneCurve::Highlights;
    float highlightRolloff = 1.0f;
    // Colour grading of the 3D view after the game's post-processing, before the HUD: exposure in stops, contrast
    // around middle grey, saturation, and white balance (warmth toward a warmer light's colour, tint toward magenta).
    bool grading = false;
    float exposure = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float warmth = 0.0f;
    float tint = 0.0f;
    // Camera effects on the 3D view after the game's post-processing, before the HUD (post_effects.cpp). Depth of field
    // (AMD FidelityFX DoF) follows a thin lens: focused on what the crosshair points at, settling over dofFocusTime
    // seconds, or at dofFocusDistance units; at f-number dofAperture on a sensor dofSensorSize times as tall as a
    // full-frame one (the view's field of view sets the focal length); blurring at most dofMaxBlur of the screen's
    // height, gathered over dofRings rings of samples.
    bool depthOfField = false;
    DofFocus dofFocus = DofFocus::Auto;
    float dofFocusDistance = 256.0f;
    float dofFocusTime = 0.25f;
    float dofAperture = 2.8f;
    float dofSensorSize = 1.0f;
    float dofMaxBlur = 0.03f;
    int dofRings = 8;
    // Motion blur of a shutter open 1 / motionBlurShutter seconds.
    bool motionBlur = false;
    float motionBlurShutter = 60.0f;
    // Lens flare from the sun or the Moon, and the starburst of glare around it.
    bool lensFlare = false;
    float lensFlareIntensity = 1.0f;
    float lensFlareStarburst = 1.0f;
    // Lens effects, by the field angle the view's field of view gives each pixel: radial distortion (lensDistortionAmount
    // is the coefficient of tan^2 of the angle, negative for barrel), lateral chromatic aberration (each colour
    // magnified differently by FidelityFX Lens's dispersion, growing with the angle like the distortion), vignetting
    // (vignetteAmount times the cos^4 law's exponent), film grain grainSize pixels across at 1080p, and contrast adaptive
    // sharpening (FidelityFX CAS).
    bool chromaticAberration = false;
    float chromaticAberrationStrength = 0.5f;
    bool lensDistortion = false;
    float lensDistortionAmount = -0.02f;
    bool vignette = false;
    float vignetteAmount = 0.35f;
    bool filmGrain = false;
    float grainAmount = 0.08f;
    float grainSize = 1.0f;
    bool contrastSharpening = false;
    float contrastSharpness = 0.5f;
    // Source's auto exposure on HDR maps (exposure.cpp): its goal biased by exposureBias stops and kept between
    // exposureMinimum and exposureMaximum (within Source's own range, which it clamps to first), approached over
    // exposureTime seconds (a third of that when darkening) in place of its frame-rate-bound step.
    bool exposureAdaptation = false;
    float exposureBias = 0.0f;
    float exposureMinimum = 0.0f;
    float exposureMaximum = 16.0f;
    float exposureTime = 1.0f;

    // The always-visible performance overlay; overlayHideWithHud hides it while the game hides its own HUD.
    bool performanceOverlay = false;
    bool overlayHideWithHud = true;
    OverlayDetail overlayDetail = OverlayDetail::Normal;
    OverlayCorner overlayCorner = OverlayCorner::TopRight;
    // Its look: background opacity and colour, text colour, size (times the menu's font), distance from the screen's
    // edge in font sizes, and whether it has a border.
    float overlayOpacity = 0.85f;
    Color overlayBackground = {0.063f, 0.075f, 0.094f};
    Color overlayText = {0.90f, 0.92f, 0.95f};
    float overlayScale = 1.0f;
    float overlayMargin = 1.0f;
    bool overlayBorder = true;

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
    // Depth slices of volumetric light's froxel volume (shaders/volumetrics.hlsl).
    int volumetricSlices = 64;
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
    // The main view's clouds at cloudResolution, marched in cloudSteps steps straight up (twice as many toward the
    // horizon) and cloudLightSteps toward the light from each.
    CloudResolution cloudResolution = CloudResolution::Full;
    int cloudSteps = 64;
    int cloudLightSteps = 6;
    // Latency and frame generation (this computer's): NVIDIA Reflex, the frame generator (DLSS or FSR, chosen when the
    // game starts as it makes the swap chain) with generatedFrames frames between rendered ones for DLSS, and AMD
    // Anti-Lag 2.
    ReflexMode reflex = ReflexMode::Off;
    FrameGeneration frameGeneration = FrameGeneration::Off;
    int generatedFrames = 1;
    bool antiLag = false;
    // The launch check's warning about a GPU that can't run the mod or its ray tracing (hardware_check.cpp); its
    // "Don't show this again" turns it off.
    bool hardwareWarning = true;
    // The display the game window goes on, 1-based in the order Displays() lists them; 0 leaves it where the game
    // puts it (the primary display).
    int display = 0;
    // The menu (this computer's): the virtual-key code that opens and closes it (VK_INSERT), whether Escape closes it,
    // its window's opacity, its size relative to the default for the display, its accent colour, the language of its
    // text (a localization folder name; empty follows the game's language), and the page it reopens on.
    int menuKey = 0x2D;
    bool menuEscapeCloses = true;
    float menuOpacity = 0.96f;
    float menuScale = 1.0f;
    Color menuAccent = {0.180f, 0.608f, 1.0f};
    std::string menuLanguage;
    bool menuRememberPage = true;
    int menuLastPage = 0;
    // The menu's collapsed cards, as widgets::CollapsedCards lists them.
    std::string menuCollapsed;
    // Turns off GMod's frame rate cap while its window is not focused (engine::CapUnfocusedFrames).
    bool backgroundFullSpeed = false;

    bool operator==(const Settings&) const = default;
};

// Sets the ray tracing detail settings (the menu's Quality page) to a preset's values; QualityPreset::Custom leaves them.
void ApplyPreset(Settings& settings, QualityPreset preset);

// What a weather sets: the low layer's coverage, type, altitude and thickness (metres), the middle and high layers'
// coverage, and the cirrus' optical depth (the Settings members of the same names).
struct WeatherValues {
    float coverage;
    float type;
    float altitude;
    float thickness;
    float midCoverage;
    float highCoverage;
    float highOpacity;
};
// A weather's values; Custom's are the settings' own.
WeatherValues WeatherOf(Weather weather, const Settings& settings);
// Sets the cloud settings a weather decides to its values; Weather::Custom leaves them.
void ApplyWeather(Settings& settings, Weather weather);

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

// What settings are grouped by: the menu's pages and what a preset or an exported file holds. General holds the
// real-time lighting switch; Display, Latency, Overlay, Developer and Menu belong to this computer.
enum class Section : uint32_t { General = 0, Lighting, Sky, Quality, Image, Overlay, Camera, Display, Latency, Developer, Menu, Count };
using SectionMask = uint32_t;
constexpr SectionMask SectionBit(Section section) { return 1u << static_cast<uint32_t>(section); }
// The sections presets and exported files can hold; the others belong to this computer.
constexpr SectionMask kShareableSections =
    SectionBit(Section::Lighting) | SectionBit(Section::Sky) | SectionBit(Section::Quality) | SectionBit(Section::Image) | SectionBit(Section::Camera);

using SettingMember = std::variant<bool Settings::*, float Settings::*, int Settings::*, Color Settings::*, std::string Settings::*, DebugView Settings::*,
    AntiAliasing Settings::*,
    UpscalingMode Settings::*, QualityPreset Settings::*, FoliageShadows Settings::*, IndirectLighting Settings::*, EmissiveLighting Settings::*,
    LightSampling Settings::*, DenoiserType Settings::*, MapSun Settings::*, OverlayDetail Settings::*, OverlayCorner Settings::*, DofFocus Settings::*,
    CloudResolution Settings::*, Weather Settings::*, ReflexMode Settings::*, FrameGeneration Settings::*, ToneCurve Settings::*>;

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

    // Wall-clock time between presents, oldest first once the ring has wrapped, and the frames each present put on
    // screen: 1, or with frame generation those it presented, generated ones included.
    std::array<float, kFrameHistory> frameMs = {};
    std::array<uint8_t, kFrameHistory> shownFrames = {};
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
    // AMD FSR upscaling can run (src/fsr.h).
    bool fsrAvailable = false;
    // AMD FSR Ray Regeneration can run (Radeon RX 9000).
    bool rayRegenerationAvailable = false;
    // NVIDIA Reflex and DLSS Frame Generation can run (src/streamline.h), and how many frames the latter can generate.
    bool reflexAvailable = false;
    bool dlssFrameGenerationAvailable = false;
    uint32_t generatedFramesMax = 0;
    // AMD FSR Frame Generation and Anti-Lag 2 can run.
    bool fsrFrameGenerationAvailable = false;
    bool antiLagAvailable = false;
    // The frame generator the swap chain was made for when the game started.
    FrameGeneration startedFrameGeneration = FrameGeneration::Off;
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
    // The changing weather in the last main view: the weather it shows and the one it is turning to (the same while it
    // holds).
    Weather weather = Weather::FairWeather;
    Weather nextWeather = Weather::FairWeather;
    // The game's fog in the open air before the fog override: sRGB colour, start, end and maximum density.
    Color gameFogColor = {};
    float gameFogStart = 0.0f;
    float gameFogEnd = 0.0f;
    float gameFogMaxDensity = 1.0f;
    bool gameFogKnown = false;
};

Statistics& GetStatistics();
// Records the time since the last call as a frame's; call once per present, with the frames it put on screen.
void RecordFrameInterval(uint32_t shownFrames);

}
