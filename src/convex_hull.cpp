#include "convex_hull.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>
#include <unordered_map>

namespace gmodpp::hull {

namespace {

struct Vec {
    double x, y, z;
};

Vec operator-(const Vec& a, const Vec& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double Dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(const Vec& a, const Vec& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Length(const Vec& v) { return std::sqrt(Dot(v, v)); }

struct Face {
    std::array<uint32_t, 3> vertices;
    Vec normal;
    double distance;
    bool alive = true;
    // Points outside this face that no earlier face claimed.
    std::vector<uint32_t> outside;
    // The search for the faces a point sees: when this face was last checked, and whether it saw the point then.
    uint32_t visited = 0;
    bool sees = false;
};

uint64_t EdgeKey(uint32_t from, uint32_t to) { return static_cast<uint64_t>(from) << 32 | to; }

// Quickhull: a face's farthest outside point replaces the faces it sees, found by walking from that face, with a
// fan to their horizon. Adding extreme points first keeps points on edges and faces from becoming vertices.
// Faces keep counter-clockwise vertices seen from outside, so each directed edge belongs to exactly one face.
class Builder {
public:
    Builder(std::vector<Vec> points, double epsilon) : m_points(std::move(points)), m_epsilon(epsilon) {}

    bool Run()
    {
        std::array<uint32_t, 4> simplex;
        if (!Simplex(simplex))
            return false;
        const Vec& a = m_points[simplex[0]];
        const Vec& b = m_points[simplex[1]];
        const Vec& c = m_points[simplex[2]];
        const Vec& d = m_points[simplex[3]];
        const Vec centre = {(a.x + b.x + c.x + d.x) * 0.25, (a.y + b.y + c.y + d.y) * 0.25, (a.z + b.z + c.z + d.z) * 0.25};
        constexpr int kTetrahedron[4][3] = {{0, 1, 2}, {0, 3, 1}, {1, 3, 2}, {2, 3, 0}};
        for (const auto& face : kTetrahedron) {
            uint32_t v0 = simplex[face[0]];
            uint32_t v1 = simplex[face[1]];
            uint32_t v2 = simplex[face[2]];
            if (Dot(Cross(m_points[v1] - m_points[v0], m_points[v2] - m_points[v0]), centre - m_points[v0]) > 0.0)
                std::swap(v1, v2);
            if (!AddFace(v0, v1, v2))
                return false;
        }
        std::vector<uint32_t> pending;
        for (uint32_t p = 0; p < m_points.size(); ++p) {
            if (std::find(simplex.begin(), simplex.end(), p) == simplex.end())
                pending.push_back(p);
        }
        Assign(pending, 0);

        for (bool grown = true; grown;) {
            grown = false;
            for (uint32_t f = 0; f < m_faces.size(); ++f) {
                while (m_faces[f].alive && !m_faces[f].outside.empty()) {
                    if (!AddApex(f))
                        return false;
                    grown = true;
                }
            }
        }
        for (const Face& face : m_faces) {
            if (!face.alive)
                continue;
            for (int e = 0; e < 3; ++e) {
                if (!m_edges.contains(EdgeKey(face.vertices[(e + 1) % 3], face.vertices[e])))
                    return false;
            }
        }
        return true;
    }

    std::vector<Plane> Planes() const
    {
        std::vector<Plane> planes;
        for (const Face& face : m_faces) {
            if (!face.alive)
                continue;
            const bool merged = std::any_of(planes.begin(), planes.end(), [&](const Plane& plane) {
                const Vec normal = {plane.normal.x, plane.normal.y, plane.normal.z};
                return Dot(normal, face.normal) > 1.0 - 1e-6 && std::abs(plane.distance - face.distance) < m_epsilon;
            });
            if (!merged)
                planes.push_back({{static_cast<float>(face.normal.x), static_cast<float>(face.normal.y), static_cast<float>(face.normal.z)},
                    static_cast<float>(face.distance)});
        }
        return planes;
    }

private:
    // Adds the farthest point outside a face: the faces it sees, found by walking out from that face, give way to
    // a fan from their horizon to the point.
    bool AddApex(uint32_t first)
    {
        const std::vector<uint32_t>& outside = m_faces[first].outside;
        const uint32_t apex = *std::max_element(outside.begin(), outside.end(),
            [&](uint32_t l, uint32_t r) { return Height(m_faces[first], l) < Height(m_faces[first], r); });
        ++m_stamp;
        std::vector<uint32_t> visible;
        std::vector<std::pair<uint32_t, uint32_t>> horizon;
        std::vector<uint32_t> stack(1, first);
        m_faces[first].visited = m_stamp;
        m_faces[first].sees = true;
        while (!stack.empty()) {
            const uint32_t current = stack.back();
            stack.pop_back();
            visible.push_back(current);
            const auto& v = m_faces[current].vertices;
            for (int e = 0; e < 3; ++e) {
                const uint32_t from = v[e];
                const uint32_t to = v[(e + 1) % 3];
                const auto twin = m_edges.find(EdgeKey(to, from));
                if (twin == m_edges.end())
                    return false;
                Face& neighbour = m_faces[twin->second];
                if (neighbour.visited != m_stamp) {
                    neighbour.visited = m_stamp;
                    neighbour.sees = Height(neighbour, apex) > m_epsilon;
                    if (neighbour.sees)
                        stack.push_back(twin->second);
                }
                if (!neighbour.sees)
                    horizon.emplace_back(from, to);
            }
        }
        std::vector<uint32_t> orphans;
        for (uint32_t face : visible) {
            Face& removed = m_faces[face];
            for (uint32_t p : removed.outside) {
                if (p != apex)
                    orphans.push_back(p);
            }
            removed.outside.clear();
            removed.alive = false;
            for (int e = 0; e < 3; ++e)
                m_edges.erase(EdgeKey(removed.vertices[e], removed.vertices[(e + 1) % 3]));
        }
        const auto firstNew = static_cast<uint32_t>(m_faces.size());
        for (const auto& [from, to] : horizon) {
            if (!AddFace(from, to, apex))
                return false;
        }
        Assign(orphans, firstNew);
        return true;
    }

    double Height(const Face& face, uint32_t point) const { return Dot(face.normal, m_points[point]) - face.distance; }

    // Gives each point to a live face it lies outside, trying the faces from `firstFace` on first; points outside
    // none are inside the hull.
    void Assign(const std::vector<uint32_t>& points, uint32_t firstFace)
    {
        const auto claim = [&](uint32_t f, uint32_t p) {
            if (!m_faces[f].alive || Height(m_faces[f], p) <= m_epsilon)
                return false;
            m_faces[f].outside.push_back(p);
            return true;
        };
        for (uint32_t p : points) {
            bool claimed = false;
            for (uint32_t f = firstFace; f < m_faces.size() && !claimed; ++f)
                claimed = claim(f, p);
            for (uint32_t f = 0; f < firstFace && !claimed; ++f)
                claimed = claim(f, p);
        }
    }

    bool Simplex(std::array<uint32_t, 4>& simplex) const
    {
        const auto farthest = [&](auto&& measure) {
            uint32_t best = 0;
            double bestValue = -1.0;
            for (uint32_t i = 0; i < m_points.size(); ++i) {
                const double value = measure(m_points[i]);
                if (value > bestValue) {
                    bestValue = value;
                    best = i;
                }
            }
            return std::pair(best, bestValue);
        };
        simplex[0] = farthest([](const Vec& p) { return -p.x; }).first;
        const Vec origin = m_points[simplex[0]];
        double extent;
        std::tie(simplex[1], extent) = farthest([&](const Vec& p) { return Length(p - origin); });
        if (extent < m_epsilon)
            return false;
        const Vec axis = m_points[simplex[1]] - origin;
        std::tie(simplex[2], extent) = farthest([&](const Vec& p) { return Length(Cross(p - origin, axis)) / Length(axis); });
        if (extent < m_epsilon)
            return false;
        Vec normal = Cross(axis, m_points[simplex[2]] - origin);
        const double length = Length(normal);
        normal = {normal.x / length, normal.y / length, normal.z / length};
        std::tie(simplex[3], extent) = farthest([&](const Vec& p) { return std::abs(Dot(p - origin, normal)); });
        return extent >= m_epsilon;
    }

    bool AddFace(uint32_t a, uint32_t b, uint32_t c)
    {
        Vec normal = Cross(m_points[b] - m_points[a], m_points[c] - m_points[a]);
        const double length = Length(normal);
        if (length <= 0.0)
            return false;
        normal = {normal.x / length, normal.y / length, normal.z / length};
        const auto index = static_cast<uint32_t>(m_faces.size());
        for (const auto& [from, to] : {std::pair(a, b), std::pair(b, c), std::pair(c, a)}) {
            if (!m_edges.emplace(EdgeKey(from, to), index).second)
                return false;
        }
        m_faces.push_back({{a, b, c}, normal, Dot(normal, m_points[a])});
        return true;
    }

    std::vector<Vec> m_points;
    double m_epsilon;
    uint32_t m_stamp = 0;
    std::vector<Face> m_faces;
    std::unordered_map<uint64_t, uint32_t> m_edges;
};

}

std::vector<Plane> Build(std::span<const bsp::Float3> points)
{
    if (points.size() < 4)
        return {};
    std::vector<Vec> vectors;
    vectors.reserve(points.size());
    Vec low = {points[0].x, points[0].y, points[0].z};
    Vec high = low;
    for (const bsp::Float3& p : points) {
        vectors.push_back({p.x, p.y, p.z});
        low = {std::min<double>(low.x, p.x), std::min<double>(low.y, p.y), std::min<double>(low.z, p.z)};
        high = {std::max<double>(high.x, p.x), std::max<double>(high.y, p.y), std::max<double>(high.z, p.z)};
    }
    const double size = Length(high - low);
    const double epsilon = std::max(size * 1e-5, 1e-3);
    Builder builder(std::move(vectors), epsilon);
    if (!builder.Run())
        return {};
    std::vector<Plane> planes = builder.Planes();
    // A hull that leaves an input point outside was built wrongly.
    const float tolerance = static_cast<float>(std::max(epsilon * 10.0, 0.01));
    for (const bsp::Float3& p : points) {
        if (Distance(planes, p) > tolerance)
            return {};
    }
    return planes;
}

float Distance(std::span<const Plane> planes, const bsp::Float3& point)
{
    float distance = -INFINITY;
    for (const Plane& plane : planes)
        distance = std::max(distance, plane.normal.x * point.x + plane.normal.y * point.y + plane.normal.z * point.z - plane.distance);
    return distance;
}

}
