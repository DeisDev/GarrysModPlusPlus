#include "bsp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>

#include <LzmaDec.h>

namespace gmodpp::bsp {

namespace {

constexpr int32_t kIdent = 'V' | ('B' << 8) | ('S' << 16) | ('P' << 24);
constexpr int kLumpCount = 64;
constexpr int kLumpEntities = 0;
constexpr int kLumpPlanes = 1;
constexpr int kLumpTexdata = 2;
constexpr int kLumpVertexes = 3;
constexpr int kLumpVisibility = 4;
constexpr int kLumpNodes = 5;
constexpr int kLumpLeafs = 10;
constexpr int kLumpLeafFaces = 16;
constexpr int kLumpTexinfo = 6;
constexpr int kLumpFaces = 7;
constexpr int kLumpEdges = 12;
constexpr int kLumpSurfedges = 13;
constexpr int kLumpModels = 14;
constexpr int kLumpWorldLights = 15;
constexpr int kLumpDispInfo = 26;
constexpr int kLumpGameLump = 35;
constexpr int kLumpDispVerts = 33;
constexpr int kLumpTexdataStringData = 43;
constexpr int kLumpTexdataStringTable = 44;
constexpr int kLumpWorldLightsHdr = 54;
constexpr int kLumpLighting = 8;
constexpr int kLumpLeafAmbientIndexHdr = 51;
constexpr int kLumpLeafAmbientIndex = 52;
constexpr int kLumpLightingHdr = 53;
constexpr int kLumpLeafAmbientLightingHdr = 55;
constexpr int kLumpLeafAmbientLighting = 56;
constexpr int kLumpFacesHdr = 58;

// dworldlight_t::flags (bspfile.h).
constexpr int32_t kWorldLightInAmbientCube = 0x0001;
constexpr int32_t kSurfSky2D = 0x0002;
constexpr int32_t kSurfSky = 0x0004;
constexpr int32_t kSurfWarp = 0x0008;
constexpr int32_t kSurfTrigger = 0x0040;
constexpr int32_t kSurfNoDraw = 0x0080;
constexpr int32_t kSurfHint = 0x0100;
constexpr int32_t kSurfSkip = 0x0200;
constexpr int32_t kSkippedSurfaces = kSurfSky2D | kSurfSky | kSurfWarp | kSurfTrigger | kSurfNoDraw | kSurfHint | kSurfSkip;
// bspflags.h: no lightmap (VRAD's TEX_SPECIAL is SURF_SKY | SURF_NOLIGHT), and three more lightmaps for the bump basis.
constexpr int32_t kSurfNoLight = 0x0400;
constexpr int32_t kSurfBumpLight = 0x0800;
// mathlib/bumpvects.h: g_localBumpBasis.
constexpr Float3 kLocalBumpBasis[3] = {{0.81649661f, 0.0f, 0.57735026f}, {-0.40824822f, 0.70710677f, 0.57735026f}, {-0.40824822f, -0.70710677f, 0.57735026f}};

constexpr int32_t kStaticPropLumpId = ('s' << 24) | ('p' << 16) | ('r' << 8) | 'p';
constexpr size_t kStaticPropNameLength = 128;
constexpr uint32_t kStaticPropIgnoreNormals = 0x08;
constexpr uint32_t kStaticPropNoShadow = 0x10;
constexpr float kDegreesToRadians = 3.14159265358979f / 180.0f;

constexpr uint32_t kLzmaId = 'L' | ('Z' << 8) | ('M' << 16) | ('A' << 24);
constexpr uint16_t kGameLumpCompressed = 0x0001;


#pragma pack(push, 1)
struct Lump {
    int32_t offset;
    int32_t length;
    int32_t version;
    int32_t fourCC;
};

struct Header {
    int32_t ident;
    int32_t version;
    Lump lumps[kLumpCount];
    int32_t mapRevision;
};

struct Plane {
    Float3 normal;
    float distance;
    int32_t type;
};

struct Node {
    int32_t planeNum;
    int32_t children[2];
    int16_t mins[3];
    int16_t maxs[3];
    uint16_t firstFace;
    uint16_t numFaces;
    int16_t area;
    int16_t padding;
};

struct Leaf {
    int32_t contents;
    int16_t cluster;
    int16_t areaFlags;
    int16_t mins[3];
    int16_t maxs[3];
    uint16_t firstLeafFace;
    uint16_t numLeafFaces;
    uint16_t firstLeafBrush;
    uint16_t numLeafBrushes;
    int16_t leafWaterDataId;
};

struct Face {
    uint16_t planeNum;
    uint8_t side;
    uint8_t onNode;
    int32_t firstEdge;
    int16_t numEdges;
    int16_t texinfo;
    int16_t dispinfo;
    int16_t surfaceFogVolumeId;
    uint8_t styles[4];
    int32_t lightOffset;
    float area;
    int32_t lightmapMins[2];
    int32_t lightmapSize[2];
    int32_t origFace;
    uint16_t numPrims;
    uint16_t firstPrimId;
    uint32_t smoothingGroups;
};

struct Texinfo {
    float textureVecs[2][4];
    float lightmapVecs[2][4];
    int32_t flags;
    int32_t texdata;
};

struct Texdata {
    Float3 reflectivity;
    int32_t nameStringTableId;
    int32_t width, height;
    int32_t viewWidth, viewHeight;
};

struct Model {
    Float3 mins, maxs, origin;
    int32_t headNode;
    int32_t firstFace, numFaces;
};

struct DispVert {
    Float3 vec;
    float dist;
    float alpha;
};

struct DispInfo {
    Float3 startPosition;
    int32_t dispVertStart;
    int32_t dispTriStart;
    int32_t power;
    int32_t minTess;
    float smoothingAngle;
    int32_t contents;
    uint16_t mapFace;
    uint16_t padding;
    int32_t lightmapAlphaStart;
    int32_t lightmapSamplePositionStart;
    uint8_t neighbors[88];
    uint32_t allowedVerts[10];
};

struct LeafAmbientIndex {
    uint16_t sampleCount;
    uint16_t firstSample;
};

// dleafambientlighting_t: a CompressedLightCube and the sample's place as fractions of the leaf's bounds (0-255).
struct LeafAmbientLighting {
    uint8_t cube[6][4];
    uint8_t x, y, z, padding;
};

struct WorldLight {
    Float3 origin;
    Float3 intensity;
    Float3 normal;
    int32_t cluster;
    int32_t type;
    int32_t style;
    float stopdot, stopdot2, exponent, radius;
    float constantAttn, linearAttn, quadraticAttn;
    int32_t flags;
    int32_t texinfo;
    int32_t owner;
};

struct GameLump {
    int32_t id;
    uint16_t flags;
    uint16_t version;
    int32_t offset;
    int32_t length;
};

struct LzmaHeader {
    uint32_t id;
    uint32_t actualSize;
    uint32_t lzmaSize;
    uint8_t properties[LZMA_PROPS_SIZE];
};
#pragma pack(pop)

static_assert(sizeof(Header) == 1036);
static_assert(sizeof(Plane) == 20);
static_assert(sizeof(Node) == 32);
static_assert(sizeof(Leaf) == 30);
static_assert(sizeof(Face) == 56);
static_assert(sizeof(Texinfo) == 72);
static_assert(sizeof(Model) == 48);
static_assert(sizeof(Texdata) == 32);
static_assert(sizeof(DispVert) == 20);
static_assert(sizeof(DispInfo) == 176);
static_assert(sizeof(WorldLight) == 88);
static_assert(sizeof(GameLump) == 16);
static_assert(sizeof(LzmaHeader) == 17);

constexpr size_t kWorldLightV1Size = 100;
constexpr size_t kLeafV0Size = 56;
constexpr size_t kLeafV1Size = 32;

template <class T>
std::optional<std::span<const T>> LumpArray(std::span<const std::byte> file, const Header& header, int index, size_t stride = sizeof(T))
{
    const Lump& lump = header.lumps[index];
    if (lump.offset < 0 || lump.length < 0 || static_cast<size_t>(lump.offset) + lump.length > file.size() || lump.length % stride != 0)
        return std::nullopt;
    return std::span<const T>(reinterpret_cast<const T*>(file.data() + lump.offset), lump.length / stride);
}

Float3 Sub(Float3 a, Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 Add(Float3 a, Float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 Scale(Float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float DistanceSquared(Float3 a, Float3 b)
{
    Float3 d = Sub(a, b);
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

struct Lumps {
    std::span<const Float3> vertices;
    std::span<const int32_t> surfedges;
    std::span<const uint16_t> edges;
    std::span<const Face> faces;
    std::span<const Texinfo> texinfos;
    std::span<const Texdata> texdatas;
    std::span<const DispInfo> dispinfos;
    std::span<const DispVert> dispVerts;
    std::span<const Plane> planes;
    // The faces whose lighting offsets index `baked->samples` (LUMP_FACES_HDR for HDR lighting when present).
    std::span<const Face> lightFaces;
    BakedLighting* baked = nullptr;
};

float Dot(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 Cross(Float3 a, Float3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Float3 Normalize(Float3 a)
{
    const float length = std::sqrt(Dot(a, a));
    return length > 0.0f ? Scale(a, 1.0f / length) : a;
}

// A face's luxel coordinates of a point on it (WorldToLuxelSpace in src/utils/vrad/radial.cpp): its texinfo's
// lightmap vectors, less the lightmap's mins.
Float2 FaceLuxel(const Lumps& lumps, const Face& face, Float3 p)
{
    const Texinfo& texinfo = lumps.texinfos[face.texinfo];
    const auto& v = texinfo.lightmapVecs;
    return {v[0][0] * p.x + v[0][1] * p.y + v[0][2] * p.z + v[0][3] - static_cast<float>(face.lightmapMins[0]),
        v[1][0] * p.x + v[1][1] * p.y + v[1][2] * p.z + v[1][3] - static_cast<float>(face.lightmapMins[1])};
}

// Adds the style-0 lightmap of a world face to `lumps.baked` and returns its index, or kNoFace when the face has
// none. Flat faces also get their luxels to bake, placed as VRAD placed them (CalcFaceVectors and LuxelSpaceToWorld
// in src/utils/vrad/lightmap.cpp and radial.cpp); displacements add theirs in AddDisplacement.
uint32_t AddFaceLightmap(const Lumps& lumps, size_t faceIndex, bool displacement)
{
    if (lumps.baked == nullptr || lumps.baked->samples.empty() || faceIndex >= lumps.lightFaces.size())
        return kNoFace;
    const Face& face = lumps.lightFaces[faceIndex];
    if (face.lightOffset < 0 || face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= lumps.texinfos.size() || face.planeNum >= lumps.planes.size())
        return kNoFace;
    const Texinfo& texinfo = lumps.texinfos[face.texinfo];
    if ((texinfo.flags & (kSurfSky | kSurfNoLight)) != 0)
        return kNoFace;
    uint32_t style = 0;
    while (style < 4 && face.styles[style] != 0 && face.styles[style] != 255)
        ++style;
    if (style == 4 || face.styles[style] != 0 || face.lightmapSize[0] < 0 || face.lightmapSize[1] < 0)
        return kNoFace;
    const bool bumped = (texinfo.flags & kSurfBumpLight) != 0;
    const uint32_t width = static_cast<uint32_t>(face.lightmapSize[0]) + 1;
    const uint32_t height = static_cast<uint32_t>(face.lightmapSize[1]) + 1;
    const size_t luxels = static_cast<size_t>(width) * height;
    const size_t perStyle = luxels * (bumped ? 4 : 1);
    const size_t sample = static_cast<size_t>(face.lightOffset) / 4 + style * perStyle;
    if (sample + perStyle > lumps.baked->samples.size())
        return kNoFace;

    const Plane& plane = lumps.planes[face.planeNum];
    FaceLightmap lightmap = {};
    lightmap.sample = static_cast<uint32_t>(sample);
    lightmap.width = width;
    lightmap.height = height;
    lightmap.bumped = bumped ? 1u : 0u;
    lightmap.normal = plane.normal;
    // GetBumpNormals (src/mathlib/bumpvects.cpp) around the face normal.
    const Float3 s = {texinfo.textureVecs[0][0], texinfo.textureVecs[0][1], texinfo.textureVecs[0][2]};
    const Float3 t = {texinfo.textureVecs[1][0], texinfo.textureVecs[1][1], texinfo.textureVecs[1][2]};
    const bool leftHanded = Dot(plane.normal, Cross(s, t)) < 0.0f;
    lightmap.tangentT = Normalize(Cross(plane.normal, s));
    lightmap.tangentS = Normalize(Cross(lightmap.tangentT, plane.normal));
    if (leftHanded)
        lightmap.tangentT = Scale(lightmap.tangentT, -1.0f);
    const uint32_t index = static_cast<uint32_t>(lumps.baked->faces.size());
    lumps.baked->faces.push_back(lightmap);
    if (displacement)
        return index;

    const auto& w = texinfo.lightmapVecs;
    const Float3 w0 = {w[0][0], w[0][1], w[0][2]};
    const Float3 w1 = {w[1][0], w[1][1], w[1][2]};
    const Float3 cross = {w[1][1] * w[0][2] - w[1][2] * w[0][1], w[1][2] * w[0][0] - w[1][0] * w[0][2], w[1][0] * w[0][1] - w[1][1] * w[0][0]};
    const float det = -Dot(plane.normal, cross);
    if (std::fabs(det) < 1e-20f)
        return index;
    const Float3& n = plane.normal;
    const Float3 toWorld0 = {(n.z * w1.y - n.y * w1.z) / det, (n.x * w1.z - n.z * w1.x) / det, (n.y * w1.x - n.x * w1.y) / det};
    const Float3 toWorld1 = {(n.y * w0.z - n.z * w0.y) / det, (n.z * w0.x - n.x * w0.z) / det, (n.x * w0.y - n.y * w0.x) / det};
    Float3 origin = Scale(cross, -plane.distance / det);
    origin = Add(origin, Scale(toWorld0, -w[0][3]));
    origin = Add(origin, Scale(toWorld1, -w[1][3]));
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float sCoord = static_cast<float>(static_cast<int32_t>(x) + face.lightmapMins[0]);
            const float tCoord = static_cast<float>(static_cast<int32_t>(y) + face.lightmapMins[1]);
            const Float3 position = Add(Add(origin, Scale(toWorld0, sCoord)), Scale(toWorld1, tCoord));
            lumps.baked->luxels.push_back({position, index, plane.normal, y * width + x});
        }
    }
    return index;
}

uint32_t PackUnorm(Float3 c)
{
    auto channel = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return channel(c.x) | channel(c.y) << 8 | channel(c.z) << 16 | 0xFF000000u;
}

uint32_t FaceMaterial(const Lumps& lumps, const Face& face)
{
    if (face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= lumps.texinfos.size())
        return kNoMaterial;
    const int32_t texdata = lumps.texinfos[face.texinfo].texdata;
    return texdata >= 0 && static_cast<size_t>(texdata) < lumps.texdatas.size() ? static_cast<uint32_t>(texdata) : kNoMaterial;
}

// Texture coordinates of a point on a face: texinfo's texture vectors in texels, over the texture size.
Float2 FaceUv(const Lumps& lumps, const Face& face, Float3 p)
{
    const uint32_t material = FaceMaterial(lumps, face);
    if (material == kNoMaterial)
        return {};
    const Texinfo& texinfo = lumps.texinfos[face.texinfo];
    const Texdata& texdata = lumps.texdatas[material];
    auto axis = [&](const float (&v)[4], int32_t size) {
        return (v[0] * p.x + v[1] * p.y + v[2] * p.z + v[3]) / static_cast<float>(std::max(size, 1));
    };
    return {axis(texinfo.textureVecs[0], texdata.width), axis(texinfo.textureVecs[1], texdata.height)};
}

uint32_t FaceAlbedo(const Lumps& lumps, const Face& face)
{
    constexpr Float3 kFallback = {0.5f, 0.5f, 0.5f};
    if (face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= lumps.texinfos.size())
        return PackUnorm(kFallback);
    const int32_t texdata = lumps.texinfos[face.texinfo].texdata;
    if (texdata < 0 || static_cast<size_t>(texdata) >= lumps.texdatas.size())
        return PackUnorm(kFallback);
    return PackUnorm(lumps.texdatas[texdata].reflectivity);
}

// Material names come from the texdata string table: offsets into a block of NUL-terminated strings.
void ReadMaterialNames(std::span<const std::byte> file, const Header& header, std::span<const Texdata> texdatas, std::vector<std::string>& names)
{
    names.assign(texdatas.size(), {});
    auto table = LumpArray<int32_t>(file, header, kLumpTexdataStringTable);
    const Lump& data = header.lumps[kLumpTexdataStringData];
    if (!table || data.offset < 0 || data.length <= 0 || static_cast<size_t>(data.offset) + data.length > file.size())
        return;
    const std::string_view strings(reinterpret_cast<const char*>(file.data() + data.offset), data.length);
    for (size_t i = 0; i < texdatas.size(); ++i) {
        const int32_t id = texdatas[i].nameStringTableId;
        if (id < 0 || static_cast<size_t>(id) >= table->size())
            continue;
        const int32_t offset = (*table)[id];
        if (offset < 0 || static_cast<size_t>(offset) >= strings.size())
            continue;
        const std::string_view rest = strings.substr(offset);
        names[i] = std::string(rest.substr(0, rest.find('\0')));
    }
}

bool IsSkipped(const Lumps& lumps, const Face& face)
{
    return face.texinfo >= 0 && static_cast<size_t>(face.texinfo) < lumps.texinfos.size() && (lumps.texinfos[face.texinfo].flags & kSkippedSurfaces) != 0;
}

bool FacePolygon(const Lumps& lumps, const Face& face, std::vector<Float3>& polygon)
{
    polygon.clear();
    for (int i = 0; i < face.numEdges; ++i) {
        size_t surfedgeIndex = static_cast<size_t>(face.firstEdge) + i;
        if (surfedgeIndex >= lumps.surfedges.size())
            return false;
        int32_t surfedge = lumps.surfedges[surfedgeIndex];
        size_t edge = static_cast<size_t>(surfedge < 0 ? -surfedge : surfedge);
        if (edge * 2 + 1 >= lumps.edges.size())
            return false;
        uint16_t vertex = lumps.edges[edge * 2 + (surfedge < 0 ? 1 : 0)];
        if (vertex >= lumps.vertices.size())
            return false;
        polygon.push_back(lumps.vertices[vertex]);
    }
    return polygon.size() >= 3;
}

// `lightmap` is the face's AddFaceLightmap entry, or kNoFace.
void AddPolygon(Mesh& mesh, const Lumps& lumps, const Face& face, const std::vector<Float3>& polygon, uint32_t lightmap = kNoFace)
{
    const uint32_t albedo = FaceAlbedo(lumps, face);
    const uint32_t material = FaceMaterial(lumps, face);
    uint32_t base = static_cast<uint32_t>(mesh.positions.size());
    for (const Float3& p : polygon) {
        mesh.positions.push_back(p);
        mesh.uvs.push_back(FaceUv(lumps, face, p));
        mesh.luxels.push_back(lightmap != kNoFace ? FaceLuxel(lumps, face, p) : Float2{});
    }
    for (uint32_t i = 1; i + 1 < polygon.size(); ++i) {
        mesh.indices.push_back(base);
        mesh.indices.push_back(base + i);
        mesh.indices.push_back(base + i + 1);
        mesh.triangleAlbedo.push_back(albedo);
        mesh.triangleMaterials.push_back(material);
        mesh.triangleFaces.push_back(lightmap);
    }
}

// A displacement's luxels span its grid from the start corner (CCoreDispSurface::CalcLuxelCoords and
// CCoreDispInfo::GenerateDispSurf in src/public/builddisp.cpp): vertex (row i, column j) of n per side is at luxel
// (width * j / (n - 1), height * i / (n - 1)), counting the lightmap's luxels less one.
bool AddDisplacement(Mesh& mesh, const Lumps& lumps, const Face& face, const DispInfo& disp, const std::vector<Float3>& corners, uint32_t lightmap = kNoFace)
{
    const uint32_t albedo = FaceAlbedo(lumps, face);
    const uint32_t material = FaceMaterial(lumps, face);
    if (corners.size() != 4 || disp.power < 2 || disp.power > 4)
        return false;

    size_t start = 0;
    float best = DistanceSquared(corners[0], disp.startPosition);
    for (size_t i = 1; i < 4; ++i) {
        float d = DistanceSquared(corners[i], disp.startPosition);
        if (d < best) {
            best = d;
            start = i;
        }
    }
    Float3 p[4];
    for (size_t i = 0; i < 4; ++i)
        p[i] = corners[(start + i) % 4];

    const int post = (1 << disp.power) + 1;
    if (disp.dispVertStart < 0 || static_cast<size_t>(disp.dispVertStart) + post * post > lumps.dispVerts.size())
        return false;

    const float step = 1.0f / static_cast<float>(post - 1);
    const Float3 edge0 = Scale(Sub(p[1], p[0]), step);
    const Float3 edge1 = Scale(Sub(p[2], p[3]), step);
    uint32_t base = static_cast<uint32_t>(mesh.positions.size());
    for (int i = 0; i < post; ++i) {
        Float3 end0 = Add(p[0], Scale(edge0, static_cast<float>(i)));
        Float3 end1 = Add(p[3], Scale(edge1, static_cast<float>(i)));
        Float3 segment = Scale(Sub(end1, end0), step);
        for (int j = 0; j < post; ++j) {
            const DispVert& dv = lumps.dispVerts[disp.dispVertStart + i * post + j];
            Float3 flat = Add(end0, Scale(segment, static_cast<float>(j)));
            mesh.positions.push_back(Add(flat, Scale(dv.vec, dv.dist)));
            mesh.uvs.push_back(FaceUv(lumps, face, flat));
            mesh.luxels.push_back({static_cast<float>(face.lightmapSize[0]) * static_cast<float>(j) * step,
                static_cast<float>(face.lightmapSize[1]) * static_cast<float>(i) * step});
        }
    }
    if (lightmap != kNoFace) {
        const FaceLightmap& entry = lumps.baked->faces[lightmap];
        const Float3* grid = mesh.positions.data() + base;
        auto at = [&](int i, int j) { return grid[std::clamp(i, 0, post - 1) * post + std::clamp(j, 0, post - 1)]; };
        auto normalAt = [&](int i, int j) {
            Float3 normal = Normalize(Cross(Sub(at(i + 1, j), at(i - 1, j)), Sub(at(i, j + 1), at(i, j - 1))));
            return Dot(normal, entry.normal) < 0.0f ? Scale(normal, -1.0f) : normal;
        };
        const float spanS = std::max(static_cast<float>(entry.width - 1), 1.0f);
        const float spanT = std::max(static_cast<float>(entry.height - 1), 1.0f);
        for (uint32_t y = 0; y < entry.height; ++y) {
            for (uint32_t x = 0; x < entry.width; ++x) {
                const float gj = static_cast<float>(x) / spanS * static_cast<float>(post - 1);
                const float gi = static_cast<float>(y) / spanT * static_cast<float>(post - 1);
                const int i0 = std::min(static_cast<int>(gi), post - 2);
                const int j0 = std::min(static_cast<int>(gj), post - 2);
                const float fi = gi - static_cast<float>(i0);
                const float fj = gj - static_cast<float>(j0);
                auto bilinear = [&](auto value) {
                    return Add(Add(Scale(value(i0, j0), (1 - fi) * (1 - fj)), Scale(value(i0, j0 + 1), (1 - fi) * fj)),
                        Add(Scale(value(i0 + 1, j0), fi * (1 - fj)), Scale(value(i0 + 1, j0 + 1), fi * fj)));
                };
                lumps.baked->luxels.push_back({bilinear(at), lightmap, Normalize(bilinear(normalAt)), y * entry.width + x});
            }
        }
    }
    for (int i = 0; i + 1 < post; ++i) {
        for (int j = 0; j + 1 < post; ++j) {
            uint32_t a = base + i * post + j;
            uint32_t b = a + 1;
            uint32_t c = a + post;
            uint32_t d = c + 1;
            // The engine alternates the diagonal by vertex index (CCoreDispInfo::GetTriangleIndicesForDispBBox); on
            // curved displacements the other split lies a few units off the drawn surface and shadows it.
            if ((i * post + j) % 2 == 1)
                mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
            else
                mesh.indices.insert(mesh.indices.end(), {a, c, d, a, d, b});
            mesh.triangleAlbedo.insert(mesh.triangleAlbedo.end(), {albedo, albedo});
            mesh.triangleMaterials.insert(mesh.triangleMaterials.end(), {material, material});
            mesh.triangleFaces.insert(mesh.triangleFaces.end(), {lightmap, lightmap});
        }
    }
    return true;
}

struct SkyCamera {
    Float3 origin;
    float scale = 0.0f;
};

std::optional<SkyCamera> ReadSkyCamera(std::span<const std::byte> file, const Header& header)
{
    const Lump& lump = header.lumps[kLumpEntities];
    if (lump.offset < 0 || lump.length <= 0 || static_cast<size_t>(lump.offset) + lump.length > file.size())
        return std::nullopt;
    std::string_view entities(reinterpret_cast<const char*>(file.data() + lump.offset), lump.length);
    size_t classPos = entities.find("\"sky_camera\"");
    if (classPos == std::string_view::npos)
        return std::nullopt;
    size_t blockStart = entities.rfind('{', classPos);
    size_t blockEnd = entities.find('}', classPos);
    if (blockStart == std::string_view::npos || blockEnd == std::string_view::npos)
        return std::nullopt;
    std::string block(entities.substr(blockStart, blockEnd - blockStart));
    size_t originPos = block.find("\"origin\"");
    if (originPos == std::string::npos)
        return std::nullopt;
    SkyCamera camera;
    if (sscanf_s(block.c_str() + originPos + 8, " \"%f %f %f\"", &camera.origin.x, &camera.origin.y, &camera.origin.z) != 3)
        return std::nullopt;
    if (const size_t scalePos = block.find("\"scale\""); scalePos != std::string::npos)
        sscanf_s(block.c_str() + scalePos + 7, " \"%f\"", &camera.scale);
    return camera;
}

struct AreaLocator {
    std::span<const Plane> planes;
    std::span<const Node> nodes;
    std::span<const std::byte> leafBytes;
    size_t leafStride = 0;

    int AreaAt(Float3 point) const
    {
        int32_t node = 0;
        for (size_t steps = 0; node >= 0 && steps <= nodes.size(); ++steps) {
            const Node& current = nodes[node];
            if (current.planeNum < 0 || static_cast<size_t>(current.planeNum) >= planes.size())
                return -1;
            const Plane& plane = planes[current.planeNum];
            float side = plane.normal.x * point.x + plane.normal.y * point.y + plane.normal.z * point.z - plane.distance;
            node = current.children[side >= 0.0f ? 0 : 1];
        }
        const int32_t leafIndex = -node - 1;
        if (node >= 0 || leafIndex < 0 || static_cast<size_t>(leafIndex) * leafStride + sizeof(Leaf) > leafBytes.size())
            return -1;
        Leaf leaf;
        std::memcpy(&leaf, leafBytes.data() + leafIndex * leafStride, sizeof(leaf));
        return leaf.areaFlags & 0x1FF;
    }

    // The bounds of an area's leaves; false when it has none.
    bool Bounds(int area, Float3& mins, Float3& maxs) const
    {
        bool found = false;
        for (size_t offset = 0; offset + sizeof(Leaf) <= leafBytes.size(); offset += leafStride) {
            Leaf leaf;
            std::memcpy(&leaf, leafBytes.data() + offset, sizeof(leaf));
            if ((leaf.areaFlags & 0x1FF) != area || leaf.contents != 0)
                continue;
            const Float3 low = {static_cast<float>(leaf.mins[0]), static_cast<float>(leaf.mins[1]), static_cast<float>(leaf.mins[2])};
            const Float3 high = {static_cast<float>(leaf.maxs[0]), static_cast<float>(leaf.maxs[1]), static_cast<float>(leaf.maxs[2])};
            mins = found ? Float3{std::min(mins.x, low.x), std::min(mins.y, low.y), std::min(mins.z, low.z)} : low;
            maxs = found ? Float3{std::max(maxs.x, high.x), std::max(maxs.y, high.y), std::max(maxs.z, high.z)} : high;
            found = true;
        }
        return found;
    }
};

// Run-length decoding as in DecompressVis (utils/common/bsplib.cpp): a zero byte is followed by a count of zero bytes.
bool DecompressVis(std::span<const std::byte> in, std::span<uint8_t> out)
{
    size_t read = 0;
    size_t written = 0;
    while (written < out.size()) {
        if (read >= in.size())
            return false;
        const uint8_t value = static_cast<uint8_t>(in[read]);
        if (value != 0) {
            out[written++] = value;
            ++read;
            continue;
        }
        if (read + 1 >= in.size() || in[read + 1] == std::byte{0})
            return false;
        written = std::min(out.size(), written + static_cast<uint8_t>(in[read + 1]));
        read += 2;
    }
    return true;
}

void ReadVisibility(std::span<const std::byte> file, const Header& header, const SunLight& sun, Visibility& visibility)
{
    auto planes = LumpArray<Plane>(file, header, kLumpPlanes);
    auto nodes = LumpArray<Node>(file, header, kLumpNodes);
    if (!planes || !nodes || nodes->empty())
        return;
    const Lump& leafLump = header.lumps[kLumpLeafs];
    const size_t leafStride = leafLump.version == 0 ? kLeafV0Size : kLeafV1Size;
    if (leafLump.length <= 0 || static_cast<size_t>(leafLump.offset) + leafLump.length > file.size() || leafLump.length % leafStride != 0)
        return;
    const Lump& visLump = header.lumps[kLumpVisibility];
    if (visLump.length < static_cast<int32_t>(sizeof(int32_t)) ||
        static_cast<size_t>(visLump.offset) + visLump.length > file.size())
        return;
    const std::span<const std::byte> vis = file.subspan(visLump.offset, visLump.length);
    int32_t clusterCount;
    std::memcpy(&clusterCount, vis.data(), sizeof(clusterCount));
    if (clusterCount <= 0 || (static_cast<size_t>(clusterCount) * 2 + 1) * sizeof(int32_t) > vis.size())
        return;
    const size_t row = (static_cast<size_t>(clusterCount) + 7) / 8;
    std::vector<uint8_t> pvs(row * clusterCount);
    for (int32_t c = 0; c < clusterCount; ++c) {
        int32_t offset;
        std::memcpy(&offset, vis.data() + sizeof(int32_t) * (1 + 2 * c), sizeof(offset));
        if (offset < 0 || static_cast<size_t>(offset) >= vis.size() || !DecompressVis(vis.subspan(offset), std::span(pvs).subspan(c * row, row)))
            return;
    }
    std::vector<BspLeaf> leaves;
    std::vector<uint8_t> sunClusters(row, 0);
    const auto seeSky = [&](int32_t cluster) {
        if (cluster < 0 || cluster >= clusterCount)
            return;
        for (size_t b = 0; b < row; ++b)
            sunClusters[b] |= pvs[cluster * row + b];
    };
    auto leafFaces = LumpArray<uint16_t>(file, header, kLumpLeafFaces);
    auto faces = LumpArray<Face>(file, header, kLumpFaces);
    auto texinfos = LumpArray<Texinfo>(file, header, kLumpTexinfo);
    for (size_t offset = 0; offset < static_cast<size_t>(leafLump.length); offset += leafStride) {
        Leaf leaf;
        std::memcpy(&leaf, file.data() + leafLump.offset + offset, sizeof(leaf));
        for (uint32_t i = 0; leafFaces && faces && texinfos && i < leaf.numLeafFaces; ++i) {
            const size_t at = static_cast<size_t>(leaf.firstLeafFace) + i;
            if (at >= leafFaces->size() || (*leafFaces)[at] >= faces->size())
                break;
            const int32_t texinfo = (*faces)[(*leafFaces)[at]].texinfo;
            if (texinfo >= 0 && static_cast<size_t>(texinfo) < texinfos->size() && ((*texinfos)[texinfo].flags & kSurfSky) != 0) {
                seeSky(leaf.cluster);
                break;
            }
        }
        leaves.push_back({leaf.cluster < clusterCount ? leaf.cluster : -1, leaf.areaFlags & 0x1FF,
            {static_cast<float>(leaf.mins[0]), static_cast<float>(leaf.mins[1]), static_cast<float>(leaf.mins[2])},
            {static_cast<float>(leaf.maxs[0]), static_cast<float>(leaf.maxs[1]), static_cast<float>(leaf.maxs[2])}});
    }
    std::vector<BspNode> tree;
    tree.reserve(nodes->size());
    for (const Node& node : *nodes) {
        if (node.planeNum < 0 || static_cast<size_t>(node.planeNum) >= planes->size())
            return;
        for (int32_t child : node.children) {
            if (child >= 0 ? static_cast<size_t>(child) >= nodes->size() : static_cast<size_t>(-child - 1) >= leaves.size())
                return;
        }
        const Plane& plane = (*planes)[node.planeNum];
        tree.push_back({plane.normal, plane.distance, {node.children[0], node.children[1]}});
    }
    visibility.clusterCount = static_cast<uint32_t>(clusterCount);
    visibility.nodes = std::move(tree);
    visibility.leaves = std::move(leaves);
    if (sun.present) {
        const int32_t leaf = visibility.LeafAt(sun.origin);
        if (leaf >= 0)
            seeSky(visibility.leaves[leaf].cluster);
        visibility.sunClusters = std::move(sunClusters);
    }
    visibility.pvs = std::move(pvs);
}

}

bool InBounds(Float3 point, Float3 mins, Float3 maxs)
{
    return point.x >= mins.x && point.y >= mins.y && point.z >= mins.z && point.x <= maxs.x && point.y <= maxs.y && point.z <= maxs.z;
}

bool WorldGeometry::InSkybox(Float3 point) const
{
    if (skyArea < 0)
        return false;
    if (InBounds(point, skyMins, skyMaxs))
        return true;
    const int32_t leaf = visibility.LeafAt(point);
    return leaf >= 0 && static_cast<size_t>(leaf) < visibility.leaves.size() && visibility.leaves[leaf].area == skyArea;
}

int32_t Visibility::LeafAt(Float3 point) const
{
    if (nodes.empty())
        return -1;
    int32_t node = 0;
    for (size_t steps = 0; node >= 0 && steps <= nodes.size(); ++steps) {
        const BspNode& current = nodes[node];
        node = current.children[point.x * current.normal.x + point.y * current.normal.y + point.z * current.normal.z - current.distance >= 0.0f ? 0 : 1];
    }
    return node < 0 ? -node - 1 : -1;
}

namespace {

std::optional<AreaLocator> MakeAreaLocator(std::span<const std::byte> file, const Header& header)
{
    auto planes = LumpArray<Plane>(file, header, kLumpPlanes);
    auto nodes = LumpArray<Node>(file, header, kLumpNodes);
    const Lump& leafLump = header.lumps[kLumpLeafs];
    const size_t leafStride = leafLump.version == 0 ? kLeafV0Size : kLeafV1Size;
    if (!planes || !nodes || nodes->empty() || leafLump.length <= 0 ||
        static_cast<size_t>(leafLump.offset) + leafLump.length > file.size() || leafLump.length % leafStride != 0)
        return std::nullopt;
    return AreaLocator{*planes, *nodes, file.subspan(leafLump.offset, leafLump.length), leafStride};
}

template <class T>
std::optional<T> ReadAt(std::span<const std::byte> file, size_t offset)
{
    if (offset > file.size() || file.size() - offset < sizeof(T))
        return std::nullopt;
    T value;
    std::memcpy(&value, file.data() + offset, sizeof(T));
    return value;
}

// Matches Source's AngleMatrix: rotation = yaw * pitch * roll, angles in degrees.
void PropTransform(Float3 origin, Float3 angles, float out[3][4])
{
    const float sp = std::sin(angles.x * kDegreesToRadians), cp = std::cos(angles.x * kDegreesToRadians);
    const float sy = std::sin(angles.y * kDegreesToRadians), cy = std::cos(angles.y * kDegreesToRadians);
    const float sr = std::sin(angles.z * kDegreesToRadians), cr = std::cos(angles.z * kDegreesToRadians);
    out[0][0] = cp * cy;
    out[1][0] = cp * sy;
    out[2][0] = -sp;
    out[0][1] = sp * sr * cy - cr * sy;
    out[1][1] = sp * sr * sy + cr * cy;
    out[2][1] = sr * cp;
    out[0][2] = sp * cr * cy + sr * sy;
    out[1][2] = sp * cr * sy - sr * cy;
    out[2][2] = cr * cp;
    out[0][3] = origin.x;
    out[1][3] = origin.y;
    out[2][3] = origin.z;
}

void ReadStaticProps(std::span<const std::byte> file, const Header& header, WorldGeometry& world)
{
    const Lump& lump = header.lumps[kLumpGameLump];
    if (lump.offset < 0 || lump.length <= 0 || static_cast<size_t>(lump.offset) + lump.length > file.size())
        return;
    auto count = ReadAt<int32_t>(file, lump.offset);
    for (int32_t i = 0; count && i < *count; ++i) {
        const size_t entry = lump.offset + 4 + static_cast<size_t>(i) * 16;
        auto id = ReadAt<int32_t>(file, entry);
        auto flags = ReadAt<uint16_t>(file, entry + 4);
        auto version = ReadAt<uint16_t>(file, entry + 6);
        auto offset = ReadAt<int32_t>(file, entry + 8);
        auto length = ReadAt<int32_t>(file, entry + 12);
        if (!id || *id != kStaticPropLumpId || !flags || !version || !offset || !length)
            continue;
        if (*offset < 0 || *length <= 0 || static_cast<size_t>(*offset) + *length > file.size())
            return;
        std::span<const std::byte> data = file.subspan(*offset, *length);
        size_t cursor = 0;
        auto dictEntries = ReadAt<int32_t>(data, cursor);
        if (!dictEntries || *dictEntries < 0)
            return;
        cursor += 4;
        for (int32_t d = 0; d < *dictEntries; ++d, cursor += kStaticPropNameLength) {
            if (cursor + kStaticPropNameLength > data.size())
                return;
            const char* name = reinterpret_cast<const char*>(data.data() + cursor);
            world.propModels.emplace_back(name, strnlen(name, kStaticPropNameLength));
        }
        auto leafEntries = ReadAt<int32_t>(data, cursor);
        if (!leafEntries || *leafEntries < 0)
            return;
        cursor += 4 + static_cast<size_t>(*leafEntries) * sizeof(uint16_t);
        auto propEntries = ReadAt<int32_t>(data, cursor);
        cursor += 4;
        if (!propEntries || *propEntries <= 0 || cursor > data.size() || (data.size() - cursor) / *propEntries < 56)
            return;
        const size_t stride = (data.size() - cursor) / *propEntries;
        for (int32_t p = 0; p < *propEntries; ++p) {
            const size_t prop = cursor + static_cast<size_t>(p) * stride;
            auto origin = ReadAt<Float3>(data, prop);
            auto angles = ReadAt<Float3>(data, prop + 12);
            auto model = ReadAt<uint16_t>(data, prop + 24);
            std::optional<uint32_t> propFlags = *version <= 6 ? std::optional<uint32_t>(ReadAt<uint8_t>(data, prop + 31).value_or(0))
                                                             : ReadAt<uint32_t>(data, prop + 64);
            if (!origin || !angles || !model || *model >= world.propModels.size())
                continue;
            StaticProp staticProp;
            staticProp.model = *model;
            PropTransform(*origin, *angles, staticProp.transform);
            staticProp.noShadow = (propFlags.value_or(0) & kStaticPropNoShadow) != 0;
            staticProp.ignoreNormals = (propFlags.value_or(0) & kStaticPropIgnoreNormals) != 0;
            world.props.push_back(staticProp);
        }
        return;
    }
}

// The lighting lump the engine uses (HDR first) and the faces that index it.
void ReadLighting(std::span<const std::byte> file, const Header& header, Lumps& lumps, BakedLighting& baked)
{
    auto hdr = LumpArray<uint32_t>(file, header, kLumpLightingHdr);
    auto ldr = LumpArray<uint32_t>(file, header, kLumpLighting);
    baked.hdr = hdr && !hdr->empty();
    const std::optional<std::span<const uint32_t>> samples = baked.hdr ? hdr : ldr;
    if (!samples || samples->empty())
        return;
    baked.samples.assign(samples->begin(), samples->end());
    lumps.lightFaces = lumps.faces;
    auto hdrFaces = LumpArray<Face>(file, header, kLumpFacesHdr);
    if (baked.hdr && hdrFaces && hdrFaces->size() == lumps.faces.size())
        lumps.lightFaces = *hdrFaces;
}

// The leaf ambient samples (LUMP_LEAF_AMBIENT_*, HDR first), placed in their leaves' bounds.
void ReadLeafAmbient(std::span<const std::byte> file, const Header& header, const Visibility& visibility, BakedLighting& baked)
{
    const bool hdr = header.lumps[kLumpLeafAmbientIndexHdr].length > 0;
    auto indices = LumpArray<LeafAmbientIndex>(file, header, hdr ? kLumpLeafAmbientIndexHdr : kLumpLeafAmbientIndex);
    auto lighting = LumpArray<LeafAmbientLighting>(file, header, hdr ? kLumpLeafAmbientLightingHdr : kLumpLeafAmbientLighting);
    if (!indices || !lighting || indices->size() != visibility.leaves.size())
        return;
    baked.leafAmbient.reserve(indices->size());
    for (size_t leaf = 0; leaf < indices->size(); ++leaf) {
        const LeafAmbientIndex& index = (*indices)[leaf];
        const BspLeaf& bounds = visibility.leaves[leaf];
        const uint32_t first = static_cast<uint32_t>(baked.ambientSamples.size());
        for (uint32_t i = 0; i < index.sampleCount && static_cast<size_t>(index.firstSample) + i < lighting->size(); ++i) {
            const LeafAmbientLighting& source = (*lighting)[index.firstSample + i];
            AmbientSample& sample = baked.ambientSamples.emplace_back();
            const float fraction[3] = {source.x / 255.0f, source.y / 255.0f, source.z / 255.0f};
            sample.position[0] = bounds.mins.x + fraction[0] * (bounds.maxs.x - bounds.mins.x);
            sample.position[1] = bounds.mins.y + fraction[1] * (bounds.maxs.y - bounds.mins.y);
            sample.position[2] = bounds.mins.z + fraction[2] * (bounds.maxs.z - bounds.mins.z);
            // VRAD stores the cubes in linear units with VectorToColorRGBExp32, whose inverse is ColorRGBExp32ToVector:
            // c * 2^exponent, 255 times TexLightToLinear's lightmap scale (src/mathlib/color_conversion.cpp).
            for (int side = 0; side < 6; ++side) {
                const float scale = std::ldexp(1.0f, static_cast<int8_t>(source.cube[side][3]));
                for (int c = 0; c < 3; ++c)
                    sample.cube[side][c] = source.cube[side][c] * scale;
            }
        }
        baked.leafAmbient.push_back({first, static_cast<uint32_t>(baked.ambientSamples.size()) - first});
    }
    // Leaves without samples of their own hold the nearest leaf with samples in firstAmbientSample
    // (ComputePerLeafAmbientLighting and NearestNeighborWithLight in src/utils/vrad/leaf_ambient_lighting.cpp).
    for (size_t leaf = 0; leaf < indices->size(); ++leaf) {
        const LeafAmbientIndex& index = (*indices)[leaf];
        if (index.sampleCount == 0 && index.firstSample < baked.leafAmbient.size())
            baked.leafAmbient[leaf] = baked.leafAmbient[index.firstSample];
    }
}

void ReadLights(std::span<const std::byte> file, const Header& header, WorldGeometry& world)
{
    for (int lumpIndex : {kLumpWorldLightsHdr, kLumpWorldLights}) {
        const Lump& lump = header.lumps[lumpIndex];
        size_t stride = lump.version == 1 ? kWorldLightV1Size : sizeof(WorldLight);
        if (lump.length <= 0 || static_cast<size_t>(lump.offset) + lump.length > file.size() || lump.length % stride != 0)
            continue;
        for (size_t offset = 0; offset < static_cast<size_t>(lump.length); offset += stride) {
            const std::byte* entry = file.data() + lump.offset + offset;
            WorldLight light;
            std::memcpy(&light, entry, 36);
            const size_t tail = stride == kWorldLightV1Size ? 48 : 36;
            std::memcpy(&light.cluster, entry + tail, sizeof(WorldLight) - 36);
            switch (static_cast<LightType>(light.type)) {
            case LightType::Sky:
                world.sun.present = true;
                world.sun.origin = light.origin;
                world.sun.direction = Scale(light.normal, -1.0f);
                world.sun.color = light.intensity;
                world.sun.style = light.style;
                break;
            case LightType::SkyAmbient:
                world.sun.ambient = light.intensity;
                world.sun.ambientStyle = light.style;
                break;
            case LightType::Surface:
            case LightType::Point:
            case LightType::Spot:
                world.lights.push_back({static_cast<LightType>(light.type), light.origin, light.intensity, light.normal, light.stopdot,
                    light.stopdot2, light.exponent, light.radius, light.constantAttn, light.linearAttn, light.quadraticAttn, light.cluster, light.style,
                    (light.flags & kWorldLightInAmbientCube) != 0});
                break;
            default:
                break;
            }
        }
        // The HDR lump is preferred; the LDR one is only a fallback for maps compiled without HDR.
        if (world.sun.present || !world.lights.empty())
            return;
    }
}

void* LzmaAlloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
void LzmaFree(ISzAllocPtr, void* address) { std::free(address); }
const ISzAlloc kLzmaAllocator = {LzmaAlloc, LzmaFree};

// Appends the decoded contents of a Source LZMA block (lzma_header_t in the SDK's lzmaDecoder.h), 4-byte aligned.
std::optional<int32_t> AppendDecompressed(std::span<const std::byte> block, int32_t size, std::vector<std::byte>& out)
{
    const std::optional<LzmaHeader> lzma = ReadAt<LzmaHeader>(block, 0);
    const size_t offset = (out.size() + 3) & ~size_t{3};
    if (!lzma || lzma->id != kLzmaId || size <= 0 || lzma->actualSize != static_cast<uint32_t>(size) ||
        lzma->lzmaSize > block.size() - sizeof(LzmaHeader) || offset + size > static_cast<size_t>(INT32_MAX))
        return std::nullopt;
    out.resize(offset + size);
    SizeT outSize = static_cast<SizeT>(size);
    SizeT inSize = lzma->lzmaSize;
    ELzmaStatus status;
    if (LzmaDecode(reinterpret_cast<Byte*>(out.data() + offset), &outSize, reinterpret_cast<const Byte*>(block.data() + sizeof(LzmaHeader)), &inSize,
            lzma->properties, LZMA_PROPS_SIZE, LZMA_FINISH_END, &status, &kLzmaAllocator) != SZ_OK ||
        outSize != static_cast<SizeT>(size))
        return std::nullopt;
    return static_cast<int32_t>(offset);
}

// Any lump, and each game lump, may be LZMA-compressed. Such a map is copied into out with those lumps appended
// decoded and the header and game lump directory pointed at them, so the readers above only see plain lumps.
// out stays empty when nothing is compressed.
bool DecompressLumps(std::span<const std::byte> file, const Header& header, std::vector<std::byte>& out, std::string& error)
{
    const auto append = [&](std::span<const std::byte> block, int32_t size) {
        if (out.empty())
            out.assign(file.begin(), file.end());
        return AppendDecompressed(block, size, out);
    };

    Header decoded = header;
    for (int i = 0; i < kLumpCount; ++i) {
        Lump& lump = decoded.lumps[i];
        if (lump.fourCC == 0)
            continue;
        std::optional<int32_t> offset;
        if (lump.offset >= 0 && lump.length >= 0 && static_cast<size_t>(lump.offset) + lump.length <= file.size())
            offset = append(file.subspan(lump.offset, lump.length), lump.fourCC);
        if (!offset) {
            error = std::format("lump {} could not be decompressed", i);
            return false;
        }
        lump.offset = *offset;
        lump.length = lump.fourCC;
        lump.fourCC = 0;
    }
    if (!out.empty())
        std::memcpy(out.data(), &decoded, sizeof(decoded));

    // Game lump offsets are absolute in the original file; a compressed one's length is its decoded size.
    const Lump& games = decoded.lumps[kLumpGameLump];
    const auto current = [&] { return out.empty() ? file : std::span<const std::byte>(out); };
    const std::optional<int32_t> count = games.offset >= 0 ? ReadAt<int32_t>(current(), games.offset) : std::nullopt;
    for (int32_t i = 0; count && i < *count && sizeof(int32_t) + (i + 1) * sizeof(GameLump) <= static_cast<size_t>(games.length); ++i) {
        const size_t entryOffset = games.offset + sizeof(int32_t) + i * sizeof(GameLump);
        std::optional<GameLump> entry = ReadAt<GameLump>(current(), entryOffset);
        if (!entry || (entry->flags & kGameLumpCompressed) == 0 || entry->length == 0)
            continue;
        std::optional<int32_t> offset;
        if (entry->offset >= 0 && static_cast<size_t>(entry->offset) < file.size())
            offset = append(file.subspan(entry->offset), entry->length);
        if (!offset) {
            error = std::format("game lump {} could not be decompressed", i);
            return false;
        }
        entry->offset = *offset;
        entry->flags &= ~kGameLumpCompressed;
        std::memcpy(out.data() + entryOffset, &*entry, sizeof(GameLump));
    }
    return true;
}

}

bool LoadWorld(std::span<const std::byte> file, WorldGeometry& world, std::string& error)
{
    if (file.size() < sizeof(Header)) {
        error = "file is smaller than a BSP header";
        return false;
    }
    Header header;
    std::memcpy(&header, file.data(), sizeof(header));
    if (header.ident != kIdent) {
        error = "missing VBSP identifier";
        return false;
    }
    if (header.version < 19 || header.version > 21) {
        error = std::format("unsupported BSP version {}", header.version);
        return false;
    }
    std::vector<std::byte> decompressed;
    if (!DecompressLumps(file, header, decompressed, error))
        return false;
    if (!decompressed.empty()) {
        file = decompressed;
        std::memcpy(&header, file.data(), sizeof(header));
    }

    auto vertices = LumpArray<Float3>(file, header, kLumpVertexes);
    auto surfedges = LumpArray<int32_t>(file, header, kLumpSurfedges);
    auto edges = LumpArray<uint16_t>(file, header, kLumpEdges, sizeof(uint16_t) * 2);
    auto faces = LumpArray<Face>(file, header, kLumpFaces);
    auto texinfos = LumpArray<Texinfo>(file, header, kLumpTexinfo);
    auto texdatas = LumpArray<Texdata>(file, header, kLumpTexdata);
    auto models = LumpArray<Model>(file, header, kLumpModels);
    auto dispinfos = LumpArray<DispInfo>(file, header, kLumpDispInfo);
    auto dispVerts = LumpArray<DispVert>(file, header, kLumpDispVerts);
    if (!vertices || !surfedges || !edges || !faces || !texinfos || !texdatas || !models || models->empty() || !dispinfos || !dispVerts) {
        error = "a required lump is missing or malformed";
        return false;
    }

    Lumps lumps;
    lumps.vertices = *vertices;
    lumps.surfedges = *surfedges;
    lumps.edges = std::span<const uint16_t>(reinterpret_cast<const uint16_t*>(edges->data()), edges->size() * 2);
    lumps.faces = *faces;
    lumps.texinfos = *texinfos;
    lumps.texdatas = *texdatas;
    lumps.dispinfos = *dispinfos;
    lumps.dispVerts = *dispVerts;

    world = {};
    auto planeLump = LumpArray<Plane>(file, header, kLumpPlanes);
    if (planeLump)
        lumps.planes = *planeLump;
    lumps.baked = &world.baked;
    ReadLighting(file, header, lumps, world.baked);
    // The scaled-down 3D skybox scene is world geometry placed outside the playable space; it must not
    // occlude the main view's rays, so faces and props in the sky camera's area are kept apart.
    const std::optional<AreaLocator> areas = MakeAreaLocator(file, header);
    const std::optional<SkyCamera> skyCamera = ReadSkyCamera(file, header);
    int skyArea = areas && skyCamera ? areas->AreaAt(skyCamera->origin) : -1;
    // ProcessSkyCameras in src/utils/vrad/vrad.cpp keeps only cameras with a positive scale.
    if (skyCamera && skyCamera->scale > 0.0f) {
        world.sun.skyCameraOrigin = skyCamera->origin;
        world.sun.skyCameraScale = skyCamera->scale;
    }
    if (skyArea >= 0 && !areas->Bounds(skyArea, world.skyMins, world.skyMaxs))
        skyArea = -1;
    world.skyArea = skyArea;
    const auto inSkybox = [&](Float3 point) { return skyArea >= 0 && (InBounds(point, world.skyMins, world.skyMaxs) || areas->AreaAt(point) == skyArea); };
    auto planes = LumpArray<Plane>(file, header, kLumpPlanes);
    const Model& worldModel = (*models)[0];
    std::vector<Float3> polygon;
    for (int32_t f = worldModel.firstFace; f < worldModel.firstFace + worldModel.numFaces; ++f) {
        if (f < 0 || static_cast<size_t>(f) >= lumps.faces.size())
            break;
        const Face& face = lumps.faces[f];
        if (IsSkipped(lumps, face) || !FacePolygon(lumps, face, polygon))
            continue;
        if (skyArea >= 0 && planes && face.planeNum < planes->size()) {
            Float3 center = {};
            for (const Float3& p : polygon)
                center = Add(center, p);
            center = Scale(center, 1.0f / static_cast<float>(polygon.size()));
            Float3 normal = (*planes)[face.planeNum].normal;
            if (face.side != 0)
                normal = Scale(normal, -1.0f);
            if (inSkybox(Add(center, normal))) {
                if (face.dispinfo >= 0 && static_cast<size_t>(face.dispinfo) < lumps.dispinfos.size())
                    AddDisplacement(world.skybox, lumps, face, lumps.dispinfos[face.dispinfo], polygon);
                else
                    AddPolygon(world.skybox, lumps, face, polygon);
                ++world.skyboxFaceCount;
                continue;
            }
        }
        if (face.dispinfo >= 0 && static_cast<size_t>(face.dispinfo) < lumps.dispinfos.size()) {
            const DispInfo& disp = lumps.dispinfos[face.dispinfo];
            const size_t luxels = world.baked.luxels.size();
            const uint32_t lightmap = AddFaceLightmap(lumps, static_cast<size_t>(f), true);
            if (AddDisplacement(world.mesh, lumps, face, disp, polygon, lightmap)) {
                ++world.displacementCount;
            } else if (lightmap != kNoFace) {
                world.baked.faces.pop_back();
                world.baked.luxels.resize(luxels);
            }
            continue;
        }
        AddPolygon(world.mesh, lumps, face, polygon, AddFaceLightmap(lumps, static_cast<size_t>(f), false));
        ++world.faceCount;
    }

    world.brushModels.resize(models->size());
    for (size_t m = 1; m < models->size(); ++m) {
        const Model& model = (*models)[m];
        for (int32_t f = model.firstFace; f < model.firstFace + model.numFaces; ++f) {
            if (f < 0 || static_cast<size_t>(f) >= lumps.faces.size())
                break;
            const Face& face = lumps.faces[f];
            if (!IsSkipped(lumps, face) && FacePolygon(lumps, face, polygon))
                AddPolygon(world.brushModels[m], lumps, face, polygon);
        }
    }

    ReadMaterialNames(file, header, lumps.texdatas, world.materials);
    ReadLights(file, header, world);
    ReadVisibility(file, header, world.sun, world.visibility);
    ReadLeafAmbient(file, header, world.visibility, world.baked);
    for (const Light& light : world.lights) {
        if (light.inAmbientCube)
            world.baked.ambientLights.push_back({{light.origin.x, light.origin.y, light.origin.z, light.radius},
                {light.intensity.x, light.intensity.y, light.intensity.z, 0.0f}, {light.normal.x, light.normal.y, light.normal.z, 0.0f}});
    }
    ReadStaticProps(file, header, world);
    if (skyArea >= 0) {
        for (const StaticProp& prop : world.props) {
            if (inSkybox({prop.transform[0][3], prop.transform[1][3], prop.transform[2][3]}))
                world.skyboxProps.push_back(prop);
        }
        std::erase_if(world.props, [&](const StaticProp& prop) { return inSkybox({prop.transform[0][3], prop.transform[1][3], prop.transform[2][3]}); });
    }
    if (world.mesh.indices.empty()) {
        error = "the world model has no drawable faces";
        return false;
    }
    return true;
}

}
