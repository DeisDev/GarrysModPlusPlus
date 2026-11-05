#pragma once

#include "view_types.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace gmodpp::entities {

struct ModelEntity {
    int index;
    void* entity;
    void* renderable;
    // The renderable of the entity it is attached to (its move parent), or null; a held weapon's is its owner's.
    void* parent;
    std::string model;
    // Its skin family (C_BaseAnimating::m_nSkin), 0 for entities without one.
    int skin;
    // C_BaseAnimating::m_flModelScale, 1 for entities without one.
    float modelScale;
    // Entity:SetLegacyTransform: its single bone is drawn scaled even when the model is not a $staticprop.
    bool legacyTransform;
    float transform[3][4];
};

// A drawn rope (C_RopeKeyframe): the simulated nodes the game draws it through, in world space.
struct Rope {
    int index;
    // Its material's VMT path, from the model precache table.
    std::string material;
    float width;
    // Catmull-Rom points the game adds between neighbouring nodes.
    int subdivisions;
    std::vector<std::array<float, 3>> nodes;
};

void TryInitialize();
bool Ready();

// Visible studio-model entities with their model path and model-to-world transform. Renderable
// pointers are only valid until the end of the frame.
void Collect(std::vector<ModelEntity>& entities);

// Ropes the game draws whose simulation has placed their nodes.
void CollectRopes(std::vector<Rope>& ropes);

// The map's env_sun (the first one that is on, else the first), or nothing without one.
std::optional<gbuffer::EnvSun> FindSun();

// The world model path of a weapon from Collect that shows its view model, as the local player's weapons do in first
// person; empty otherwise. Only for entities known to be weapons.
std::string WeaponWorldModel(const ModelEntity& weapon);

// Bone-to-world matrices for a renderable from Collect; returns false when the entity has no bones.
bool SetupBones(void* renderable, float (*boneToWorld)[3][4], int maxBones);

// A drawn sprite entity (env_sprite and its kin, such as the manhack's eye glow) whose render mode adds its colour to
// the image: where it is drawn, its colour (m_clrRender, 0-1, times its HDR colour scale), its brightness (0-1), the area
// it covers in units and the name of the material it is drawn with.
struct GlowSprite {
    float origin[3];
    float color[3];
    float brightness;
    float area;
    std::string material;
};
void CollectGlowSprites(std::vector<GlowSprite>& sprites);

}
