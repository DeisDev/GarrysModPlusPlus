#include "glow_draws.h"

#include "engine_interfaces.h"
#include "log.h"
#include "studio_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace gmodpp::glow_draws {

namespace {

// Textures are read at this size for their mean.
constexpr uint32_t kMeanTextureSize = 32;
// Glow is gathered in cubes this many units across.
constexpr float kClusterSize = 48.0f;
// Triangles recorded in a frame beyond this give no light (logged once).
constexpr size_t kMaxTriangles = 1 << 18;

struct Cluster {
    double color[3] = {};
    double position[3] = {};
    double squared = 0.0;
    double weight = 0.0;
};

std::unordered_map<std::string, MaterialGlow> g_materials;
std::unordered_map<int64_t, Cluster> g_clusters;
size_t g_triangles = 0;
bool g_loggedFull = false;

float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float Luminance(const float (&c)[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

std::string NormalizedName(std::string_view name)
{
    std::string normalized(name);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return c == '\\' ? '/' : static_cast<char>(std::tolower(c)); });
    if (normalized.starts_with("materials/"))
        normalized.erase(0, 10);
    if (normalized.ends_with(".vmt"))
        normalized.resize(normalized.size() - 4);
    return normalized;
}

int64_t CellKey(const float (&p)[3])
{
    // 21 bits a cube coordinate: +-1048576 cubes of 48 units covers any map.
    int64_t key = 0;
    for (int a = 0; a < 3; ++a) {
        const int64_t cell = static_cast<int64_t>(std::floor(p[a] / kClusterSize)) + (1 << 20);
        key = key << 21 | (cell & ((1 << 21) - 1));
    }
    return key;
}

void Add(const float (&position)[3], const float (&emitted)[3])
{
    const float luminance = Luminance(emitted);
    if (!(luminance > 0.0f) || !std::isfinite(luminance) || !std::isfinite(position[0] + position[1] + position[2]))
        return;
    Cluster& cluster = g_clusters[CellKey(position)];
    for (int c = 0; c < 3; ++c) {
        cluster.color[c] += emitted[c];
        cluster.position[c] += luminance * position[c];
    }
    cluster.squared += luminance * (position[0] * position[0] + position[1] * position[1] + position[2] * position[2]);
    cluster.weight += luminance;
}

}

const MaterialGlow& GlowOf(std::string_view materialName)
{
    const std::string name = NormalizedName(materialName);
    const auto [entry, added] = g_materials.try_emplace(name);
    if (!added || name.empty())
        return entry->second;
    std::vector<std::byte> file;
    if (!engine::ReadGameFile("materials/" + name + ".vmt", file))
        return entry->second;
    const std::string_view vmt(reinterpret_cast<const char*>(file.data()), file.size());
    const std::optional<std::string> additive = studio::MaterialValue(vmt, "$additive");
    entry->second.additive = additive && !additive->empty() && additive->front() != '0';
    const std::string texture = studio::MaterialTexture(vmt, "$basetexture");
    std::vector<std::byte> vtf;
    std::string error;
    if (texture.empty() || !engine::ReadGameFile(texture, vtf))
        return entry->second;
    const std::optional<studio::TextureImage> image = studio::ReadTexture(vtf, kMeanTextureSize, error);
    if (!image)
        return entry->second;
    std::vector<uint8_t> channels[4];
    for (uint32_t c = 0; c < 4; ++c) {
        channels[c] = studio::DecodeChannel(*image, 0, c);
        if (channels[c].empty())
            return entry->second;
    }
    double weighted[3] = {};
    double plain[3] = {};
    for (size_t t = 0; t < channels[0].size(); ++t) {
        const double alpha = channels[3][t] / 255.0;
        for (int c = 0; c < 3; ++c) {
            const double linear = SrgbToLinear(static_cast<float>(channels[c][t]) / 255.0f);
            weighted[c] += linear * alpha;
            plain[c] += linear;
        }
    }
    const double texels = static_cast<double>(channels[0].size());
    for (int c = 0; c < 3; ++c) {
        entry->second.alphaWeighted[c] = static_cast<float>(weighted[c] / texels);
        entry->second.plain[c] = static_cast<float>(plain[c] / texels);
    }
    entry->second.known = true;
    return entry->second;
}

void Record(const std::uint8_t* vertices, std::uint32_t vertexCount, const Layout& layout, const std::uint16_t* indices,
    std::span<const std::pair<int, int>> primitives, bool strip, const float (&color)[3], bool vertexAlpha)
{
    if (vertices == nullptr || layout.stride == 0 || layout.position == kAbsent || Luminance(color) <= 0.0f)
        return;
    const auto position = [&](uint32_t v, float (&p)[3]) { std::memcpy(p, vertices + static_cast<size_t>(v) * layout.stride + layout.position, sizeof(p)); };
    // A vertex's colour, linear, times its alpha where the draw blends by it.
    const auto tint = [&](uint32_t v, float (&c)[3]) {
        if (layout.color == kAbsent) {
            c[0] = c[1] = c[2] = 1.0f;
            return;
        }
        const std::uint8_t* bgra = vertices + static_cast<size_t>(v) * layout.stride + layout.color;
        const float alpha = vertexAlpha ? bgra[3] / 255.0f : 1.0f;
        for (int i = 0; i < 3; ++i)
            c[i] = SrgbToLinear(bgra[2 - i] / 255.0f) * alpha;
    };
    const auto index = [&](int first, int i) { return indices != nullptr ? static_cast<uint32_t>(indices[first + i]) : static_cast<uint32_t>(first + i); };
    // A triangle with these corners: its area, centre and the mean of its corners' colour.
    const auto triangle = [&](uint32_t v0, uint32_t v1, uint32_t v2) {
        if (v0 >= vertexCount || v1 >= vertexCount || v2 >= vertexCount || v0 == v1 || v1 == v2 || v0 == v2)
            return;
        float p[3][3];
        position(v0, p[0]);
        position(v1, p[1]);
        position(v2, p[2]);
        const float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
        const float e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float area = 0.5f * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(area > 0.0f))
            return;
        float c[3][3];
        tint(v0, c[0]);
        tint(v1, c[1]);
        tint(v2, c[2]);
        float centre[3];
        float emitted[3];
        for (int a = 0; a < 3; ++a) {
            centre[a] = (p[0][a] + p[1][a] + p[2][a]) / 3.0f;
            emitted[a] = color[a] * (c[0][a] + c[1][a] + c[2][a]) / 3.0f * area;
        }
        Add(centre, emitted);
    };
    // SpriteCard writes a sprite's centre into all four of its vertices (the vertex shader moves them out) and its radius
    // into TEXCOORD2.z: a sprite 2r across. Where the four differ they are the corners.
    const auto sprite = [&](int first, int i) {
        const uint32_t v0 = index(first, i);
        if (v0 + 3 >= vertexCount)
            return;
        float p0[3];
        float p2[3];
        position(v0, p0);
        position(v0 + 2, p2);
        const float apart = std::fabs(p2[0] - p0[0]) + std::fabs(p2[1] - p0[1]) + std::fabs(p2[2] - p0[2]);
        if (apart > 1e-3f * std::max(1.0f, std::fabs(p0[0]) + std::fabs(p0[1]) + std::fabs(p0[2]))) {
            triangle(index(first, i), index(first, i + 1), index(first, i + 2));
            triangle(index(first, i + 3), index(first, i + 4), index(first, i + 5));
            return;
        }
        float parameters[4];
        std::memcpy(parameters, vertices + static_cast<size_t>(v0) * layout.stride + layout.parameters, sizeof(parameters));
        float c[3];
        tint(v0, c);
        const float area = 4.0f * parameters[2] * parameters[2];
        const float emitted[3] = {color[0] * c[0] * area, color[1] * c[1] * area, color[2] * c[2] * area};
        Add(p0, emitted);
    };
    const bool sprites = layout.parameters != kAbsent && !strip && indices != nullptr;
    for (const auto& [first, count] : primitives) {
        if (first < 0 || count < 3)
            continue;
        const int size = sprites ? 6 : 3;
        for (int i = 0; i + size <= count; i += strip ? 1 : size) {
            if (++g_triangles > kMaxTriangles) {
                if (!std::exchange(g_loggedFull, true))
                    log::Info("Effect lights: more than {} glowing triangles drawn in a frame; the rest give no light", kMaxTriangles);
                return;
            }
            if (sprites)
                sprite(first, i);
            else
                triangle(index(first, i), index(first, i + 1), index(first, i + 2));
        }
    }
}

std::vector<Glow> Take()
{
    std::vector<Glow> glows;
    glows.reserve(g_clusters.size());
    for (const auto& [key, cluster] : g_clusters) {
        if (cluster.weight <= 0.0)
            continue;
        Glow& glow = glows.emplace_back();
        double spread = cluster.squared / cluster.weight;
        for (int c = 0; c < 3; ++c) {
            glow.color[c] = static_cast<float>(cluster.color[c]);
            glow.centre[c] = static_cast<float>(cluster.position[c] / cluster.weight);
            spread -= static_cast<double>(glow.centre[c]) * glow.centre[c];
        }
        glow.spread = static_cast<float>(std::sqrt(std::max(spread, 0.0)));
    }
    g_clusters.clear();
    g_triangles = 0;
    return glows;
}

}
