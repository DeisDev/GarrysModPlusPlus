#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace gmodpp::light_queries {

// Answers the engine's lighting queries (IVEngineClient::ComputeLighting and GetLightForPoint, which particles
// and client effects light themselves with) from the ray-traced irradiance volume, points outside it from its nearest
// probes; any query while the volume is not being filled gets the engine's baked lighting.
void TryInstall();

// The volume as the GPU last filled it: the cells from `origin` on, `grid` probes `spacing` apart, stored at cell
// modulo the grid size, x fastest. Each texel holds the red, green and blue (a, b.x, b.y, b.z) coefficients of
// a + b.n as half floats.
void Publish(const std::array<int, 3>& origin, const std::array<uint32_t, 3>& grid, float spacing, std::span<const uint16_t> texels);

// Stops answering until the next Publish (map change).
void Clear();

}
