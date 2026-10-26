#pragma once

#include <string>
#include <vector>

namespace gmodpp::entities {

struct ModelEntity {
    int index;
    void* renderable;
    std::string model;
    float transform[3][4];
};

void TryInitialize();
bool Ready();

// Visible studio-model entities with their model path and model-to-world transform. Renderable
// pointers are only valid until the end of the frame.
void Collect(std::vector<ModelEntity>& entities);

// Bone-to-world matrices for a renderable from Collect; returns false when the entity has no bones.
bool SetupBones(void* renderable, float (*boneToWorld)[3][4], int maxBones);

}
