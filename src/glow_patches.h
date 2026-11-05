#pragma once

#include "bsp.h"
#include "scene.h"
#include "studio_model.h"

#include <cstdint>
#include <span>
#include <vector>

namespace gmodpp::glow {

// A glowing patch of a model: connected self-illuminated triangles of one material, cut into pieces at most
// kMaxPatchSize across, in the model's bind pose. A model's moving instances light the scene with its patches as area
// lights (FrameLight::Kind::Glow).
struct Patch {
    // Area-weighted centre and mean emission (as HitEmission in shaders/scene.hlsli gives it).
    bsp::Float3 centre;
    bsp::Float3 emission;
    // The triangles' areas times their front normals, summed, and their area.
    bsp::Float3 axis;
    float area;
    // The sphere around `centre` that holds the patch.
    float radius;
    // The bones that move the patch most and their shares, summing to 1 (skinned models), blended as the skinning blends
    // its vertices.
    static constexpr size_t kBones = 4;
    uint32_t bones[kBones];
    float boneWeights[kBones];
    // The `materials` value its triangles share, and whether its glow follows its material's $selfillumtint (set by the
    // caller; an $emissiveblend layer's does not).
    uint32_t material;
    bool tinted;
};

inline constexpr float kMaxPatchSize = 16.0f;

// A model's patches: `emission` is each triangle's (zero where it does not glow), `materials` its scene material, both
// in the order of `indices`; `weights` are the vertices' bone weights (empty for models with one bone).
std::vector<Patch> BuildPatches(std::span<const bsp::Float3> positions, std::span<const uint32_t> indices, std::span<const bsp::Float3> emission,
    std::span<const uint32_t> materials, std::span<const studio::VertexWeights> weights);

// The light of a patch where `transform` (its model's space to the world) puts it. `emissiveScale` (how strongly glowing
// surfaces light others) sets how far it reaches.
FrameLight Place(const Patch& patch, const studio::Matrix3x4& transform, float emissiveScale, uint64_t key, uint32_t flags);

// The light a patch gives in all, its emission's luminance times its area, scaled by `transform`'s area scale.
float Power(const Patch& patch, const studio::Matrix3x4& transform);

}
