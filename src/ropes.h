#pragma once

#include "bsp.h"
#include "entities.h"

#include <cstdint>
#include <vector>

namespace gmodpp::ropes {

// The tube ropes are traced as: a prism of radius 1 around the z axis from z = 0 to 1, placed along each segment
// of a rope's curve by SegmentTransform.
struct Prism {
    std::vector<bsp::Float3> positions;
    std::vector<uint32_t> indices;
};
Prism UnitPrism();

// The points the game draws a rope through: its nodes with Catmull-Rom points between them (C_RopeKeyframe::BuildRope).
std::vector<bsp::Float3> Curve(const entities::Rope& rope);

// Maps the unit prism onto the segment from `a` to `b`, `radius` thick and lengthened by it at both ends so
// neighbouring segments close their joint. False for a segment too short to place.
bool SegmentTransform(const bsp::Float3& a, const bsp::Float3& b, float radius, float (&transform)[3][4]);

}
