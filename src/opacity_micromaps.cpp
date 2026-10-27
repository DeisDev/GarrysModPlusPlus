#include "opacity_micromaps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <execution>
#include <map>
#include <numeric>
#include <string>

namespace gmodpp::omm {

namespace {

// Scene::PackAlpha and Scene::PackTranslucent fields.
constexpr uint32_t kTextureMask = 0xFFFFFFu;
constexpr uint32_t kTranslucentFlag = 0x80000000u;
constexpr uint32_t kSubdivisionCap = 6;
// Micro-triangles get about this many texels each.
constexpr float kTexelsPerMicroTriangle = 2.0f;
// Micro-triangles spanning more texels than this (texture coordinates stretched over many repeats) stay unknown.
constexpr int kMaxTexels = 1024;
// The GPU's block decoders may round alpha one step away from ours, so values this close to the test are unknown.
constexpr float kAlphaMargin = 2.0f / 255.0f;

enum State : uint8_t {
    kTransparent = D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_TRANSPARENT,
    kOpaque = D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_OPAQUE,
    kUnknownTransparent = D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_UNKNOWN_TRANSPARENT,
    kUnknownOpaque = D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_UNKNOWN_OPAQUE,
};

int32_t SpecialIndex(State state)
{
    switch (state) {
    case kTransparent:
        return D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_TRANSPARENT;
    case kOpaque:
        return D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE;
    case kUnknownTransparent:
        return D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_TRANSPARENT;
    default:
        return D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE;
    }
}

// The index of the micro-triangle holding barycentrics (u, v) along the micromap's space-filling curve. From the
// reference code of the Vulkan specification's VK_EXT_opacity_micromap appendix (Khronos Group, CC BY 4.0), which
// defines the same ordering as D3D12's opacity micromaps.
uint32_t SpaceFillingCurveIndex(float u, float v, uint32_t level)
{
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const float fu = u * static_cast<float>(1u << level);
    const float fv = v * static_cast<float>(1u << level);
    uint32_t iu = static_cast<uint32_t>(fu);
    uint32_t iv = static_cast<uint32_t>(fv);
    const float uf = fu - static_cast<float>(iu);
    const float vf = fv - static_cast<float>(iv);
    if (iu >= (1u << level))
        iu = (1u << level) - 1u;
    if (iv >= (1u << level))
        iv = (1u << level) - 1u;
    const uint32_t iuv = iu + iv;
    if (iuv >= (1u << level))
        iu -= iuv - (1u << level) + 1u;
    uint32_t iw = ~(iu + iv);
    if (uf + vf >= 1.0f && iuv < (1u << level) - 1u)
        --iw;
    uint32_t b0 = ~(iu ^ iw);
    b0 &= (1u << level) - 1u;
    const uint32_t t = (iu ^ iv) & b0;
    uint32_t f = t;
    f ^= f >> 1u;
    f ^= f >> 2u;
    f ^= f >> 4u;
    f ^= f >> 8u;
    uint32_t b1 = ((f ^ iu) & ~b0) | t;
    b0 = (b0 | (b0 << 8u)) & 0x00ff00ffu;
    b0 = (b0 | (b0 << 4u)) & 0x0f0f0f0fu;
    b0 = (b0 | (b0 << 2u)) & 0x33333333u;
    b0 = (b0 | (b0 << 1u)) & 0x55555555u;
    b1 = (b1 | (b1 << 8u)) & 0x00ff00ffu;
    b1 = (b1 | (b1 << 4u)) & 0x0f0f0f0fu;
    b1 = (b1 | (b1 << 2u)) & 0x33333333u;
    b1 = (b1 | (b1 << 1u)) & 0x55555555u;
    return b0 | (b1 << 1u);
}

uint32_t Read16(const std::byte* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; }

uint64_t Read64(const std::byte* p)
{
    uint64_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

// Alpha of the 16 texels of a block, row by row.
void DecodeBlock(DXGI_FORMAT format, const std::byte* block, uint8_t (&alpha)[16])
{
    if (format == DXGI_FORMAT_BC1_UNORM) {
        const bool punchThrough = Read16(block) <= Read16(block + 2);
        const uint32_t codes = static_cast<uint32_t>(Read64(block + 4));
        for (int i = 0; i < 16; ++i)
            alpha[i] = punchThrough && ((codes >> (2 * i)) & 3u) == 3u ? 0 : 255;
    } else if (format == DXGI_FORMAT_BC2_UNORM) {
        const uint64_t bits = Read64(block);
        for (int i = 0; i < 16; ++i)
            alpha[i] = static_cast<uint8_t>(((bits >> (4 * i)) & 15u) * 17u);
    } else {
        const uint32_t a0 = static_cast<uint32_t>(block[0]);
        const uint32_t a1 = static_cast<uint32_t>(block[1]);
        uint32_t palette[8] = {a0, a1};
        if (a0 > a1) {
            for (uint32_t k = 1; k < 7; ++k)
                palette[k + 1] = ((7 - k) * a0 + k * a1 + 3) / 7;
        } else {
            for (uint32_t k = 1; k < 5; ++k)
                palette[k + 1] = ((5 - k) * a0 + k * a1 + 2) / 5;
            palette[6] = 0;
            palette[7] = 255;
        }
        const uint64_t codes = Read64(block) >> 16;
        for (int i = 0; i < 16; ++i)
            alpha[i] = static_cast<uint8_t>(palette[(codes >> (3 * i)) & 7u]);
    }
}

struct Float2 {
    float x;
    float y;
};

// Whether the texel square [x, x + 1] x [y, y + 1] meets the triangle (separating axes of the triangle's edges;
// the square's own axes are covered by the caller's bounding box).
bool TexelMeets(const Float2 (&t)[3], float x, float y)
{
    const float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[1].y - t[0].y) * (t[2].x - t[0].x);
    if (area == 0.0f)
        return true;
    for (int e = 0; e < 3; ++e) {
        const Float2& p = t[e];
        const Float2& q = t[(e + 1) % 3];
        const float nx = (q.y - p.y) * area;
        const float ny = (p.x - q.x) * area;
        // Inside the triangle the edge function has the sign of the area; a square entirely on the other side is apart.
        const float c0 = (x - p.x) * nx + (y - p.y) * ny;
        const float c1 = (x + 1.0f - p.x) * nx + (y - p.y) * ny;
        const float c2 = (x - p.x) * nx + (y + 1.0f - p.y) * ny;
        const float c3 = (x + 1.0f - p.x) * nx + (y + 1.0f - p.y) * ny;
        if (c0 > 0.0f && c1 > 0.0f && c2 > 0.0f && c3 > 0.0f)
            return false;
    }
    return true;
}

struct Test {
    float reference;
    bool translucent;
};

// The state of a micro-triangle with these corners in texel space: known where every texel it touches agrees.
State Classify(const AlphaImage& image, const Float2 (&corners)[3], const Test& test)
{
    const float minX = std::floor(std::min({corners[0].x, corners[1].x, corners[2].x}));
    const float maxX = std::floor(std::max({corners[0].x, corners[1].x, corners[2].x}));
    const float minY = std::floor(std::min({corners[0].y, corners[1].y, corners[2].y}));
    const float maxY = std::floor(std::max({corners[0].y, corners[1].y, corners[2].y}));
    if ((maxX - minX + 1.0f) * (maxY - minY + 1.0f) > static_cast<float>(kMaxTexels))
        return kUnknownTransparent;
    int opaque = 0;
    int transparent = 0;
    int unknown = 0;
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    for (float y = minY; y <= maxY; y += 1.0f) {
        for (float x = minX; x <= maxX; x += 1.0f) {
            if (!TexelMeets(corners, x, y))
                continue;
            const int tx = ((static_cast<int>(x) % width) + width) % width;
            const int ty = ((static_cast<int>(y) % height) + height) % height;
            const float alpha = static_cast<float>(image.alpha[static_cast<size_t>(ty) * width + tx]) / 255.0f;
            if (test.translucent) {
                const float opacity = alpha * test.reference;
                if (alpha == 0.0f || test.reference == 0.0f)
                    ++transparent;
                else if (opacity >= 0.5f + kAlphaMargin)
                    ++opaque;
                else
                    ++unknown;
            } else if (alpha >= test.reference + kAlphaMargin) {
                ++opaque;
            } else if (alpha < test.reference - kAlphaMargin) {
                ++transparent;
            } else {
                ++unknown;
            }
        }
    }
    // Translucent surfaces need the shader wherever light gets partly through, so they are never known opaque.
    if (test.translucent)
        return opaque + unknown == 0 && transparent > 0 ? kTransparent : opaque > transparent + unknown ? kUnknownOpaque : kUnknownTransparent;
    if (unknown == 0 && transparent == 0 && opaque > 0)
        return kOpaque;
    if (unknown == 0 && opaque == 0 && transparent > 0)
        return kTransparent;
    return opaque >= transparent ? kUnknownOpaque : kUnknownTransparent;
}

// One triangle's micromap at `level`: 2 bits per micro-triangle, in curve order, least significant bits first.
std::vector<uint8_t> BakeTriangle(const AlphaImage& image, const AlphaTriangle& triangle, const Test& test, uint32_t level)
{
    const uint32_t count = 1u << (2 * level);
    std::vector<uint8_t> states(count, kUnknownTransparent);
    const uint32_t n = 1u << level;
    const float scale = 1.0f / static_cast<float>(n);
    const float w = static_cast<float>(image.width);
    const float h = static_cast<float>(image.height);
    auto texel = [&](float u, float v) {
        const bsp::Float2* uv = triangle.uvs;
        return Float2{(uv[0].u + u * (uv[1].u - uv[0].u) + v * (uv[2].u - uv[0].u)) * w, (uv[0].v + u * (uv[1].v - uv[0].v) + v * (uv[2].v - uv[0].v)) * h};
    };
    for (uint32_t j = 0; j < n; ++j) {
        for (uint32_t i = 0; i + j < n; ++i) {
            const float u = static_cast<float>(i) * scale;
            const float v = static_cast<float>(j) * scale;
            const Float2 upright[3] = {texel(u, v), texel(u + scale, v), texel(u, v + scale)};
            states[SpaceFillingCurveIndex(u + scale / 3.0f, v + scale / 3.0f, level)] = Classify(image, upright, test);
            if (i + j + 1 < n) {
                const Float2 inverted[3] = {texel(u + scale, v), texel(u, v + scale), texel(u + scale, v + scale)};
                states[SpaceFillingCurveIndex(u + scale * 2.0f / 3.0f, v + scale * 2.0f / 3.0f, level)] = Classify(image, inverted, test);
            }
        }
    }
    return states;
}

uint32_t LevelFor(const AlphaImage& image, const AlphaTriangle& triangle)
{
    const bsp::Float2* uv = triangle.uvs;
    const float texels = std::fabs((uv[1].u - uv[0].u) * (uv[2].v - uv[0].v) - (uv[1].v - uv[0].v) * (uv[2].u - uv[0].u)) * 0.5f *
        static_cast<float>(image.width) * static_cast<float>(image.height);
    if (!(texels > kTexelsPerMicroTriangle))
        return 0;
    return std::min(kSubdivisionCap, static_cast<uint32_t>(std::ceil(std::log2(texels / kTexelsPerMicroTriangle) * 0.5f)));
}

}

std::optional<AlphaImage> DecodeAlpha(DXGI_FORMAT format, uint32_t width, uint32_t height, std::span<const std::byte> mip)
{
    AlphaImage image;
    image.width = width;
    image.height = height;
    image.alpha.resize(static_cast<size_t>(width) * height);
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        if (mip.size() < image.alpha.size() * 4)
            return std::nullopt;
        for (size_t i = 0; i < image.alpha.size(); ++i)
            image.alpha[i] = static_cast<uint8_t>(mip[i * 4 + 3]);
        return image;
    }
    if (format != DXGI_FORMAT_BC1_UNORM && format != DXGI_FORMAT_BC2_UNORM && format != DXGI_FORMAT_BC3_UNORM)
        return std::nullopt;
    const size_t blockBytes = format == DXGI_FORMAT_BC1_UNORM ? 8 : 16;
    const uint32_t blocksX = std::max(1u, (width + 3) / 4);
    const uint32_t blocksY = std::max(1u, (height + 3) / 4);
    if (mip.size() < blocksX * blocksY * blockBytes)
        return std::nullopt;
    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            uint8_t alpha[16];
            DecodeBlock(format, mip.data() + (static_cast<size_t>(by) * blocksX + bx) * blockBytes, alpha);
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4 + i % 4;
                const uint32_t y = by * 4 + i / 4;
                if (x < width && y < height)
                    image.alpha[static_cast<size_t>(y) * width + x] = alpha[i];
            }
        }
    }
    return image;
}

Micromaps Bake(std::span<const AlphaTriangle> triangles, const std::unordered_map<uint32_t, AlphaImage>& images)
{
    Micromaps result;
    result.indices.resize(triangles.size());
    // Models repeat the same cards (grass, leaves): equal triangles share one micromap.
    std::map<std::string, size_t> unique;
    std::vector<size_t> firstOf;
    std::vector<size_t> slot(triangles.size(), SIZE_MAX);
    for (size_t t = 0; t < triangles.size(); ++t) {
        const AlphaTriangle& triangle = triangles[t];
        const uint32_t view = triangle.packed & kTextureMask;
        const bool translucent = (triangle.packed & kTranslucentFlag) != 0;
        const float reference = static_cast<float>((triangle.packed >> 24) & 0x7Fu) / 127.0f;
        const auto image = images.find(view);
        if (image == images.end()) {
            // A constant opacity, or an alpha test whose texture could not be read: the shader decides everywhere.
            result.indices[t] = SpecialIndex(translucent && reference == 0.0f ? kTransparent : translucent && reference < 0.5f ? kUnknownTransparent : kUnknownOpaque);
            continue;
        }
        std::string key(reinterpret_cast<const char*>(&triangle), sizeof(triangle));
        const auto [it, inserted] = unique.emplace(std::move(key), firstOf.size());
        if (inserted)
            firstOf.push_back(t);
        slot[t] = it->second;
    }

    struct Baked {
        uint32_t level = 0;
        std::vector<uint8_t> states;
    };
    std::vector<Baked> baked(firstOf.size());
    std::vector<size_t> order(firstOf.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::for_each(std::execution::par, order.begin(), order.end(), [&](size_t u) {
        const AlphaTriangle& triangle = triangles[firstOf[u]];
        const AlphaImage& image = images.at(triangle.packed & kTextureMask);
        const Test test = {static_cast<float>((triangle.packed >> 24) & 0x7Fu) / 127.0f, (triangle.packed & kTranslucentFlag) != 0};
        baked[u].level = LevelFor(image, triangle);
        baked[u].states = BakeTriangle(image, triangle, test, baked[u].level);
    });

    std::vector<int32_t> uniqueIndex(baked.size());
    std::map<uint32_t, uint32_t> levelCounts;
    for (size_t u = 0; u < baked.size(); ++u) {
        const std::vector<uint8_t>& states = baked[u].states;
        if (std::all_of(states.begin(), states.end(), [&](uint8_t s) { return s == states[0]; })) {
            uniqueIndex[u] = SpecialIndex(static_cast<State>(states[0]));
            continue;
        }
        uniqueIndex[u] = static_cast<int32_t>(result.descs.size());
        const size_t offset = (result.data.size() + 3) & ~size_t{3};
        result.data.resize(offset + std::max<size_t>(1, (states.size() * 2 + 7) / 8));
        for (size_t i = 0; i < states.size(); ++i)
            result.data[offset + i / 4] |= static_cast<std::byte>(states[i] << ((i % 4) * 2));
        D3D12_RAYTRACING_OPACITY_MICROMAP_DESC desc = {};
        desc.ByteOffset = static_cast<UINT>(offset);
        desc.SubdivisionLevel = baked[u].level;
        desc.Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE;
        result.descs.push_back(desc);
        ++levelCounts[baked[u].level];
    }
    for (const auto& [level, count] : levelCounts)
        result.histogram.push_back({count, level, D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE});
    for (size_t t = 0; t < triangles.size(); ++t) {
        if (slot[t] != SIZE_MAX)
            result.indices[t] = uniqueIndex[slot[t]];
    }
    return result;
}

}
