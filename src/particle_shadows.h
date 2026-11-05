#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace gmodpp::particle_shadows {

// Smoke and other particles that blend shadow the sun's light (Settings::particleShadows): the main view's SpriteCard draws
// are read on the CPU as they are drawn, each sprite a sphere of its size and coverage, and the next main view's lighting
// splats them into a volume of extinction and a shadow map over the plane across the sun (shaders/particle_shadows.hlsl),
// which every sun shadow ray reads (ParticleShadowAt in shaders/common.hlsli). They shade the sun's light only: the
// sprites already dim what lies behind them and carry their own light.

// A sprite as the GPU reads it (Particle in shaders/particle_shadows.hlsl): its centre, radius and the share of light a
// ray through its centre loses.
struct Particle {
    float centre[3];
    float radius;
    float opacity;
    float padding[3];
};
static_assert(sizeof(Particle) == 32);

inline constexpr std::size_t kMaxParticles = 16384;

// Where in a vertex its parts lie (bytes), as SpriteCard's vertex format puts them (stdshader_dx9.dll's Spritecard draw,
// 0x180053f30, REA 2026-11-04: POSITION and COLOR, texture coordinates of 4, 4, 4, 2 and 4 floats): the position, the
// colour (its alpha in byte 3) and the third texture coordinate, whose z is the radius (the SDK's
// RenderSpriteCard: blend, rotation, radius, yaw).
struct Layout {
    std::uint32_t stride;
    std::uint32_t position;
    std::uint32_t color;
    std::uint32_t parameters;
};

// Records the sprites of a draw: its vertices, `vertexCount` of them, and 16-bit indices drawn as (first index, count)
// lists of triangles, two per sprite over its four vertices; `density` scales their coverage.
void Record(const std::uint8_t* vertices, std::uint32_t vertexCount, const Layout& layout, const std::uint16_t* indices,
    std::span<const std::pair<int, int>> primitives, float density);
// The sprites recorded since the last call.
std::vector<Particle> Take();

}
