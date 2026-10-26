#pragma once

#include "bsp.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace gmodpp::studio {

struct VertexWeights {
    float weight[3];
    uint8_t bone[3];
    uint8_t count;
};

struct Geometry {
    std::vector<bsp::Float3> positions;
    std::vector<VertexWeights> weights;
    std::vector<uint32_t> indices;
};

using Matrix3x4 = std::array<std::array<float, 4>, 3>;

// Number of bones in the model; models with more than one bone are skinned.
int BoneCount(std::span<const std::byte> mdl);

// Each bone's poseToBone matrix (bind pose to bone space).
bool ReadPoseToBone(std::span<const std::byte> mdl, std::vector<Matrix3x4>& poseToBone);

// Candidate material paths ("materials/<cdtexture><texture>.vmt") for each texture of the model.
bool ReadMaterialPaths(std::span<const std::byte> mdl, std::vector<std::vector<std::string>>& texturePaths, std::string& error);

// Builds LOD0 triangles of body group 0, skipping meshes whose (skin 0) texture is marked in `skipTexture`.
bool BuildGeometry(std::span<const std::byte> mdl, std::span<const std::byte> vvd, std::span<const std::byte> vtx,
    const std::vector<bool>& skipTexture, Geometry& geometry, std::string& error);

// True when a VMT asks for alpha testing or translucency, so the surface must not cast an opaque shadow.
bool MaterialIsSeeThrough(std::string_view vmt);

}
