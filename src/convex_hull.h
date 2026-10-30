#pragma once

#include "bsp.h"

#include <span>
#include <vector>

namespace gmodpp::hull {

// A face plane of a convex hull: points with dot(normal, point) <= distance are on its inner side.
struct Plane {
    bsp::Float3 normal;
    float distance;
};

// The planes of the convex hull of `points`, coplanar faces merged; empty when the points span no volume or the
// hull could not be built.
std::vector<Plane> Build(std::span<const bsp::Float3> points);

// The triangles of the convex hull of `points`, three positions each; empty when it could not be built.
std::vector<bsp::Float3> Triangles(std::span<const bsp::Float3> points);

// How far `point` lies outside the hull's planes; negative inside.
float Distance(std::span<const Plane> planes, const bsp::Float3& point);

}
