#include "glow_patches.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <tuple>
#include <unordered_map>

namespace gmodpp::glow {

namespace {

constexpr float kPi = 3.14159265358979f;

bsp::Float3 Sub(const bsp::Float3& a, const bsp::Float3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
bsp::Float3 Add(const bsp::Float3& a, const bsp::Float3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
bsp::Float3 Scale(const bsp::Float3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
bsp::Float3 Cross(const bsp::Float3& a, const bsp::Float3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(const bsp::Float3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
float Luminance(const bsp::Float3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
float Component(const bsp::Float3& a, int axis) { return axis == 0 ? a.x : axis == 1 ? a.y : a.z; }

struct GlowTriangle {
    uint32_t index;
    float area;
    bsp::Float3 centroid;
};

struct PositionHash {
    size_t operator()(const bsp::Float3& p) const
    {
        const size_t x = std::bit_cast<uint32_t>(p.x);
        const size_t y = std::bit_cast<uint32_t>(p.y);
        const size_t z = std::bit_cast<uint32_t>(p.z);
        return x * 73856093u ^ y * 19349663u ^ z * 83492791u;
    }
};

struct PositionEqual {
    bool operator()(const bsp::Float3& a, const bsp::Float3& b) const { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

uint32_t Root(std::vector<uint32_t>& parent, uint32_t x)
{
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

// The triangles' corners, for bounds.
template <class Visit>
void ForCorners(std::span<const bsp::Float3> positions, std::span<const uint32_t> indices, std::span<const GlowTriangle> triangles, Visit visit)
{
    for (const GlowTriangle& triangle : triangles) {
        for (int c = 0; c < 3; ++c)
            visit(positions[indices[static_cast<size_t>(triangle.index) * 3 + c]]);
    }
}

// Cuts `triangles` (one connected piece) into ranges whose corners span at most kMaxPatchSize, halving the longest side
// at the median of the area.
void Cut(std::span<const bsp::Float3> positions, std::span<const uint32_t> indices, std::span<GlowTriangle> triangles,
    std::vector<std::span<GlowTriangle>>& pieces)
{
    bsp::Float3 low = {1e30f, 1e30f, 1e30f};
    bsp::Float3 high = {-1e30f, -1e30f, -1e30f};
    ForCorners(positions, indices, triangles, [&](const bsp::Float3& p) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    });
    const bsp::Float3 extent = Sub(high, low);
    const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
    if (triangles.size() < 2 || Component(extent, axis) <= kMaxPatchSize) {
        pieces.push_back(triangles);
        return;
    }
    std::sort(triangles.begin(), triangles.end(), [&](const GlowTriangle& a, const GlowTriangle& b) { return Component(a.centroid, axis) < Component(b.centroid, axis); });
    float total = 0.0f;
    for (const GlowTriangle& triangle : triangles)
        total += triangle.area;
    size_t middle = 1;
    for (float before = triangles[0].area; middle + 1 < triangles.size() && before < 0.5f * total; ++middle)
        before += triangles[middle].area;
    Cut(positions, indices, triangles.first(middle), pieces);
    Cut(positions, indices, triangles.subspan(middle), pieces);
}

Patch MakePatch(std::span<const bsp::Float3> positions, std::span<const uint32_t> indices, std::span<const bsp::Float3> emission,
    std::span<const uint32_t> materials, std::span<const studio::VertexWeights> weights, std::span<const GlowTriangle> triangles)
{
    Patch patch = {};
    bsp::Float3 weighted = {};
    std::vector<float> boneWeight;
    for (const GlowTriangle& triangle : triangles) {
        const size_t t = triangle.index;
        const bsp::Float3& p0 = positions[indices[t * 3]];
        // Source's front faces: cross(p1 - p0, p2 - p0) points out of the back.
        const bsp::Float3 back = Cross(Sub(positions[indices[t * 3 + 1]], p0), Sub(positions[indices[t * 3 + 2]], p0));
        patch.axis = Sub(patch.axis, Scale(back, 0.5f));
        patch.area += triangle.area;
        patch.material = materials[t];
        patch.centre = Add(patch.centre, Scale(triangle.centroid, triangle.area));
        weighted = Add(weighted, Scale(emission[t], triangle.area));
        for (int c = 0; c < 3 && !weights.empty(); ++c) {
            const studio::VertexWeights& vertex = weights[indices[t * 3 + c]];
            for (uint8_t b = 0; b < vertex.count && b < 3; ++b) {
                if (vertex.bone[b] >= boneWeight.size())
                    boneWeight.resize(vertex.bone[b] + 1, 0.0f);
                boneWeight[vertex.bone[b]] += vertex.weight[b] * triangle.area;
            }
        }
    }
    patch.centre = Scale(patch.centre, 1.0f / patch.area);
    patch.emission = Scale(weighted, 1.0f / patch.area);
    std::vector<uint32_t> order(boneWeight.size());
    for (uint32_t b = 0; b < order.size(); ++b)
        order[b] = b;
    const size_t bones = std::min(order.size(), Patch::kBones);
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(bones), order.end(),
        [&](uint32_t a, uint32_t b) { return boneWeight[a] > boneWeight[b]; });
    float total = 0.0f;
    for (size_t b = 0; b < bones; ++b)
        total += boneWeight[order[b]];
    for (size_t b = 0; b < Patch::kBones; ++b) {
        const bool used = b < bones && total > 0.0f;
        patch.bones[b] = used ? order[b] : 0u;
        patch.boneWeights[b] = used ? boneWeight[order[b]] / total : (b == 0 && (bones == 0 || total <= 0.0f) ? 1.0f : 0.0f);
    }
    patch.radius = 0.5f;
    ForCorners(positions, indices, triangles, [&](const bsp::Float3& p) { patch.radius = std::max(patch.radius, Length(Sub(p, patch.centre))); });
    return patch;
}

}

std::vector<Patch> BuildPatches(std::span<const bsp::Float3> positions, std::span<const uint32_t> indices, std::span<const bsp::Float3> emission,
    std::span<const uint32_t> materials, std::span<const studio::VertexWeights> weights)
{
    const size_t triangleCount = std::min({indices.size() / 3, emission.size(), materials.size()});
    std::vector<GlowTriangle> glowing;
    for (size_t t = 0; t < triangleCount; ++t) {
        if (Luminance(emission[t]) <= 0.0f)
            continue;
        const bsp::Float3& p0 = positions[indices[t * 3]];
        const bsp::Float3& p1 = positions[indices[t * 3 + 1]];
        const bsp::Float3& p2 = positions[indices[t * 3 + 2]];
        const float area = 0.5f * Length(Cross(Sub(p1, p0), Sub(p2, p0)));
        if (area > 0.0f)
            glowing.push_back({static_cast<uint32_t>(t), area, Scale(Add(Add(p0, p1), p2), 1.0f / 3.0f)});
    }
    // Triangles of one material that share a corner belong together; a model's vertices are split at texture seams, so
    // corners are matched by position.
    std::vector<uint32_t> parent(glowing.size());
    for (uint32_t i = 0; i < parent.size(); ++i)
        parent[i] = i;
    std::unordered_map<bsp::Float3, std::vector<uint32_t>, PositionHash, PositionEqual> corners;
    for (uint32_t i = 0; i < glowing.size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            std::vector<uint32_t>& sharing = corners[positions[indices[static_cast<size_t>(glowing[i].index) * 3 + c]]];
            for (const uint32_t other : sharing) {
                if (materials[glowing[other].index] == materials[glowing[i].index])
                    parent[Root(parent, i)] = Root(parent, other);
            }
            sharing.push_back(i);
        }
    }
    std::unordered_map<uint32_t, std::vector<GlowTriangle>> pieces;
    for (uint32_t i = 0; i < glowing.size(); ++i)
        pieces[Root(parent, i)].push_back(glowing[i]);
    std::vector<Patch> patches;
    for (auto& [root, triangles] : pieces) {
        std::vector<std::span<GlowTriangle>> cut;
        Cut(positions, indices, triangles, cut);
        for (const std::span<GlowTriangle> piece : cut)
            patches.push_back(MakePatch(positions, indices, emission, materials, weights, piece));
    }
    // A stable order, so a patch keeps its number (its lights' keys) from load to load.
    std::sort(patches.begin(), patches.end(), [](const Patch& a, const Patch& b) {
        return std::tie(a.centre.x, a.centre.y, a.centre.z) < std::tie(b.centre.x, b.centre.y, b.centre.z);
    });
    return patches;
}

FrameLight Place(const Patch& patch, const studio::Matrix3x4& transform, float emissiveScale, uint64_t key, uint32_t flags)
{
    const auto& m = transform;
    const auto apply = [&](const bsp::Float3& v, bool point) {
        return bsp::Float3{m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + (point ? m[0][3] : 0.0f),
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + (point ? m[1][3] : 0.0f), m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + (point ? m[2][3] : 0.0f)};
    };
    // Areas (and area-weighted normals) follow the linear part's cofactor matrix; lengths its cube root of the
    // determinant's size.
    const bsp::Float3 columns[3] = {{m[0][0], m[1][0], m[2][0]}, {m[0][1], m[1][1], m[2][1]}, {m[0][2], m[1][2], m[2][2]}};
    const bsp::Float3 cofactor[3] = {Cross(columns[1], columns[2]), Cross(columns[2], columns[0]), Cross(columns[0], columns[1])};
    const bsp::Float3 axis = Add(Add(Scale(cofactor[0], patch.axis.x), Scale(cofactor[1], patch.axis.y)), Scale(cofactor[2], patch.axis.z));
    const float determinant = std::fabs(columns[0].x * cofactor[0].x + columns[0].y * cofactor[0].y + columns[0].z * cofactor[0].z);
    const float lengthScale = std::cbrt(determinant);
    const float area = patch.area * lengthScale * lengthScale;
    const float axisLength = Length(axis);
    const float isotropic = std::max(area - axisLength, 0.0f) * 0.25f;
    const float radius = patch.radius * lengthScale;
    FrameLight light;
    light.kind = FrameLight::Kind::Glow;
    // Half its radius in front of a flat patch, so shadow rays towards it stop short of the patch itself.
    const bsp::Float3 origin = Add(apply(patch.centre, true), axisLength > 0.0f ? Scale(axis, 0.5f * radius / axisLength) : bsp::Float3{});
    light.origin[0] = origin.x;
    light.origin[1] = origin.y;
    light.origin[2] = origin.z;
    light.color[0] = patch.emission.x;
    light.color[1] = patch.emission.y;
    light.color[2] = patch.emission.z;
    light.direction[0] = axis.x;
    light.direction[1] = axis.y;
    light.direction[2] = axis.z;
    light.innerCosine = isotropic;
    light.size = radius;
    // Where its irradiance facing it falls to the threshold map lights are binned at (LightIrradiance's glow branch).
    light.radius = std::max(std::sqrt(Luminance(patch.emission) * emissiveScale * (axisLength + isotropic) / (kPi * kLightThreshold)), radius);
    light.flags = flags;
    light.key = key;
    return light;
}

float Power(const Patch& patch, const studio::Matrix3x4& transform)
{
    const auto& m = transform;
    const float determinant = std::fabs(m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]));
    const float lengthScale = std::cbrt(determinant);
    return Luminance(patch.emission) * patch.area * lengthScale * lengthScale;
}

}
