#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace gmodpp::glow_draws {

// Glow the game draws (Settings::effectLights): the additive draws of the main view's translucent pass (sprites,
// old-style particles, beams, Lua's render.DrawSprite) are read on the CPU as they are drawn and gathered into lights for
// the next main view, as the pass is drawn after its own view is lit.

// What a material adds to the image per unit of vertex colour where it is drawn additively: the mean of its base
// texture's linear colour, times its alpha (`alphaWeighted`, for SRC_ALPHA, ONE blending) or not (`plain`, for ONE, ONE);
// whether its VMT sets $additive; `known` false when its texture could not be read.
struct MaterialGlow {
    float alphaWeighted[3] = {};
    float plain[3] = {};
    bool additive = false;
    bool known = false;
};

// A material's glow by its name as the game gives it ("effects/muzzleflash1", with or without "materials/" and ".vmt"),
// read once (render thread).
const MaterialGlow& GlowOf(std::string_view materialName);

// Where in a vertex its parts lie (bytes): the position, the colour (B8G8R8A8; kAbsent for none) and SpriteCard's third
// texture coordinate, whose z is a sprite's radius (kAbsent for other draws, whose vertices are the corners).
inline constexpr std::uint32_t kAbsent = ~0u;
struct Layout {
    std::uint32_t stride;
    std::uint32_t position;
    std::uint32_t color;
    std::uint32_t parameters;
};

// Records the glow of an additive draw: its vertices, `vertexCount` of them, drawn as (first index, count) lists of
// triangles (`strip`: triangle strips) through 16-bit `indices` (null: the vertices in order); `color` is what its
// material adds per unit of vertex colour (MaterialGlow), times the vertices' alpha where `vertexAlpha` (SRC_ALPHA, ONE).
void Record(const std::uint8_t* vertices, std::uint32_t vertexCount, const Layout& layout, const std::uint16_t* indices,
    std::span<const std::pair<int, int>> primitives, bool strip, const float (&color)[3], bool vertexAlpha);

// How bright the light of drawn glow is for the brightness it is drawn with: a calibration, as glows are drawn at what
// reads well on screen, not at their real brightness (flames are far brighter than the white they are drawn at). Glowing
// particle systems (src/effect_lights.cpp) take the same.
inline constexpr float kLightPerGlow = 64.0f;

// A cluster of the glow recorded since the last call: what it adds in all (the summed emission times area, linear), its
// luminance-weighted centre and spread.
struct Glow {
    float color[3];
    float centre[3];
    float spread;
};
// The glow recorded since the last call, gathered into clusters.
std::vector<Glow> Take();

}
