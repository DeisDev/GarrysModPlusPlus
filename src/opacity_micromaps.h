#pragma once

#include "bsp.h"

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace gmodpp::omm {

// The alpha of a texture's largest uploaded mip, as the alpha tests in shaders/scene.hlsli read it.
struct AlphaImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> alpha;
};

// Decodes the alpha of one mip in its stored layout; nothing for formats without alpha.
std::optional<AlphaImage> DecodeAlpha(DXGI_FORMAT format, uint32_t width, uint32_t height, std::span<const std::byte> mip);

// An alpha-tested or translucent triangle: its Scene::PackAlpha or Scene::PackTranslucent entry and texture coordinates.
struct AlphaTriangle {
    uint32_t packed;
    bsp::Float2 uvs[3];
};

// 4-state opacity micromaps for a model's alpha triangles: the micro-triangles a texel of the alpha test decides
// are opaque or transparent, the others unknown (resolved by the ray query loop). Triangles with one state
// everywhere get a special index instead of a micromap.
struct Micromaps {
    std::vector<std::byte> data;
    std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_DESC> descs;
    std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> histogram;
    // One per triangle: an index into descs or a D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX.
    std::vector<int32_t> indices;
};

Micromaps Bake(std::span<const AlphaTriangle> triangles, const std::unordered_map<uint32_t, AlphaImage>& images);

}
