#include "settings.h"

#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <variant>

namespace gmodpp {

namespace {

using Field = std::variant<bool Settings::*, float Settings::*, int Settings::*, DebugView Settings::*, FlashlightMode Settings::*,
    AntiAliasing Settings::*, DlssMode Settings::*, QualityPreset Settings::*, FoliageShadows Settings::*>;

template <class Enum>
Enum EnumValue(double value, Enum fallback)
{
    return value >= 0.0 && value < static_cast<double>(Enum::Count) ? static_cast<Enum>(value) : fallback;
}

struct NamedField {
    const char* name;
    Field field;
    SettingRange range = {};
};

bool g_dirty = false;
std::filesystem::file_time_type g_lastWrite = {};

constexpr NamedField kFields[] = {
    {"realtime_lighting", &Settings::realtimeLighting},
    {"debug_view", &Settings::debugView},
    {"sun", &Settings::sun},
    {"map_lights", &Settings::mapLights},
    {"dynamic_lights", &Settings::dynamicLights},
    {"projected_lights", &Settings::projectedLights},
    {"indirect_light", &Settings::indirectLight},
    {"denoiser", &Settings::denoiser},
    {"ambient_occlusion", &Settings::ambientOcclusion},
    {"ao_intensity", &Settings::aoIntensity, {0.0f, 1.0f}},
    {"reflections", &Settings::reflections},
    {"translucent_lighting", &Settings::translucentLighting},
    {"volumetric_light", &Settings::volumetricLight},
    {"volumetric_intensity", &Settings::volumetricIntensity, {0.0f, 4.0f}},
    {"fog", &Settings::fog},
    {"flashlight_mode", &Settings::flashlightMode},
    {"sun_intensity", &Settings::sunIntensity, {0.0f, 4.0f}},
    {"sky_intensity", &Settings::skyIntensity, {0.0f, 4.0f}},
    {"light_intensity", &Settings::lightIntensity, {0.0f, 4.0f}},
    {"indirect_intensity", &Settings::indirectIntensity, {0.0f, 4.0f}},
    {"projected_intensity", &Settings::projectedIntensity, {0.0f, 8.0f}},
    {"reflection_intensity", &Settings::reflectionIntensity, {0.0f, 4.0f}},
    {"anti_aliasing", &Settings::antiAliasing},
    {"dlss_mode", &Settings::dlssMode},
    {"taa_history_weight", &Settings::taaHistoryWeight, {0.5f, 0.98f}},
    {"sharpening", &Settings::sharpening, {0.0f, 1.0f}},
    {"quality_preset", &Settings::qualityPreset},
    {"exact_lights", &Settings::exactLights, {0.0f, 8.0f}},
    {"sampled_light_rays", &Settings::sampledLightRays, {1.0f, 4.0f}},
    {"indirect_rays", &Settings::indirectRays, {1.0f, 4.0f}},
    {"indirect_bounces", &Settings::indirectBounces, {1.0f, 3.0f}},
    {"ao_rays", &Settings::aoRays, {1.0f, 4.0f}},
    {"volume_rays", &Settings::volumeRays, {8.0f, 64.0f}},
    {"volumetric_steps", &Settings::volumetricSteps, {4.0f, 32.0f}},
    {"foliage_shadows", &Settings::foliageShadows},
    {"fast_alpha_tests", &Settings::fastAlphaTests},
    {"opacity_micromaps", &Settings::opacityMicromaps},
};

struct PresetValues {
    int exactLights;
    int sampledLightRays;
    int indirectRays;
    int indirectBounces;
    int aoRays;
    int volumeRays;
    int volumetricSteps;
    FoliageShadows foliageShadows;
    bool fastAlphaTests;
};

// Low, Medium, High (the original quality) and Ultra.
constexpr PresetValues kPresets[] = {
    {1, 1, 1, 1, 1, 16, 8, FoliageShadows::Sun, true},
    {2, 1, 1, 2, 1, 24, 12, FoliageShadows::SunAndIndirect, true},
    {4, 2, 1, 2, 1, 32, 16, FoliageShadows::SunAndIndirect, false},
    {8, 4, 2, 3, 2, 64, 32, FoliageShadows::SunAndIndirect, false},
};
static_assert(std::size(kPresets) == static_cast<size_t>(QualityPreset::Custom));

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
    settings.foliageShadows = values.foliageShadows;
    settings.fastAlphaTests = values.fastAlphaTests;
}

Settings& GetSettings()
{
    static Settings settings;
    return settings;
}

Statistics& GetStatistics()
{
    static Statistics statistics;
    return statistics;
}

bool LoadSettings(const std::filesystem::path& path, Settings& settings)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"r") != 0)
        return false;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char* equals = std::strchr(line, '=');
        if (equals == nullptr)
            continue;
        *equals = '\0';
        const std::string key(line);
        const double value = std::strtod(equals + 1, nullptr);
        for (const NamedField& named : kFields) {
            if (key != named.name)
                continue;
            std::visit(
                [&](auto member) {
                    using Value = std::remove_reference_t<decltype(settings.*member)>;
                    if constexpr (std::is_same_v<Value, bool>)
                        settings.*member = value != 0.0;
                    else if constexpr (std::is_same_v<Value, float>)
                        settings.*member = std::clamp(static_cast<float>(value), named.range.minimum, named.range.maximum);
                    else if constexpr (std::is_same_v<Value, int>)
                        settings.*member = static_cast<int>(std::clamp(std::round(value), static_cast<double>(named.range.minimum), static_cast<double>(named.range.maximum)));
                    else if constexpr (std::is_same_v<Value, FlashlightMode>)
                        settings.*member = value == 1.0 ? FlashlightMode::Game : FlashlightMode::RayTraced;
                    else
                        settings.*member = EnumValue(value, Settings{}.*member);
                },
                named.field);
        }
    }
    std::fclose(file);
    // A preset decides the detail settings, so naming one in the file is enough.
    ApplyPreset(settings, settings.qualityPreset);
    return true;
}

void SaveSettings(const std::filesystem::path& path, const Settings& settings)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"w") != 0) {
        log::Info("Settings: cannot write {}", path.string());
        return;
    }
    for (const NamedField& named : kFields) {
        std::visit(
            [&](auto member) {
                using Value = std::remove_reference_t<decltype(settings.*member)>;
                if constexpr (std::is_same_v<Value, float>)
                    std::fprintf(file, "%s=%g\n", named.name, settings.*member);
                else if constexpr (std::is_same_v<Value, int>)
                    std::fprintf(file, "%s=%d\n", named.name, settings.*member);
                else
                    std::fprintf(file, "%s=%u\n", named.name, static_cast<unsigned>(settings.*member));
            },
            named.field);
    }
    std::fclose(file);
}

void MarkSettingsChanged() { g_dirty = true; }

void SyncSettingsFile(const std::filesystem::path& path)
{
    std::error_code error;
    if (g_dirty) {
        g_dirty = false;
        SaveSettings(path, GetSettings());
        g_lastWrite = std::filesystem::last_write_time(path, error);
        return;
    }
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(path, error);
    if (error || written == g_lastWrite)
        return;
    g_lastWrite = written;
    if (LoadSettings(path, GetSettings()))
        log::Info("Settings: loaded {}", path.string());
}

void RecordFrameTime(float milliseconds)
{
    Statistics& statistics = GetStatistics();
    statistics.frameMs[statistics.nextFrame] = milliseconds;
    statistics.nextFrame = (statistics.nextFrame + 1) % Statistics::kFrameHistory;
    statistics.frameCount = std::min(statistics.frameCount + 1, Statistics::kFrameHistory);
}

}
