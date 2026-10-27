#include "settings.h"

#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <variant>

namespace gmodpp {

namespace {

using Field = std::variant<bool Settings::*, float Settings::*, DebugView Settings::*, FlashlightMode Settings::*, AntiAliasing Settings::*,
    DlssMode Settings::*>;

template <class Enum>
Enum EnumValue(double value, Enum fallback)
{
    return value >= 0.0 && value < static_cast<double>(Enum::Count) ? static_cast<Enum>(value) : fallback;
}

struct NamedField {
    const char* name;
    Field field;
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
    {"ao_intensity", &Settings::aoIntensity},
    {"reflections", &Settings::reflections},
    {"translucent_lighting", &Settings::translucentLighting},
    {"volumetric_light", &Settings::volumetricLight},
    {"volumetric_intensity", &Settings::volumetricIntensity},
    {"fog", &Settings::fog},
    {"flashlight_mode", &Settings::flashlightMode},
    {"sun_intensity", &Settings::sunIntensity},
    {"sky_intensity", &Settings::skyIntensity},
    {"light_intensity", &Settings::lightIntensity},
    {"indirect_intensity", &Settings::indirectIntensity},
    {"projected_intensity", &Settings::projectedIntensity},
    {"reflection_intensity", &Settings::reflectionIntensity},
    {"anti_aliasing", &Settings::antiAliasing},
    {"dlss_mode", &Settings::dlssMode},
    {"taa_history_weight", &Settings::taaHistoryWeight},
    {"sharpening", &Settings::sharpening},
};

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
                        settings.*member = static_cast<float>(value);
                    else if constexpr (std::is_same_v<Value, FlashlightMode>)
                        settings.*member = value == 1.0 ? FlashlightMode::Game : FlashlightMode::RayTraced;
                    else
                        settings.*member = EnumValue(value, Settings{}.*member);
                },
                named.field);
        }
    }
    std::fclose(file);
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
