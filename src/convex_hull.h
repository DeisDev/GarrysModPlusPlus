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

// The triangles of the convex hull of `points`, three positions each; empty when it could not be built.
std::vector<bsp::Float3> Triangles(std::span<const bsp::Float3> points);

}
