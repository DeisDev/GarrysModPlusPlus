#include "studio_model.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <optional>

namespace gmodpp::studio {

namespace {

// On-disk layouts from the Source SDK 2013 studio.h / optimize.h (32-bit pointer slots, packed VTX).
constexpr int32_t kMdlId = 'I' | ('D' << 8) | ('S' << 16) | ('T' << 24);
constexpr int32_t kVvdId = 'I' | ('D' << 8) | ('S' << 16) | ('V' << 24);
constexpr size_t kHeaderNumBones = 156;
constexpr size_t kHeaderBoneIndex = 160;
constexpr size_t kBoneSize = 216;
constexpr size_t kBonePoseToBone = 96;
constexpr size_t kHeaderNumTextures = 204;
constexpr size_t kHeaderTextureIndex = 208;
constexpr size_t kHeaderNumCdTextures = 212;
constexpr size_t kHeaderCdTextureIndex = 216;
constexpr size_t kHeaderNumSkinRef = 220;
constexpr size_t kHeaderSkinIndex = 228;
constexpr size_t kHeaderNumBodyParts = 232;
constexpr size_t kHeaderBodyPartIndex = 236;
constexpr size_t kTextureSize = 64;
constexpr size_t kBodyPartSize = 16;
constexpr size_t kModelSize = 148;
constexpr size_t kModelNumMeshes = 72;
constexpr size_t kModelMeshIndex = 76;
constexpr size_t kModelVertexIndex = 84;
constexpr size_t kMeshSize = 116;
constexpr size_t kMeshMaterial = 0;
constexpr size_t kMeshVertexOffset = 12;
constexpr size_t kStudioVertexSize = 48;
constexpr size_t kStudioVertexPosition = 16;
constexpr size_t kVvdNumLods = 12;
constexpr size_t kVvdNumFixups = 48;
constexpr size_t kVvdFixupTableStart = 52;
constexpr size_t kVvdVertexDataStart = 56;
constexpr size_t kVvdLod0Vertexes = 16;
constexpr size_t kVtxNumBodyParts = 28;
constexpr size_t kVtxBodyPartOffset = 32;
constexpr size_t kVtxVertexSize = 9;
constexpr size_t kVtxVertexOrigId = 4;

static_assert(sizeof(VertexWeights) == 16);
static_assert(sizeof(Matrix3x4) == 48);

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : m_data(data) {}

    template <class T>
    std::optional<T> At(size_t offset) const
    {
        if (offset > m_data.size() || m_data.size() - offset < sizeof(T))
            return std::nullopt;
        T value;
        std::memcpy(&value, m_data.data() + offset, sizeof(T));
        return value;
    }

    std::optional<std::string> String(size_t offset) const
    {
        if (offset >= m_data.size())
            return std::nullopt;
        const char* begin = reinterpret_cast<const char*>(m_data.data() + offset);
        size_t length = strnlen(begin, m_data.size() - offset);
        if (offset + length >= m_data.size())
            return std::nullopt;
        return std::string(begin, length);
    }

    size_t Size() const { return m_data.size(); }

private:
    std::span<const std::byte> m_data;
};

std::string NormalizePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    if (!path.empty() && path.back() != '/')
        path += '/';
    return path;
}

struct VtxStripGroupLayout {
    size_t size;
};

// Older VTX files use 25-byte strip group headers; newer studiomdl versions add 8 bytes of topology data.
constexpr VtxStripGroupLayout kStripGroupLayouts[] = {{25}, {33}};

struct StudioVertices {
    std::vector<bsp::Float3> positions;
    std::vector<VertexWeights> weights;
};

bool AppendMesh(const Reader& vtx, size_t meshHeader, const StudioVertices& vertices, size_t meshVertexBase,
    size_t stripGroupSize, Geometry& geometry)
{
    auto numStripGroups = vtx.At<int32_t>(meshHeader);
    auto stripGroupOffset = vtx.At<int32_t>(meshHeader + 4);
    if (!numStripGroups || !stripGroupOffset || *numStripGroups < 0)
        return false;
    for (int32_t g = 0; g < *numStripGroups; ++g) {
        const size_t group = meshHeader + *stripGroupOffset + g * stripGroupSize;
        auto numVerts = vtx.At<int32_t>(group);
        auto vertOffset = vtx.At<int32_t>(group + 4);
        auto numIndices = vtx.At<int32_t>(group + 8);
        auto indexOffset = vtx.At<int32_t>(group + 12);
        if (!numVerts || !vertOffset || !numIndices || !indexOffset || *numIndices % 3 != 0)
            return false;
        const uint32_t base = static_cast<uint32_t>(geometry.positions.size());
        for (int32_t v = 0; v < *numVerts; ++v) {
            auto original = vtx.At<uint16_t>(group + *vertOffset + v * kVtxVertexSize + kVtxVertexOrigId);
            if (!original || meshVertexBase + *original >= vertices.positions.size())
                return false;
            geometry.positions.push_back(vertices.positions[meshVertexBase + *original]);
            geometry.weights.push_back(vertices.weights[meshVertexBase + *original]);
        }
        for (int32_t i = 0; i < *numIndices; ++i) {
            auto index = vtx.At<uint16_t>(group + *indexOffset + i * sizeof(uint16_t));
            if (!index || *index >= *numVerts)
                return false;
            geometry.indices.push_back(base + *index);
        }
    }
    return true;
}

std::optional<StudioVertices> Lod0Vertices(const Reader& vvd, std::string& error)
{
    if (vvd.At<int32_t>(0) != kVvdId) {
        error = "VVD identifier mismatch";
        return std::nullopt;
    }
    auto numLods = vvd.At<int32_t>(kVvdNumLods);
    auto lod0Count = vvd.At<int32_t>(kVvdLod0Vertexes);
    auto numFixups = vvd.At<int32_t>(kVvdNumFixups);
    auto fixupStart = vvd.At<int32_t>(kVvdFixupTableStart);
    auto vertexStart = vvd.At<int32_t>(kVvdVertexDataStart);
    if (!numLods || !lod0Count || !numFixups || !fixupStart || !vertexStart || *lod0Count < 0) {
        error = "VVD header is truncated";
        return std::nullopt;
    }
    StudioVertices vertices;
    auto append = [&](int32_t index) {
        const size_t vertex = *vertexStart + static_cast<size_t>(index) * kStudioVertexSize;
        auto p = vvd.At<bsp::Float3>(vertex + kStudioVertexPosition);
        auto w = vvd.At<VertexWeights>(vertex);
        if (!p || !w)
            return false;
        vertices.positions.push_back(*p);
        vertices.weights.push_back(*w);
        return true;
    };

    if (*numFixups == 0) {
        for (int32_t i = 0; i < *lod0Count; ++i) {
            if (!append(i)) {
                error = "VVD vertex data is truncated";
                return std::nullopt;
            }
        }
        return vertices;
    }
    for (int32_t f = 0; f < *numFixups; ++f) {
        const size_t fixup = *fixupStart + static_cast<size_t>(f) * 12;
        auto lod = vvd.At<int32_t>(fixup);
        auto source = vvd.At<int32_t>(fixup + 4);
        auto count = vvd.At<int32_t>(fixup + 8);
        if (!lod || !source || !count) {
            error = "VVD fixup table is truncated";
            return std::nullopt;
        }
        if (*lod < 0)
            continue;
        for (int32_t i = 0; i < *count; ++i) {
            if (!append(*source + i)) {
                error = "VVD fixup references missing vertices";
                return std::nullopt;
            }
        }
    }
    return vertices;
}

}

int BoneCount(std::span<const std::byte> mdlBytes)
{
    Reader mdl(mdlBytes);
    return mdl.At<int32_t>(kHeaderNumBones).value_or(0);
}

bool ReadPoseToBone(std::span<const std::byte> mdlBytes, std::vector<Matrix3x4>& poseToBone)
{
    Reader mdl(mdlBytes);
    auto numBones = mdl.At<int32_t>(kHeaderNumBones);
    auto boneIndex = mdl.At<int32_t>(kHeaderBoneIndex);
    if (!numBones || !boneIndex || *numBones < 0)
        return false;
    poseToBone.resize(*numBones);
    for (int32_t b = 0; b < *numBones; ++b) {
        auto matrix = mdl.At<Matrix3x4>(*boneIndex + static_cast<size_t>(b) * kBoneSize + kBonePoseToBone);
        if (!matrix)
            return false;
        poseToBone[b] = *matrix;
    }
    return true;
}

bool ReadMaterialPaths(std::span<const std::byte> mdlBytes, std::vector<std::vector<std::string>>& texturePaths, std::string& error)
{
    Reader mdl(mdlBytes);
    if (mdl.At<int32_t>(0) != kMdlId) {
        error = "MDL identifier mismatch";
        return false;
    }
    auto numTextures = mdl.At<int32_t>(kHeaderNumTextures);
    auto textureIndex = mdl.At<int32_t>(kHeaderTextureIndex);
    auto numCdTextures = mdl.At<int32_t>(kHeaderNumCdTextures);
    auto cdTextureIndex = mdl.At<int32_t>(kHeaderCdTextureIndex);
    if (!numTextures || !textureIndex || !numCdTextures || !cdTextureIndex || *numTextures < 0 || *numCdTextures < 0) {
        error = "MDL header is truncated";
        return false;
    }
    std::vector<std::string> directories;
    for (int32_t c = 0; c < *numCdTextures; ++c) {
        auto offset = mdl.At<int32_t>(*cdTextureIndex + static_cast<size_t>(c) * 4);
        auto directory = offset ? mdl.String(*offset) : std::nullopt;
        if (directory)
            directories.push_back(NormalizePath(*directory));
    }
    texturePaths.assign(*numTextures, {});
    for (int32_t t = 0; t < *numTextures; ++t) {
        const size_t texture = *textureIndex + static_cast<size_t>(t) * kTextureSize;
        auto nameOffset = mdl.At<int32_t>(texture);
        auto name = nameOffset ? mdl.String(texture + *nameOffset) : std::nullopt;
        if (!name)
            continue;
        std::string file = *name;
        std::replace(file.begin(), file.end(), '\\', '/');
        std::transform(file.begin(), file.end(), file.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const std::string& directory : directories)
            texturePaths[t].push_back("materials/" + directory + file + ".vmt");
    }
    return true;
}

bool BuildGeometry(std::span<const std::byte> mdlBytes, std::span<const std::byte> vvdBytes, std::span<const std::byte> vtxBytes,
    const std::vector<bool>& skipTexture, Geometry& geometry, std::string& error)
{
    Reader mdl(mdlBytes);
    Reader vvd(vvdBytes);
    Reader vtx(vtxBytes);
    std::optional<StudioVertices> vertices = Lod0Vertices(vvd, error);
    if (!vertices)
        return false;

    auto numBodyParts = mdl.At<int32_t>(kHeaderNumBodyParts);
    auto bodyPartIndex = mdl.At<int32_t>(kHeaderBodyPartIndex);
    auto numSkinRef = mdl.At<int32_t>(kHeaderNumSkinRef);
    auto skinIndex = mdl.At<int32_t>(kHeaderSkinIndex);
    auto vtxBodyParts = vtx.At<int32_t>(kVtxNumBodyParts);
    auto vtxBodyPartOffset = vtx.At<int32_t>(kVtxBodyPartOffset);
    if (!numBodyParts || !bodyPartIndex || !numSkinRef || !skinIndex || !vtxBodyParts || !vtxBodyPartOffset || *vtxBodyParts != *numBodyParts) {
        error = "MDL and VTX body parts do not match";
        return false;
    }

    for (const VtxStripGroupLayout& layout : kStripGroupLayouts) {
        geometry = {};
        bool valid = true;
        for (int32_t b = 0; valid && b < *numBodyParts; ++b) {
            const size_t bodyPart = *bodyPartIndex + static_cast<size_t>(b) * kBodyPartSize;
            auto numModels = mdl.At<int32_t>(bodyPart + 4);
            auto modelIndex = mdl.At<int32_t>(bodyPart + 12);
            const size_t vtxBodyPart = *vtxBodyPartOffset + static_cast<size_t>(b) * 8;
            auto vtxModelOffset = vtx.At<int32_t>(vtxBodyPart + 4);
            if (!numModels || !modelIndex || !vtxModelOffset || *numModels <= 0) {
                valid = false;
                break;
            }
            const size_t model = bodyPart + *modelIndex;
            auto numMeshes = mdl.At<int32_t>(model + kModelNumMeshes);
            auto meshIndex = mdl.At<int32_t>(model + kModelMeshIndex);
            auto vertexIndex = mdl.At<int32_t>(model + kModelVertexIndex);
            const size_t vtxModel = vtxBodyPart + *vtxModelOffset;
            auto lodOffset = vtx.At<int32_t>(vtxModel + 4);
            if (!numMeshes || !meshIndex || !vertexIndex || !lodOffset) {
                valid = false;
                break;
            }
            const size_t vtxLod = vtxModel + *lodOffset;
            auto vtxNumMeshes = vtx.At<int32_t>(vtxLod);
            auto vtxMeshOffset = vtx.At<int32_t>(vtxLod + 4);
            if (!vtxNumMeshes || !vtxMeshOffset || *vtxNumMeshes != *numMeshes) {
                valid = false;
                break;
            }
            const size_t modelVertexBase = static_cast<size_t>(*vertexIndex) / kStudioVertexSize;
            for (int32_t m = 0; valid && m < *numMeshes; ++m) {
                const size_t mesh = model + *meshIndex + static_cast<size_t>(m) * kMeshSize;
                auto material = mdl.At<int32_t>(mesh + kMeshMaterial);
                auto vertexOffset = mdl.At<int32_t>(mesh + kMeshVertexOffset);
                if (!material || !vertexOffset) {
                    valid = false;
                    break;
                }
                std::optional<int16_t> texture;
                if (*material >= 0 && *material < *numSkinRef)
                    texture = mdl.At<int16_t>(*skinIndex + static_cast<size_t>(*material) * sizeof(int16_t));
                if (texture && *texture >= 0 && static_cast<size_t>(*texture) < skipTexture.size() && skipTexture[*texture])
                    continue;
                const size_t vtxMesh = vtxLod + *vtxMeshOffset + static_cast<size_t>(m) * 9;
                valid = AppendMesh(vtx, vtxMesh, *vertices, modelVertexBase + *vertexOffset, layout.size, geometry);
            }
        }
        if (valid)
            return true;
    }
    error = "VTX strip data does not match the model";
    geometry = {};
    return false;
}

bool MaterialIsSeeThrough(std::string_view vmt)
{
    std::string text(vmt);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* key : {"$alphatest", "$translucent"}) {
        size_t pos = text.find(key);
        while (pos != std::string::npos) {
            size_t value = text.find_first_not_of(" \t\"", pos + std::strlen(key));
            if (value != std::string::npos && text[value] == '1')
                return true;
            pos = text.find(key, pos + 1);
        }
    }
    return false;
}

}
