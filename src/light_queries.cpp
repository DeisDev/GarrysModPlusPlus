#include "light_queries.h"

#include "engine_interfaces.h"
#include "log.h"

#include <DirectXPackedVector.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace gmodpp::light_queries {

namespace {

// A volume this old is from frames that no longer filled it: lighting or translucent lighting turned off.
constexpr auto kMaxAge = std::chrono::milliseconds(250);
constexpr size_t kTexelHalves = 12;

struct Volume {
    std::array<int, 3> origin = {};
    std::array<uint32_t, 3> grid = {};
    float spacing = 0.0f;
    std::vector<uint16_t> texels;
    std::chrono::steady_clock::time_point published;
};

// Particles may be simulated on worker threads.
std::shared_mutex g_mutex;
Volume g_volume;
engine::ComputeLightingFn g_computeLighting = nullptr;
bool g_attempted = false;

enum class Answer { Volume, NoVolume, Stale, NoPoint, Count };
constexpr const char* kAnswerNames[] = {"answered from the irradiance volume", "left to the engine: no volume yet",
    "left to the engine: the volume is out of date", "left to the engine: no point or colour given"};
static_assert(std::size(kAnswerNames) == static_cast<size_t>(Answer::Count));
// Each kind of answer is logged the first time it is given.
std::array<std::atomic<bool>, static_cast<size_t>(Answer::Count)> g_logged = {};

// The coefficients at a point, interpolated between the eight probes around it as the GPU's trilinear filter does.
// Beyond the volume's window, its nearest probes', as translucent surfaces take there (shaders/forward_light.hlsli): the
// engine's own answer is its baked light, which follows neither the light now nor day and night.
Answer Sample(const float point[3], float coefficients[3][4])
{
    std::shared_lock lock(g_mutex);
    if (g_volume.texels.empty())
        return Answer::NoVolume;
    if (std::chrono::steady_clock::now() - g_volume.published > kMaxAge)
        return Answer::Stale;
    int base[3];
    float fraction[3];
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(point[axis]))
            return Answer::NoPoint;
        const float window = std::clamp(point[axis] / g_volume.spacing - 0.5f - static_cast<float>(g_volume.origin[axis]), 0.0f,
            static_cast<float>(g_volume.grid[axis] - 1));
        const float probe = window + static_cast<float>(g_volume.origin[axis]);
        base[axis] = static_cast<int>(std::floor(probe));
        fraction[axis] = probe - static_cast<float>(base[axis]);
    }
    std::fill(&coefficients[0][0], &coefficients[0][0] + 12, 0.0f);
    for (int corner = 0; corner < 8; ++corner) {
        float weight = 1.0f;
        size_t texel = 0;
        for (int axis = 2; axis >= 0; --axis) {
            const int offset = (corner >> axis) & 1;
            weight *= offset ? fraction[axis] : 1.0f - fraction[axis];
            const int size = static_cast<int>(g_volume.grid[axis]);
            texel = texel * size + static_cast<size_t>(((base[axis] + offset) % size + size) % size);
        }
        if (weight == 0.0f)
            continue;
        const uint16_t* halves = g_volume.texels.data() + texel * kTexelHalves;
        for (size_t i = 0; i < kTexelHalves; ++i)
            coefficients[i / 4][i % 4] += weight * DirectX::PackedVector::XMConvertHalfToFloat(halves[i]);
    }
    return Answer::Volume;
}

void __fastcall HookComputeLighting(void* engine, const float* point, const float* normal, bool clamp, float* color, float* boxColors)
{
    float coefficients[3][4];
    const Answer answer = point != nullptr && color != nullptr ? Sample(point, coefficients) : Answer::NoPoint;
    if (!g_logged[static_cast<size_t>(answer)].exchange(true))
        log::Info("Light queries: first ComputeLighting call {}", kAnswerNames[static_cast<size_t>(answer)]);
    if (answer != Answer::Volume) {
        g_computeLighting(engine, point, normal, clamp, color, boxColors);
        return;
    }
    for (int channel = 0; channel < 3; ++channel) {
        const float* c = coefficients[channel];
        // Without a normal the engine adds its local lights at full strength to the ambient cube's average; the
        // irradiance facing the brightest direction is the closest match.
        float value = normal != nullptr ? c[0] + c[1] * normal[0] + c[2] * normal[1] + c[3] * normal[2] : c[0] + std::sqrt(c[1] * c[1] + c[2] * c[2] + c[3] * c[3]);
        value = std::max(value, 0.0f);
        color[channel] = clamp ? std::min(value, 1.0f) : value;
        // Source's ambient cube order: +x, -x, +y, -y, +z, -z.
        for (int face = 0; boxColors != nullptr && face < 6; ++face)
            boxColors[face * 3 + channel] = std::max(c[0] + (face % 2 ? -c[1 + face / 2] : c[1 + face / 2]), 0.0f);
    }
}

}

void TryInstall()
{
    if (g_attempted)
        return;
    engine::ComputeLightingFn target = engine::ComputeLightingFunction();
    if (target == nullptr)
        return;
    g_attempted = true;
    MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)
        status = MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&HookComputeLighting), reinterpret_cast<void**>(&g_computeLighting));
    if (status == MH_OK)
        status = MH_EnableHook(reinterpret_cast<void*>(target));
    if (status != MH_OK)
        log::Info("Hooking IVEngineClient::ComputeLighting failed: {}; particles keep their baked light", MH_StatusToString(status));
}

void Publish(const std::array<int, 3>& origin, const std::array<uint32_t, 3>& grid, float spacing, std::span<const uint16_t> texels)
{
    std::unique_lock lock(g_mutex);
    g_volume.origin = origin;
    g_volume.grid = grid;
    g_volume.spacing = spacing;
    g_volume.texels.assign(texels.begin(), texels.end());
    g_volume.published = std::chrono::steady_clock::now();
}

void Clear()
{
    std::unique_lock lock(g_mutex);
    g_volume.texels.clear();
}

}
