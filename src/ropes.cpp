#include "ropes.h"

#include <algorithm>
#include <cmath>

namespace gmodpp::ropes {

namespace {

constexpr uint32_t kSides = 6;
constexpr float kTwoPi = 6.28318530718f;
constexpr float kMinSegment = 0.01f;

bsp::Float3 Add(const bsp::Float3& a, const bsp::Float3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
bsp::Float3 Scale(const bsp::Float3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
bsp::Float3 Cross(const bsp::Float3& a, const bsp::Float3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(const bsp::Float3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

// p1 * w1 + p2 * w2 + p3 * w3 + p4 * w4.
bsp::Float3 Blend(const bsp::Float3& p1, const bsp::Float3& p2, const bsp::Float3& p3, const bsp::Float3& p4, float w1, float w2, float w3, float w4)
{
    return Add(Add(Scale(p1, w1), Scale(p2, w2)), Add(Scale(p3, w3), Scale(p4, w4)));
}

}

Prism UnitPrism()
{
    Prism prism;
    for (uint32_t ring = 0; ring < 2; ++ring) {
        for (uint32_t side = 0; side < kSides; ++side) {
            const float angle = kTwoPi * static_cast<float>(side) / static_cast<float>(kSides);
            prism.positions.push_back({std::cos(angle), std::sin(angle), static_cast<float>(ring)});
        }
    }
    for (uint32_t side = 0; side < kSides; ++side) {
        const uint32_t next = (side + 1) % kSides;
        prism.indices.insert(prism.indices.end(), {side, next, kSides + side, next, kSides + next, kSides + side});
    }
    return prism;
}

std::vector<bsp::Float3> Curve(const entities::Rope& rope)
{
    std::vector<bsp::Float3> p;
    for (const std::array<float, 3>& node : rope.nodes)
        p.push_back({node[0], node[1], node[2]});
    std::vector<bsp::Float3> curve;
    if (p.empty())
        return curve;
    const size_t last = p.size() - 1;
    size_t previous = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        curve.push_back(p[i]);
        if (i == last)
            break;
        const size_t next = i + 1;
        const size_t afterNext = std::min(i + 2, last);
        // Catmull_Rom_Spline_Matrix's terms of t, t^2 and t^3 around p[i].
        const bsp::Float3 t1 = Blend(p[previous], p[i], p[next], p[afterNext], -0.5f, 0.0f, 0.5f, 0.0f);
        const bsp::Float3 t2 = Blend(p[previous], p[i], p[next], p[afterNext], 1.0f, -2.5f, 2.0f, -0.5f);
        const bsp::Float3 t3 = Blend(p[previous], p[i], p[next], p[afterNext], -0.5f, 1.5f, -1.5f, 0.5f);
        for (int s = 0; s < rope.subdivisions; ++s) {
            const float t = static_cast<float>(s + 1) / static_cast<float>(rope.subdivisions + 1);
            curve.push_back(Add(Add(p[i], Scale(t1, t)), Add(Scale(t2, t * t), Scale(t3, t * t * t))));
        }
        previous = i;
    }
    return curve;
}

bool SegmentTransform(const bsp::Float3& a, const bsp::Float3& b, float radius, float (&transform)[3][4])
{
    const bsp::Float3 axis = {b.x - a.x, b.y - a.y, b.z - a.z};
    const float length = Length(axis);
    if (length < kMinSegment)
        return false;
    const bsp::Float3 direction = Scale(axis, 1.0f / length);
    const bsp::Float3 helper = std::fabs(direction.z) < 0.9f ? bsp::Float3{0.0f, 0.0f, 1.0f} : bsp::Float3{1.0f, 0.0f, 0.0f};
    const bsp::Float3 u = Scale(Cross(helper, direction), 1.0f / Length(Cross(helper, direction)));
    const bsp::Float3 v = Cross(direction, u);
    const bsp::Float3 start = Add(a, Scale(direction, -radius));
    const bsp::Float3 columns[4] = {Scale(u, radius), Scale(v, radius), Scale(direction, length + 2.0f * radius), start};
    for (int c = 0; c < 4; ++c) {
        transform[0][c] = columns[c].x;
        transform[1][c] = columns[c].y;
        transform[2][c] = columns[c].z;
    }
    return true;
}

}
