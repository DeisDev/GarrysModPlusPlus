#include "settings.h"

#include "command_line.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace gmodpp {

namespace {

template <class Enum>
Enum EnumValue(double value, Enum fallback)
{
    return value >= 0.0 && value < static_cast<double>(Enum::Count) ? static_cast<Enum>(value) : fallback;
}

struct NamedField {
    const char* name;
    SettingMember field;
    Section section;
    SettingRange range = {};
    bool perMap = true;
};

using enum Section;

constexpr NamedField kFields[] = {
    {"realtime_lighting", &Settings::realtimeLighting, General},
    {"debug_view", &Settings::debugView, General, {}, false},
    {"display", &Settings::display, General, {0.0f, 16.0f}, false},

    {"sun", &Settings::sun, Lighting},
    {"sun_intensity", &Settings::sunIntensity, Lighting, {0.0f, 4.0f}},
    {"sun_follows_env_sun", &Settings::sunFollowsEnvSun, Lighting},
    {"sun_direction_override", &Settings::sunDirectionOverride, Lighting},
    {"sun_elevation", &Settings::sunElevation, Lighting, {-90.0f, 90.0f}},
    {"sun_azimuth", &Settings::sunAzimuth, Lighting, {0.0f, 360.0f}},
    {"sun_color_override", &Settings::sunColorOverride, Lighting},
    {"sun_color", &Settings::sunColor, Lighting},
    {"sun_shadow_softness", &Settings::sunShadowSoftness, Lighting, {0.25f, 10.0f}},
    {"sky_intensity", &Settings::skyIntensity, Lighting, {0.0f, 4.0f}},
    {"sky_light_override", &Settings::skyLightOverride, Lighting},
    {"sky_light_color", &Settings::skyLightColor, Lighting},
    {"sky_light_brightness", &Settings::skyLightBrightness, Lighting, {0.0f, 1000.0f}},
    {"map_lights", &Settings::mapLights, Lighting},
    {"light_intensity", &Settings::lightIntensity, Lighting, {0.0f, 4.0f}},
    {"light_tint", &Settings::lightTint, Lighting},
    {"dynamic_lights", &Settings::dynamicLights, Lighting},
    {"projected_lights", &Settings::projectedLights, Lighting},
    {"projected_intensity", &Settings::projectedIntensity, Lighting, {0.0f, 8.0f}},
    {"emissive_intensity", &Settings::emissiveIntensity, Lighting, {0.0f, 4.0f}},
    {"indirect_light", &Settings::indirectLight, Lighting},
    {"indirect_intensity", &Settings::indirectIntensity, Lighting, {0.0f, 4.0f}},
    {"ambient_occlusion", &Settings::ambientOcclusion, Lighting},
    {"ao_intensity", &Settings::aoIntensity, Lighting, {0.0f, 1.0f}},
    {"reflections", &Settings::reflections, Lighting},
    {"reflection_intensity", &Settings::reflectionIntensity, Lighting, {0.0f, 4.0f}},
    {"glossy_reflections", &Settings::glossyReflections, Lighting},
    {"volumetric_light", &Settings::volumetricLight, Lighting},
    {"volumetric_intensity", &Settings::volumetricIntensity, Lighting, {0.0f, 4.0f}},
    {"fog", &Settings::fog, Lighting},
    {"fog_override", &Settings::fogOverride, Lighting},
    {"fog_color", &Settings::fogColor, Lighting},
    {"fog_start", &Settings::fogStart, Lighting, {0.0f, 30000.0f}},
    {"fog_end", &Settings::fogEnd, Lighting, {1.0f, 30000.0f}},
    {"fog_max_density", &Settings::fogMaxDensity, Lighting, {0.0f, 1.0f}},
    {"translucent_lighting", &Settings::translucentLighting, Lighting},
    {"render_target_lighting", &Settings::renderTargetLighting, Lighting},

    {"physical_sky", &Settings::physicalSky, Sky},
    {"map_sun", &Settings::mapSun, Sky},
    {"moonlit_night", &Settings::moonlitNight, Sky, {0.01f, 0.25f}},
    {"moonless_night", &Settings::moonlessNight, Sky, {0.001f, 0.25f}},
    {"sun_disc", &Settings::sunDisc, Sky},
    {"sun_size", &Settings::sunSize, Sky, {0.25f, 4.0f}},
    {"stars", &Settings::stars, Sky},
    {"earlier_stars", &Settings::earlierStars, Sky, {0.0f, 12.0f}},
    {"star_amount", &Settings::starAmount, Sky, {0.0f, 4.0f}},
    {"faintest_stars", &Settings::faintestStars, Sky, {1.0f, 10.0f}},
    {"star_brightness", &Settings::starBrightness, Sky, {0.1f, 10.0f}},
    {"twinkling", &Settings::twinkling, Sky},
    {"twinkle_strength", &Settings::twinkleStrength, Sky, {0.0f, 4.0f}},
    {"twinkle_speed", &Settings::twinkleSpeed, Sky, {0.25f, 4.0f}},
    {"milky_way", &Settings::milkyWay, Sky},
    {"milky_way_brightness", &Settings::milkyWayBrightness, Sky, {0.0f, 4.0f}},
    {"shooting_stars", &Settings::shootingStars, Sky},
    {"shooting_star_frequency", &Settings::shootingStarFrequency, Sky, {0.1f, 10.0f}},
    {"satellites", &Settings::satellites, Sky},
    {"moon", &Settings::moon, Sky},
    {"moon_phase", &Settings::moonPhase, Sky, {0.0f, 1.0f}},

    {"quality_preset", &Settings::qualityPreset, Quality},
    {"exact_lights", &Settings::exactLights, Quality, {0.0f, 8.0f}},
    {"sampled_light_rays", &Settings::sampledLightRays, Quality, {1.0f, 4.0f}},
    {"indirect_rays", &Settings::indirectRays, Quality, {1.0f, 4.0f}},
    {"indirect_bounces", &Settings::indirectBounces, Quality, {1.0f, 3.0f}},
    {"ao_rays", &Settings::aoRays, Quality, {1.0f, 4.0f}},
    {"volume_rays", &Settings::volumeRays, Quality, {8.0f, 64.0f}},
    {"volumetric_steps", &Settings::volumetricSteps, Quality, {4.0f, 32.0f}},
    {"viewmodel_shadow_rays", &Settings::viewmodelShadowRays, Quality, {1.0f, 16.0f}},
    {"foliage_shadows", &Settings::foliageShadows, Quality},
    {"indirect_lighting", &Settings::indirectLighting, Quality},
    {"emissive_lighting", &Settings::emissiveLighting, Quality},
    {"fast_alpha_tests", &Settings::fastAlphaTests, Quality},
    {"opacity_micromaps", &Settings::opacityMicromaps, Quality},
    {"radiance_cache", &Settings::radianceCache, Quality},
    {"light_sampling", &Settings::lightSampling, Quality},

    {"anti_aliasing", &Settings::antiAliasing, Image},
    {"upscaling", &Settings::upscaling, Image},
    {"taa_history_weight", &Settings::taaHistoryWeight, Image, {0.5f, 0.98f}},
    {"sharpening", &Settings::sharpening, Image, {0.0f, 1.0f}},
    {"denoiser", &Settings::denoiser, Image},
    {"denoiser_type", &Settings::denoiserType, Image},
    {"grading", &Settings::grading, Image},
    {"exposure", &Settings::exposure, Image, {-4.0f, 4.0f}},
    {"contrast", &Settings::contrast, Image, {0.5f, 2.0f}},
    {"saturation", &Settings::saturation, Image, {0.0f, 2.0f}},
    {"warmth", &Settings::warmth, Image, {-1.0f, 1.0f}},
    {"tint", &Settings::tint, Image, {-1.0f, 1.0f}},

    {"performance_overlay", &Settings::performanceOverlay, Overlay, {}, false},
    {"overlay_detail", &Settings::overlayDetail, Overlay, {}, false},
    {"overlay_corner", &Settings::overlayCorner, Overlay, {}, false},
};
constexpr size_t kFieldCount = std::size(kFields);
using FieldSet = std::bitset<kFieldCount>;

struct PresetValues {
    int exactLights;
    int sampledLightRays;
    int indirectRays;
    int indirectBounces;
    int aoRays;
    int volumeRays;
    int volumetricSteps;
    int viewmodelShadowRays;
    FoliageShadows foliageShadows;
    bool fastAlphaTests;
    IndirectLighting indirectLighting;
    EmissiveLighting emissiveLighting;
    bool radianceCache;
    LightSampling lightSampling;
    bool opacityMicromaps;
};

// Low, Medium, High (the original quality) and Ultra.
constexpr PresetValues kPresets[] = {
    {1, 1, 1, 1, 1, 16, 8, 1, FoliageShadows::Sun, true, IndirectLighting::Baked, EmissiveLighting::Off, false, LightSampling::Auto, true},
    {2, 1, 1, 2, 1, 24, 12, 4, FoliageShadows::SunAndIndirect, true, IndirectLighting::RayTraced, EmissiveLighting::Bounce, true, LightSampling::Auto, true},
    {4, 2, 1, 2, 1, 32, 16, 8, FoliageShadows::All, false, IndirectLighting::RayTraced, EmissiveLighting::Full, true, LightSampling::Auto, true},
    {8, 4, 2, 3, 2, 64, 32, 16, FoliageShadows::All, false, IndirectLighting::RayTraced, EmissiveLighting::Full, true, LightSampling::Auto, true},
};
static_assert(std::size(kPresets) == static_cast<size_t>(QualityPreset::Custom));

constexpr char kHeader[] = "# GarrysMod++ settings\n";

struct State {
    Settings global;
    // What the current map sets, and which fields.
    Settings mapValues;
    FieldSet mapSet;
    std::string mapName;
    Settings effective;
    bool globalDirty = false;
    bool mapDirty = false;
    // The files' modification times as last read or written, to notice changes made outside the game.
    std::filesystem::file_time_type globalWritten = {};
    std::filesystem::file_time_type mapWritten = {};
};
State g_state;

template <class Value>
SettingRange FieldRange(Value Settings::* field)
{
    for (const NamedField& named : kFields) {
        const auto* member = std::get_if<Value Settings::*>(&named.field);
        if (member != nullptr && *member == field)
            return named.range;
    }
    return {};
}

bool Differs(const Settings& a, const Settings& b, size_t field)
{
    return std::visit([&](auto member) { return a.*member != b.*member; }, kFields[field].field);
}

void Copy(const Settings& from, Settings& to, size_t field)
{
    std::visit([&](auto member) { to.*member = from.*member; }, kFields[field].field);
}

bool InSections(size_t field, SectionMask sections) { return (sections & SectionBit(kFields[field].section)) != 0; }

std::filesystem::path FromUtf8(std::string_view text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }

std::string ToUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path GlobalPath() { return ModDirectory() / L"settings.ini"; }
std::filesystem::path MapsDirectory() { return ModDirectory() / L"maps"; }
std::filesystem::path PresetsDirectory() { return ModDirectory() / L"presets"; }
std::filesystem::path MapPath() { return MapsDirectory() / FromUtf8(g_state.mapName + ".ini"); }
std::filesystem::path PresetPath(std::string_view name) { return PresetsDirectory() / FromUtf8(std::string(name) + ".ini"); }

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

// Reads every known key of `text` into `settings`, clamped to its range, and marks it in `set`.
void Parse(std::string_view text, Settings& settings, FieldSet& set)
{
    while (!text.empty()) {
        const size_t end = text.find('\n');
        const std::string_view line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string_view key = Trim(line.substr(0, equals));
        const std::string value(Trim(line.substr(equals + 1)));
        for (size_t i = 0; i < kFieldCount; ++i) {
            const NamedField& named = kFields[i];
            if (key != named.name)
                continue;
            const double number = std::strtod(value.c_str(), nullptr);
            std::visit(
                [&](auto member) {
                    using Value = std::remove_cvref_t<decltype(settings.*member)>;
                    if constexpr (std::is_same_v<Value, bool>)
                        settings.*member = number != 0.0;
                    else if constexpr (std::is_same_v<Value, float>)
                        settings.*member = std::clamp(static_cast<float>(number), named.range.minimum, named.range.maximum);
                    else if constexpr (std::is_same_v<Value, int>)
                        settings.*member = static_cast<int>(
                            std::clamp(std::round(number), static_cast<double>(named.range.minimum), static_cast<double>(named.range.maximum)));
                    else if constexpr (std::is_same_v<Value, Color>) {
                        const char* cursor = value.c_str();
                        for (float& component : settings.*member) {
                            char* next = nullptr;
                            component = std::clamp(std::strtof(cursor, &next), 0.0f, 1.0f);
                            cursor = next;
                            while (*cursor == ' ' || *cursor == ',')
                                ++cursor;
                        }
                    } else
                        settings.*member = EnumValue(number, Settings{}.*member);
                },
                named.field);
            set.set(i);
        }
    }
}

std::string Line(const Settings& settings, size_t field)
{
    const NamedField& named = kFields[field];
    return std::visit(
        [&](auto member) {
            using Value = std::remove_cvref_t<decltype(settings.*member)>;
            const Value& value = settings.*member;
            if constexpr (std::is_same_v<Value, float>)
                return std::format("{}={}\n", named.name, value);
            else if constexpr (std::is_same_v<Value, int>)
                return std::format("{}={}\n", named.name, value);
            else if constexpr (std::is_same_v<Value, Color>)
                return std::format("{}={} {} {}\n", named.name, value[0], value[1], value[2]);
            else
                return std::format("{}={}\n", named.name, static_cast<unsigned>(value));
        },
        named.field);
}

std::optional<std::string> ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool WriteFile(const std::filesystem::path& path, std::string_view text)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    if (!file) {
        log::Info("Settings: cannot write {}", ToUtf8(path));
        return false;
    }
    return true;
}

std::filesystem::file_time_type WriteTime(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(path, error);
    return error ? std::filesystem::file_time_type{} : time;
}

void Recompose()
{
    Settings settings = g_state.global;
    for (size_t i = 0; i < kFieldCount; ++i) {
        if (g_state.mapSet.test(i))
            Copy(g_state.mapValues, settings, i);
    }
    // A preset decides the detail settings, so a map that only picks a preset gets all of it.
    ApplyPreset(settings, settings.qualityPreset);
    g_state.effective = settings;
}

void LoadGlobal()
{
    const std::filesystem::path path = GlobalPath();
    g_state.globalWritten = WriteTime(path);
    const std::optional<std::string> text = ReadFile(path);
    if (!text)
        return;
    FieldSet set;
    Parse(*text, g_state.global, set);
    ApplyPreset(g_state.global, g_state.global.qualityPreset);
    log::Info("Settings: loaded {}", ToUtf8(path));
}

void LoadMap()
{
    g_state.mapValues = Settings{};
    g_state.mapSet.reset();
    g_state.mapWritten = {};
    if (g_state.mapName.empty())
        return;
    const std::filesystem::path path = MapPath();
    g_state.mapWritten = WriteTime(path);
    const std::optional<std::string> text = ReadFile(path);
    if (!text)
        return;
    FieldSet set;
    Parse(*text, g_state.mapValues, set);
    for (size_t i = 0; i < kFieldCount; ++i)
        g_state.mapSet[i] = set[i] && kFields[i].perMap;
    log::Info("Settings: {} sets {} setting(s) of its own", g_state.mapName, g_state.mapSet.count());
}

void SaveGlobal()
{
    std::string text = kHeader;
    for (size_t i = 0; i < kFieldCount; ++i)
        text += Line(g_state.global, i);
    const std::filesystem::path path = GlobalPath();
    if (WriteFile(path, text))
        g_state.globalWritten = WriteTime(path);
}

void SaveMap()
{
    if (g_state.mapName.empty())
        return;
    const std::filesystem::path path = MapPath();
    if (g_state.mapSet.none()) {
        std::error_code error;
        std::filesystem::remove(path, error);
        g_state.mapWritten = {};
        return;
    }
    std::string text = kHeader;
    for (size_t i = 0; i < kFieldCount; ++i) {
        if (g_state.mapSet.test(i))
            text += Line(g_state.mapValues, i);
    }
    if (WriteFile(path, text))
        g_state.mapWritten = WriteTime(path);
}

void SaveDirty()
{
    if (std::exchange(g_state.globalDirty, false))
        SaveGlobal();
    if (std::exchange(g_state.mapDirty, false))
        SaveMap();
}

void EnsureLoaded()
{
    static std::once_flag once;
    std::call_once(once, [] {
        LoadGlobal();
        Recompose();
    });
}

// "maps/gm_construct.bsp" -> "gm_construct".
std::string MapOf(std::string_view level)
{
    const size_t slash = level.find_last_of("/\\");
    if (slash != std::string_view::npos)
        level.remove_prefix(slash + 1);
    if (level.size() > 4 && _strnicmp(level.data() + level.size() - 4, ".bsp", 4) == 0)
        level.remove_suffix(4);
    return std::string(level);
}

void WriteField(config::Scope scope, const Settings& from, size_t field)
{
    if (scope == config::Scope::Global || !kFields[field].perMap || g_state.mapName.empty()) {
        Copy(from, g_state.global, field);
        g_state.globalDirty = true;
    } else {
        Copy(from, g_state.mapValues, field);
        g_state.mapSet.set(field);
        g_state.mapDirty = true;
    }
}

}

FieldId FieldOf(SettingMember member)
{
    for (size_t i = 0; i < kFieldCount; ++i) {
        if (kFields[i].field == member)
            return static_cast<FieldId>(i);
    }
    return kNoField;
}

bool SameValue(const Settings& a, const Settings& b, FieldId field) { return field >= kFieldCount || !Differs(a, b, field); }

std::vector<FieldId> FieldsIn(SectionMask sections)
{
    std::vector<FieldId> fields;
    for (size_t i = 0; i < kFieldCount; ++i) {
        if (InSections(i, sections))
            fields.push_back(static_cast<FieldId>(i));
    }
    return fields;
}

SettingRange RangeOf(float Settings::* field) { return FieldRange(field); }

SettingRange RangeOf(int Settings::* field) { return FieldRange(field); }

void ApplyPreset(Settings& settings, QualityPreset preset)
{
    settings.qualityPreset = preset;
    if (preset == QualityPreset::Custom)
        return;
    const PresetValues& values = kPresets[static_cast<size_t>(preset)];
    settings.exactLights = values.exactLights;
    settings.sampledLightRays = values.sampledLightRays;
    settings.indirectRays = values.indirectRays;
    settings.indirectBounces = values.indirectBounces;
    settings.aoRays = values.aoRays;
    settings.volumeRays = values.volumeRays;
    settings.volumetricSteps = values.volumetricSteps;
    settings.viewmodelShadowRays = values.viewmodelShadowRays;
    settings.foliageShadows = values.foliageShadows;
    settings.fastAlphaTests = values.fastAlphaTests;
    settings.indirectLighting = values.indirectLighting;
    settings.emissiveLighting = values.emissiveLighting;
    settings.radianceCache = values.radianceCache;
    settings.lightSampling = values.lightSampling;
    settings.opacityMicromaps = values.opacityMicromaps;
}

const Settings& GetSettings()
{
    EnsureLoaded();
    return g_state.effective;
}

Statistics& GetStatistics()
{
    static Statistics statistics;
    return statistics;
}

namespace config {

void Sync(std::string_view levelName)
{
    EnsureLoaded();
    const std::string map = MapOf(levelName);
    if (map != g_state.mapName) {
        SaveDirty();
        g_state.mapName = map;
        LoadMap();
        Recompose();
    }
    SaveDirty();
    const std::filesystem::file_time_type global = WriteTime(GlobalPath());
    if (global != g_state.globalWritten) {
        LoadGlobal();
        Recompose();
    }
    if (!g_state.mapName.empty() && WriteTime(MapPath()) != g_state.mapWritten) {
        LoadMap();
        Recompose();
    }
}

const std::string& MapName() { return g_state.mapName; }

const Settings& Global()
{
    EnsureLoaded();
    return g_state.global;
}

bool MapSets(FieldId field) { return field < kFieldCount && g_state.mapSet.test(field); }

size_t MapSetCount() { return g_state.mapSet.count(); }

bool PerMap(FieldId field) { return field < kFieldCount && kFields[field].perMap; }

Section SectionOf(FieldId field) { return field < kFieldCount ? kFields[field].section : Section::General; }

Settings View(Scope scope)
{
    EnsureLoaded();
    return scope == Scope::Global || g_state.mapName.empty() ? g_state.global : g_state.effective;
}

void Commit(Scope scope, const Settings& before, const Settings& after)
{
    for (size_t i = 0; i < kFieldCount; ++i) {
        if (Differs(before, after, i))
            WriteField(scope, after, i);
    }
    Recompose();
}

void Reset(Scope scope, std::span<const FieldId> fields)
{
    const Settings defaults;
    for (const FieldId field : fields) {
        if (field >= kFieldCount)
            continue;
        if (scope == Scope::Global || !kFields[field].perMap || g_state.mapName.empty()) {
            Copy(defaults, g_state.global, field);
            g_state.globalDirty = true;
        } else if (g_state.mapSet.test(field)) {
            g_state.mapSet.reset(field);
            g_state.mapDirty = true;
        }
    }
    ApplyPreset(g_state.global, g_state.global.qualityPreset);
    Recompose();
}

void ResetSections(Scope scope, SectionMask sections)
{
    std::vector<FieldId> fields;
    for (size_t i = 0; i < kFieldCount; ++i) {
        if (InSections(i, sections) && (scope == Scope::Global || kFields[i].perMap))
            fields.push_back(static_cast<FieldId>(i));
    }
    Reset(scope, fields);
}

std::string Export(Scope scope, SectionMask sections)
{
    const Settings settings = View(scope);
    std::string text = kHeader;
    for (size_t i = 0; i < kFieldCount; ++i) {
        if (InSections(i, sections & kShareableSections))
            text += Line(settings, i);
    }
    return text;
}

SectionMask SectionsIn(std::string_view text)
{
    Settings parsed;
    FieldSet set;
    Parse(text, parsed, set);
    SectionMask sections = 0;
    for (size_t i = 0; i < kFieldCount; ++i) {
        if (set.test(i) && InSections(i, kShareableSections))
            sections |= SectionBit(kFields[i].section);
    }
    return sections;
}

std::optional<SectionMask> Import(Scope scope, std::string_view text)
{
    Settings parsed = View(scope);
    FieldSet set;
    Parse(text, parsed, set);
    SectionMask sections = 0;
    for (size_t i = 0; i < kFieldCount; ++i) {
        if (!set.test(i) || !InSections(i, kShareableSections))
            continue;
        WriteField(scope, parsed, i);
        sections |= SectionBit(kFields[i].section);
    }
    if (sections == 0)
        return std::nullopt;
    ApplyPreset(g_state.global, g_state.global.qualityPreset);
    Recompose();
    return sections;
}

std::vector<std::string> Presets()
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(PresetsDirectory(), error)) {
        if (entry.is_regular_file(error) && _wcsicmp(entry.path().extension().c_str(), L".ini") == 0)
            files.push_back(entry.path().stem());
    }
    std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        const std::wstring& left = a.native();
        const std::wstring& right = b.native();
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, left.c_str(), static_cast<int>(left.size()), right.c_str(),
                   static_cast<int>(right.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    std::vector<std::string> names;
    for (const std::filesystem::path& file : files)
        names.push_back(ToUtf8(file));
    return names;
}

bool ValidPresetName(std::string_view name)
{
    if (name.empty() || name.size() > 64 || name.back() == '.' || name.back() == ' ' || name.front() == ' ')
        return false;
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
            return false;
    }
    // Device names Windows reserves, with or without an extension.
    std::string stem(name.substr(0, name.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    for (const std::string_view reserved : {"CON", "PRN", "AUX", "NUL"}) {
        if (stem == reserved)
            return false;
    }
    return !(stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '0' && stem[3] <= '9');
}

bool SavePreset(std::string_view name, Scope scope, SectionMask sections)
{
    return ValidPresetName(name) && WriteFile(PresetPath(name), Export(scope, sections));
}

std::optional<SectionMask> PresetSections(std::string_view name)
{
    const std::optional<std::string> text = ReadFile(PresetPath(name));
    if (!text)
        return std::nullopt;
    return SectionsIn(*text);
}

std::optional<SectionMask> LoadPreset(std::string_view name, Scope scope)
{
    const std::optional<std::string> text = ReadFile(PresetPath(name));
    if (!text)
        return std::nullopt;
    return Import(scope, *text);
}

bool RenamePreset(std::string_view from, std::string_view to)
{
    if (!ValidPresetName(to))
        return false;
    std::error_code error;
    const std::filesystem::path target = PresetPath(to);
    // Renaming to the same name in other letter case is allowed; to another preset's name is not.
    if (std::filesystem::exists(target, error) && !std::filesystem::equivalent(target, PresetPath(from), error))
        return false;
    std::filesystem::rename(PresetPath(from), target, error);
    return !error;
}

bool DeletePreset(std::string_view name)
{
    std::error_code error;
    return std::filesystem::remove(PresetPath(name), error);
}

bool WriteTextFile(const std::wstring& path, std::string_view text) { return WriteFile(std::filesystem::path(path), text); }

std::optional<std::string> ReadTextFile(const std::wstring& path) { return ReadFile(std::filesystem::path(path)); }

}

void RecordFrameInterval()
{
    static std::optional<std::chrono::steady_clock::time_point> previous;
    const auto now = std::chrono::steady_clock::now();
    const std::optional<std::chrono::steady_clock::time_point> last = std::exchange(previous, now);
    if (!last)
        return;
    Statistics& statistics = GetStatistics();
    statistics.frameMs[statistics.nextFrame] = std::chrono::duration<float, std::milli>(now - *last).count();
    statistics.nextFrame = (statistics.nextFrame + 1) % Statistics::kFrameHistory;
    statistics.frameCount = std::min(statistics.frameCount + 1, Statistics::kFrameHistory);
}

}
