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
constexpr size_t kHeaderHullMin = 104;
constexpr size_t kHeaderViewMin = 128;
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
constexpr size_t kStudioVertexTexCoord = 40;
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
    std::vector<bsp::Float2> uvs;
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
            geometry.uvs.push_back(vertices.uvs[meshVertexBase + *original]);
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
        auto uv = vvd.At<bsp::Float2>(vertex + kStudioVertexTexCoord);
        auto w = vvd.At<VertexWeights>(vertex);
        if (!p || !uv || !w)
            return false;
        vertices.positions.push_back(*p);
        vertices.uvs.push_back(*uv);
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

Matrix3x4 Multiply(const Matrix3x4& a, const Matrix3x4& b)
{
    Matrix3x4 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c)
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (c == 3 ? a[r][3] : 0.0f);
    }
    return out;
}

int BoneCount(std::span<const std::byte> mdlBytes)
{
    Reader mdl(mdlBytes);
    return mdl.At<int32_t>(kHeaderNumBones).value_or(0);
}

bool ReadBounds(std::span<const std::byte> mdlBytes, bsp::Float3& mins, bsp::Float3& maxs)
{
    Reader mdl(mdlBytes);
    bool found = false;
    for (size_t offset : {kHeaderHullMin, kHeaderViewMin}) {
        auto low = mdl.At<bsp::Float3>(offset);
        auto high = mdl.At<bsp::Float3>(offset + sizeof(bsp::Float3));
        if (!low || !high || (low->x == high->x && low->y == high->y && low->z == high->z))
            continue;
        if (!found) {
            mins = *low;
            maxs = *high;
            found = true;
            continue;
        }
        mins = {std::min(mins.x, low->x), std::min(mins.y, low->y), std::min(mins.z, low->z)};
        maxs = {std::max(maxs.x, high->x), std::max(maxs.y, high->y), std::max(maxs.z, high->z)};
    }
    return found;
}

int BoneIndex(std::span<const std::byte> mdlBytes, std::string_view name)
{
    Reader mdl(mdlBytes);
    auto numBones = mdl.At<int32_t>(kHeaderNumBones);
    auto boneIndex = mdl.At<int32_t>(kHeaderBoneIndex);
    if (!numBones || !boneIndex)
        return -1;
    for (int32_t b = 0; b < *numBones; ++b) {
        const size_t bone = *boneIndex + static_cast<size_t>(b) * kBoneSize;
        auto nameOffset = mdl.At<int32_t>(bone);
        auto boneName = nameOffset ? mdl.String(bone + *nameOffset) : std::nullopt;
        if (boneName && *boneName == name)
            return b;
    }
    return -1;
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
                geometry.triangleTextures.resize(geometry.indices.size() / 3, texture && *texture >= 0 ? static_cast<uint16_t>(*texture) : kNoTexture);
            }
        }
        if (valid)
            return true;
    }
    error = "VTX strip data does not match the model";
    geometry = {};
    return false;
}

std::optional<std::string> MaterialValue(std::string_view vmt, std::string_view key)
{
    // KeyValues tokens: quoted strings or bare words; "//" starts a comment.
    std::vector<std::string> tokens;
    for (size_t i = 0; i < vmt.size();) {
        const char c = vmt[i];
        if (c == '/' && i + 1 < vmt.size() && vmt[i + 1] == '/') {
            i = vmt.find('\n', i);
            if (i == std::string_view::npos)
                break;
        } else if (c == '"') {
            size_t end = vmt.find('"', i + 1);
            if (end == std::string_view::npos)
                break;
            tokens.emplace_back(vmt.substr(i + 1, end - i - 1));
            i = end + 1;
        } else if (c == '{' || c == '}' || std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else {
            size_t end = i;
            while (end < vmt.size() && !std::isspace(static_cast<unsigned char>(vmt[end])) && vmt[end] != '"' && vmt[end] != '{' && vmt[end] != '}')
                ++end;
            tokens.emplace_back(vmt.substr(i, end - i));
            i = end;
        }
    }
    auto equalsKey = [&](const std::string& token) {
        return token.size() == key.size() &&
            std::equal(token.begin(), token.end(), key.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    };
    for (size_t t = 0; t + 1 < tokens.size(); ++t) {
        if (equalsKey(tokens[t]))
            return tokens[t + 1];
    }
    return std::nullopt;
}

std::string MaterialTexture(std::string_view vmt, std::string_view key)
{
    const std::optional<std::string> value = MaterialValue(vmt, key);
    if (!value)
        return {};
    std::string path = NormalizePath(*value);
    if (path.empty())
        return {};
    path.pop_back();
    if (path.size() > 4 && path.ends_with(".vtf"))
        path.resize(path.size() - 4);
    return "materials/" + path + ".vtf";
}

// A .phy file: a phyheader_t (SDK public/phyfile.h), then per solid its byte size and an IVP compact surface,
// after a 28-byte "VPHY" header in the current format. The surface's 48-byte header ends in "IVPS"; its convex
// pieces (ledges) follow back to back, each a 16-byte header (point array offset from the piece, client data,
// flags, triangle count) and 16-byte triangles whose three edges start at the points indexed by the low 16 bits
// of their last three dwords. The pieces share one array of points (x, y, z, w) in metres on IVP's axes.
bool ReadCollisionPieces(std::span<const std::byte> phyBytes, std::vector<std::vector<bsp::Float3>>& pieces, std::string& error)
{
    constexpr size_t kVphyHeaderSize = 28;
    constexpr size_t kSurfaceHeaderSize = 48;
    constexpr size_t kLedgeHeaderSize = 16;
    constexpr size_t kTriangleSize = 16;
    constexpr size_t kPointSize = 16;
    constexpr float kInchesPerMetre = 1.0f / 0.0254f;
    Reader phy(phyBytes);
    auto headerSize = phy.At<int32_t>(0);
    auto solidCount = phy.At<int32_t>(8);
    if (!headerSize || !solidCount || *headerSize < 16 || *solidCount < 1) {
        error = "PHY header is invalid";
        return false;
    }
    auto solidSize = phy.At<int32_t>(*headerSize);
    const size_t solid = static_cast<size_t>(*headerSize) + 4;
    if (!solidSize || *solidSize <= 0 || solid + *solidSize > phyBytes.size()) {
        error = "PHY solid is truncated";
        return false;
    }
    const size_t end = solid + *solidSize;
    const bool vphy = phyBytes.size() >= solid + 4 && std::memcmp(phyBytes.data() + solid, "VPHY", 4) == 0;
    const size_t surface = solid + (vphy ? kVphyHeaderSize : 0);
    if (surface + kSurfaceHeaderSize > end || std::memcmp(phyBytes.data() + surface + kSurfaceHeaderSize - 4, "IVPS", 4) != 0) {
        error = "PHY solid is not an IVP compact surface";
        return false;
    }
    std::vector<std::vector<uint16_t>> indices;
    size_t points = 0;
    for (size_t ledge = surface + kSurfaceHeaderSize; points == 0 || ledge < points;) {
        auto pointOffset = phy.At<int32_t>(ledge);
        auto triangles = phy.At<uint16_t>(ledge + 12);
        if (!pointOffset || !triangles || *triangles == 0 || ledge + kLedgeHeaderSize + *triangles * kTriangleSize > end) {
            error = "PHY convex piece is truncated";
            return false;
        }
        if (points == 0)
            points = ledge + *pointOffset;
        std::vector<uint16_t>& piece = indices.emplace_back();
        for (size_t t = 0; t < *triangles; ++t) {
            for (size_t e = 0; e < 3; ++e)
                piece.push_back(*phy.At<uint16_t>(ledge + kLedgeHeaderSize + t * kTriangleSize + 4 + e * 4));
        }
        ledge += kLedgeHeaderSize + *triangles * kTriangleSize;
    }
    pieces.clear();
    for (std::vector<uint16_t>& piece : indices) {
        std::sort(piece.begin(), piece.end());
        piece.erase(std::unique(piece.begin(), piece.end()), piece.end());
        std::vector<bsp::Float3>& out = pieces.emplace_back();
        for (uint16_t index : piece) {
            auto point = phy.At<bsp::Float3>(points + index * kPointSize);
            if (!point || points + (index + 1) * kPointSize > end) {
                error = "PHY point index is out of range";
                return false;
            }
            out.push_back({point->x * kInchesPerMetre, point->z * kInchesPerMetre, -point->y * kInchesPerMetre});
        }
    }
    return true;
}

std::optional<bsp::Float3> TextureReflectivity(std::span<const std::byte> vtfHeader)
{
    constexpr size_t kReflectivityOffset = 32;
    Reader vtf(vtfHeader);
    if (vtfHeader.size() < kVtfReflectivityHeaderSize || std::memcmp(vtfHeader.data(), "VTF", 4) != 0)
        return std::nullopt;
    return vtf.At<bsp::Float3>(kReflectivityOffset);
}

namespace {

// VTF image formats (ImageFormat in the Source SDK's bitmap/imageformat.h).
struct VtfFormat {
    int32_t id;
    TextureFormat format;
    // Bytes per 4x4 block for block-compressed formats, else 0.
    uint32_t blockBytes;
    uint32_t texelBytes;
    bool alpha;
};
constexpr VtfFormat kFormats[] = {{0, TextureFormat::Rgba8, 0, 4, true}, {5, TextureFormat::R8, 0, 1, false}, {12, TextureFormat::Bgra8, 0, 4, true},
    {13, TextureFormat::Bc1, 8, 0, true}, {14, TextureFormat::Bc2, 16, 0, true}, {15, TextureFormat::Bc3, 16, 0, true},
    {16, TextureFormat::Bgrx8, 0, 4, false}, {20, TextureFormat::Bc1, 8, 0, true}};
constexpr VtfFormat kLowResFormat = {13, TextureFormat::Bc1, 8, 0, true};
constexpr uint32_t kVtfEnvmapFlag = 0x4000;
constexpr uint32_t kVtfHighResResource = 0x30;
constexpr size_t kVtfResourcesOffset = 80;

uint32_t MipBytes(const VtfFormat& format, uint32_t width, uint32_t height)
{
    if (format.blockBytes == 0)
        return width * height * format.texelBytes;
    return std::max(1u, (width + 3) / 4) * std::max(1u, (height + 3) / 4) * format.blockBytes;
}

}

std::optional<TextureImage> ReadTexture(std::span<const std::byte> bytes, uint32_t maxSize, std::string& error)
{
    Reader vtf(bytes);
    auto minor = vtf.At<uint32_t>(8);
    auto headerSize = vtf.At<uint32_t>(12);
    auto width = vtf.At<uint16_t>(16);
    auto height = vtf.At<uint16_t>(18);
    auto flags = vtf.At<uint32_t>(20);
    auto frames = vtf.At<uint16_t>(24);
    auto format = vtf.At<int32_t>(52);
    auto mips = vtf.At<uint8_t>(56);
    auto lowFormat = vtf.At<int32_t>(57);
    auto lowWidth = vtf.At<uint8_t>(61);
    auto lowHeight = vtf.At<uint8_t>(62);
    if (bytes.size() < 4 || std::memcmp(bytes.data(), "VTF", 4) != 0 || !minor || !headerSize || !width || !height || !flags || !frames || !format ||
        !mips || !lowFormat || !lowWidth || !lowHeight || *mips == 0 || *width == 0 || *height == 0) {
        error = "not a VTF file";
        return std::nullopt;
    }
    const VtfFormat* info = nullptr;
    for (const VtfFormat& candidate : kFormats)
        info = candidate.id == *format ? &candidate : info;
    const uint16_t depth = *minor >= 2 ? vtf.At<uint16_t>(63).value_or(1) : 1;
    if (info == nullptr || (*flags & kVtfEnvmapFlag) != 0 || depth > 1) {
        error = std::format("image format {} (flags {:#x}, depth {}) is not supported", *format, *flags, depth);
        return std::nullopt;
    }

    size_t offset = 0;
    if (*minor >= 3) {
        const uint32_t resources = vtf.At<uint32_t>(68).value_or(0);
        for (uint32_t r = 0; r < resources && offset == 0; ++r) {
            auto type = vtf.At<uint32_t>(kVtfResourcesOffset + r * 8);
            auto data = vtf.At<uint32_t>(kVtfResourcesOffset + r * 8 + 4);
            if (type && data && (*type & 0xFFFFFFu) == kVtfHighResResource)
                offset = *data;
        }
    } else {
        offset = *headerSize;
        if (*lowFormat == kLowResFormat.id)
            offset += MipBytes(kLowResFormat, *lowWidth, *lowHeight);
        else if (*lowFormat != -1)
            offset = 0;
    }
    if (offset == 0) {
        error = "the image data could not be located";
        return std::nullopt;
    }

    // Mips are stored from the smallest to the largest, each with all its frames. Block-compressed mips must be
    // whole blocks to become a texture.
    const bool blocks = info->blockBytes != 0;
    int chosen = -1;
    for (int m = 0; m < *mips; ++m) {
        const uint32_t w = std::max(1u, static_cast<uint32_t>(*width) >> m);
        const uint32_t h = std::max(1u, static_cast<uint32_t>(*height) >> m);
        if (std::max(w, h) <= maxSize && (!blocks || (w % 4 == 0 && h % 4 == 0))) {
            chosen = m;
            break;
        }
    }
    if (chosen < 0) {
        error = std::format("no mip of the {}x{} image fits", *width, *height);
        return std::nullopt;
    }
    TextureImage image{info->format, std::max(1u, static_cast<uint32_t>(*width) >> chosen), std::max(1u, static_cast<uint32_t>(*height) >> chosen),
        info->alpha, {}};
    image.mips.resize(*mips - chosen);
    for (int m = *mips - 1; m >= chosen; --m) {
        const size_t size = MipBytes(*info, std::max(1u, static_cast<uint32_t>(*width) >> m), std::max(1u, static_cast<uint32_t>(*height) >> m));
        if (offset > bytes.size() || bytes.size() - offset < size) {
            error = "the image data is truncated";
            return std::nullopt;
        }
        image.mips[m - chosen] = bytes.subspan(offset, size);
        offset += size * *frames;
    }
    return image;
}

}
