#include "particle_shadows.h"

#include "log.h"
#include "options.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace gmodpp::particle_shadows {

namespace {

// Sprites fainter than this cast nothing worth tracing.
constexpr float kFaintest = 0.002f;
// The SDK's SpriteCard writes each sprite's centre into all four of its vertices and the vertex shader moves them out;
// positions that differ are the corners themselves.
constexpr float kSamePosition = 1e-3f;

std::vector<Particle> g_recorded;
bool g_loggedLayout = false;
bool g_loggedFull = false;

void Load(const std::uint8_t* vertex, std::uint32_t offset, float (&value)[3])
{
    std::memcpy(value, vertex + offset, sizeof(value));
}

}

void Record(const std::uint8_t* vertices, std::uint32_t vertexCount, const Layout& layout, const std::uint16_t* indices,
    std::span<const std::pair<int, int>> primitives, float density)
{
    if (vertices == nullptr || indices == nullptr || layout.stride == 0)
        return;
    for (const auto& [first, count] : primitives) {
        if (first < 0)
            continue;
        for (int quad = 0; quad + 6 <= count; quad += 6) {
            if (g_recorded.size() >= kMaxParticles) {
                if (!std::exchange(g_loggedFull, true))
                    log::Info("Particle shadows: more than {} sprites a frame; the rest cast no shadow", kMaxParticles);
                return;
            }
            const std::uint32_t base = indices[first + quad];
            if (base + 3 >= vertexCount)
                continue;
            float corners[4][3];
            for (std::uint32_t v = 0; v < 4; ++v)
                Load(vertices + static_cast<std::size_t>(base + v) * layout.stride, layout.position, corners[v]);
            const std::uint8_t* vertex = vertices + static_cast<std::size_t>(base) * layout.stride;
            float parameters[4];
            std::memcpy(parameters, vertex + layout.parameters, sizeof(parameters));
            const float opacity = std::clamp(vertex[layout.color + 3] / 255.0f * density, 0.0f, 0.999f);
            bool centred = true;
            for (std::uint32_t v = 1; v < 4; ++v) {
                for (int i = 0; i < 3; ++i)
                    centred = centred && std::fabs(corners[v][i] - corners[0][i]) <= kSamePosition * std::max(1.0f, std::fabs(corners[0][i]));
            }
            Particle particle = {};
            if (centred) {
                std::copy(std::begin(corners[0]), std::end(corners[0]), particle.centre);
                particle.radius = std::fabs(parameters[2]);
            } else {
                for (int i = 0; i < 3; ++i)
                    particle.centre[i] = 0.25f * (corners[0][i] + corners[1][i] + corners[2][i] + corners[3][i]);
                float diagonal = 0.0f;
                for (const auto [a, b] : {std::pair{0, 2}, std::pair{1, 3}}) {
                    float squared = 0.0f;
                    for (int i = 0; i < 3; ++i)
                        squared += (corners[a][i] - corners[b][i]) * (corners[a][i] - corners[b][i]);
                    diagonal = std::max(diagonal, std::sqrt(squared));
                }
                particle.radius = 0.5f * diagonal;
            }
            if (GetOptions().devMode && !std::exchange(g_loggedLayout, true))
                log::Info("Particle shadows: sprites come as {}", centred ? "their centre in every vertex (the radius from TEXCOORD2.z)" : "their corners");
            particle.opacity = opacity;
            if (opacity <= kFaintest || !(particle.radius > 0.0f) || !std::isfinite(particle.radius))
                continue;
            g_recorded.push_back(particle);
        }
    }
}

std::vector<Particle> Take() { return std::exchange(g_recorded, {}); }

}
