#pragma once

#include "bsp.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gmodpp::studio {

struct VertexWeights {
    float weight[3];
    uint8_t bone[3];
    uint8_t count;
};

inline constexpr uint16_t kNoTexture = 0xFFFF;

struct Geometry {
    std::vector<bsp::Float3> positions;
    std::vector<bsp::Float2> uvs;
    std::vector<VertexWeights> weights;
    std::vector<uint32_t> indices;
    // Texture index of each triangle in the skin family built, or kNoTexture.
    std::vector<uint16_t> triangleTextures;
};

using Matrix3x4 = std::array<std::array<float, 4>, 3>;

// The affine transform `b`, then `a`.
Matrix3x4 Multiply(const Matrix3x4& a, const Matrix3x4& b);

// Number of bones in the model; models with more than one bone are skinned.
int BoneCount(std::span<const std::byte> mdl);

// Whether the model was compiled with $staticprop (STUDIOHDR_FLAGS_STATIC_PROP).
bool IsStaticProp(std::span<const std::byte> mdl);

// The box around the model's collision hull and view bounds, or false when the header sets neither.
bool ReadBounds(std::span<const std::byte> mdl, bsp::Float3& mins, bsp::Float3& maxs);

// The index of the bone with this name, or -1.
int BoneIndex(std::span<const std::byte> mdl, std::string_view name);

// Each bone's poseToBone matrix (bind pose to bone space).
bool ReadPoseToBone(std::span<const std::byte> mdl, std::vector<Matrix3x4>& poseToBone);

// Candidate material paths ("materials/<cdtexture><texture>.vmt") for each texture of the model.
bool ReadMaterialPaths(std::span<const std::byte> mdl, std::vector<std::vector<std::string>>& texturePaths, std::string& error);

// Builds LOD0 triangles of body group 0 with the textures of skin family `skin` (family 0 when the model has no such
// family), skipping meshes whose texture is marked in `skipTexture`.
bool BuildGeometry(std::span<const std::byte> mdl, std::span<const std::byte> vvd, std::span<const std::byte> vtx,
    const std::vector<bool>& skipTexture, Geometry& geometry, std::string& error, int skin = 0);

// The value of a VMT key, matched without regard to case, or nothing when the key is absent.
std::optional<std::string> MaterialValue(std::string_view vmt, std::string_view key);

// A VMT texture key ($basetexture, $envmapmask, ...) as a game path ("materials/<name>.vtf"), or empty when absent.
std::string MaterialTexture(std::string_view vmt, std::string_view key);

// Bytes of a VTF header needed by TextureReflectivity.
inline constexpr size_t kVtfReflectivityHeaderSize = 44;

// The average linear colour vtex stores in a VTF header.
std::optional<bsp::Float3> TextureReflectivity(std::span<const std::byte> vtfHeader);

enum class TextureFormat { Bc1, Bc2, Bc3, Bgra8, Rgba8, Bgrx8, R8 };

// One frame of a VTF from its largest mip no bigger than a limit down to 1x1, each mip in its stored layout
// (rows of 4x4 blocks for the BC formats).
struct TextureImage {
    TextureFormat format;
    uint32_t width;
    uint32_t height;
    // False when the format stores no alpha (it then reads as 1).
    bool alpha;
    // The VTF's frames (animated textures have several).
    uint32_t frames;
    // Largest first.
    std::vector<std::span<const std::byte>> mips;
    // Mips converted from a format textures do not take, which `mips` then views.
    std::vector<std::vector<std::byte>> converted;
};

// A 2D VTF's mips no bigger than `maxSize` texels on a side, of its frame `frame`, or nothing with a reason.
std::optional<TextureImage> ReadTexture(std::span<const std::byte> vtf, uint32_t maxSize, std::string& error, uint32_t frame = 0);

}
