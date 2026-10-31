#include "bsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
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
constexpr int kLumpLeafBrushes = 17;
constexpr int kLumpBrushes = 18;
constexpr int kLumpBrushSides = 19;
// bspflags.h: MASK_OPAQUE, the brushes VRAD traces.
constexpr int32_t kMaskOpaque = 0x1 | 0x80 | 0x4000;
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
// VRAD marks the texinfos of faces with a light texture from lights.rad, which it turns into emit_surface lights
// (MakePatchForFace in src/utils/vrad/vrad.cpp).
constexpr int32_t kSurfLight = 0x0001;
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

struct Brush {
    int32_t firstSide;
    int32_t sideCount;
    int32_t contents;
};

struct BrushSide {
    uint16_t planeNum;
    int16_t texinfo;
    int16_t dispInfo;
    uint8_t bevel;
    uint8_t thin;
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

// A VRAD sample of a flat face (BuildFacesamples in src/utils/vrad/lightmap.cpp): the face's part in one lightmap
// cell, in luxel coordinates, and which of SupersampleLightAtPoint's points VRAD would use (bits 0-15: the 4x4 points
// for direct light, 16-19: the 2x2 for ambient light; none when either set is empty, as VRAD then keeps the sample).
struct LuxelCell {
    bool sampled = false;
    Float2 centroid;
    Float2 mins;
    Float2 maxs;
    uint32_t subsamples = 0;
};

// VRAD's smoothing of a face with its neighbours (PairEdges in src/utils/vrad/lightmap.cpp).
struct FaceSmoothing {
    std::vector<uint32_t> neighbours;
    std::vector<Float3> vertexNormals;
    Float3 centre = {};
    bool flat = true;
};

// A flat face's VRAD samples while the map loads, for WeightLuxels.
struct FaceSampling {
    uint32_t face = 0;
    uint32_t lightmap = 0;
    size_t firstLuxel = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // Luxel coordinates (s, t) to world: origin + s * toWorldS + t * toWorldT.
    Float3 origin = {};
    Float3 toWorldS = {};
    Float3 toWorldT = {};
    std::vector<LuxelCell> cells;
    Float3 ToWorld(Float2 luxel) const;
};

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
    const std::vector<FaceSmoothing>* smoothing = nullptr;
    std::vector<FaceSampling>* sampling = nullptr;
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

Float3 FaceSampling::ToWorld(Float2 luxel) const
{
    return Add(Add(origin, Scale(toWorldS, luxel.u)), Scale(toWorldT, luxel.v));
}

bool InConvexPolygon(std::span<const Float2> polygon, Float2 p)
{
    bool positive = false;
    bool negative = false;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const Float2 a = polygon[i];
        const Float2 b = polygon[(i + 1) % polygon.size()];
        const float side = (b.u - a.u) * (p.v - a.v) - (b.v - a.v) * (p.u - a.u);
        positive = positive || side > 0.0f;
        negative = negative || side < 0.0f;
    }
    return !(positive && negative);
}

// BuildFacesamples cuts the face at whole luxel coordinates: cell (s, t) holds [s, s + 1) x [t, t + 1), the first row
// and column also everything below, and nothing past the last. Each piece is sampled at its centroid
// (WindingAreaAndBalancePoint). Only pieces smaller than `partialBelow` (a luxel less EQUAL_EPSILON of world area)
// keep a winding that supersample points must fall in.
LuxelCell ClipLuxelCell(std::span<const Float2> outline, uint32_t s, uint32_t t, float partialBelow)
{
    std::vector<Float2> piece(outline.begin(), outline.end());
    std::vector<Float2> kept;
    // Keeps the side where sign * (coordinate - bound) <= 0.
    const auto clip = [&](bool alongS, float bound, float sign) {
        kept.clear();
        const auto side = [&](Float2 p) { return sign * ((alongS ? p.u : p.v) - bound); };
        for (size_t i = 0; i < piece.size(); ++i) {
            const Float2 a = piece[i];
            const Float2 b = piece[(i + 1) % piece.size()];
            const float da = side(a);
            const float db = side(b);
            if (da <= 0.0f)
                kept.push_back(a);
            if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) {
                const float f = da / (da - db);
                kept.push_back({a.u + (b.u - a.u) * f, a.v + (b.v - a.v) * f});
            }
        }
        piece.swap(kept);
    };
    clip(true, static_cast<float>(s + 1), 1.0f);
    if (s > 0 && piece.size() >= 3)
        clip(true, static_cast<float>(s), -1.0f);
    if (piece.size() >= 3)
        clip(false, static_cast<float>(t + 1), 1.0f);
    if (t > 0 && piece.size() >= 3)
        clip(false, static_cast<float>(t), -1.0f);
    LuxelCell cell;
    if (piece.size() < 3)
        return cell;
    cell.sampled = true;
    cell.mins = cell.maxs = piece[0];
    float area = 0.0f;
    Float2 weighted = {};
    Float2 sum = {};
    for (size_t i = 0; i < piece.size(); ++i) {
        cell.mins = {std::min(cell.mins.u, piece[i].u), std::min(cell.mins.v, piece[i].v)};
        cell.maxs = {std::max(cell.maxs.u, piece[i].u), std::max(cell.maxs.v, piece[i].v)};
        sum = {sum.u + piece[i].u, sum.v + piece[i].v};
        if (i < 2)
            continue;
        const Float2 a = piece[0], b = piece[i - 1], c = piece[i];
        const float triangle = 0.5f * std::fabs((b.u - a.u) * (c.v - a.v) - (b.v - a.v) * (c.u - a.u));
        area += triangle;
        weighted = {weighted.u + triangle * (a.u + b.u + c.u) / 3.0f, weighted.v + triangle * (a.v + b.v + c.v) / 3.0f};
    }
    const float count = static_cast<float>(piece.size());
    cell.centroid = area > 1e-8f ? Float2{weighted.u / area, weighted.v / area} : Float2{sum.u / count, sum.v / count};
    const bool partial = area < partialBelow;
    const auto inside = [&](float du, float dv) { return !partial || InConvexPolygon(piece, {cell.centroid.u + du, cell.centroid.v + dv}); };
    for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
            if (inside(-0.375f + 0.25f * static_cast<float>(a), -0.375f + 0.25f * static_cast<float>(b)))
                cell.subsamples |= 1u << (a * 4 + b);
        }
    }
    for (int k = 0; k < 4; ++k) {
        if (inside(k & 2 ? 0.25f : -0.25f, k & 1 ? 0.25f : -0.25f))
            cell.subsamples |= 1u << (16 + k);
    }
    if ((cell.subsamples & 0xFFFFu) == 0 || (cell.subsamples >> 16) == 0)
        cell.subsamples = 0;
    return cell;
}

// How much a sample adds to the luxel at (s, t) (AddDirectToRadial in src/utils/vrad/radial.cpp): the overlap of its
// bounds with the luxel's two-luxel window over its distance, at least a tenth of a luxel.
float RadialWeight(Float2 coord, Float2 mins, Float2 maxs, float s, float t)
{
    const float s0 = std::max(mins.u - s, -1.0f);
    const float s1 = std::min(maxs.u - s, 1.0f);
    const float t0 = std::max(mins.v - t, -1.0f);
    const float t1 = std::min(maxs.v - t, 1.0f);
    const float overlap = (s1 - s0) * (t1 - t0);
    if (overlap <= 0.001f)
        return 0.0f;
    return overlap / std::max(std::max(std::fabs(coord.u - s), std::fabs(coord.v - t)), 0.1f);
}

// A face's vertex (EdgeVertex in src/utils/vrad/lightmap.cpp), or -1.
int64_t EdgeVertex(const Lumps& lumps, const Face& face, int edge)
{
    if (face.numEdges <= 0)
        return -1;
    const size_t at = static_cast<size_t>(face.firstEdge) + static_cast<size_t>(edge % face.numEdges);
    if (face.firstEdge < 0 || at >= lumps.surfedges.size())
        return -1;
    const int32_t surfedge = lumps.surfedges[at];
    const size_t index = static_cast<size_t>(surfedge < 0 ? -static_cast<int64_t>(surfedge) : surfedge);
    if (index * 2 + 1 >= lumps.edges.size())
        return -1;
    const uint16_t vertex = lumps.edges[index * 2 + (surfedge < 0 ? 1 : 0)];
    return vertex < lumps.vertices.size() ? vertex : -1;
}

// PairEdges in src/utils/vrad/lightmap.cpp: the faces sharing a vertex with each face that VRAD smooths it with
// (normals within 45 degrees, or a shared smoothing group without the hard-edge bit; displacements only with
// displacements), its vertex normals (those faces' normals and its own, summed) and its vertex centre. A face whose
// vertex normals all match its normal is flat.
std::vector<FaceSmoothing> PairEdges(const Lumps& lumps)
{
    constexpr float kSmoothingThreshold = 0.7071067f;
    constexpr uint32_t kHardEdge = 0x80000000u;
    const size_t count = lumps.faces.size();
    std::vector<std::vector<uint32_t>> vertexFaces(lumps.vertices.size());
    for (uint32_t f = 0; f < count; ++f) {
        for (int j = 0; j < lumps.faces[f].numEdges; ++j) {
            const int64_t v = EdgeVertex(lumps, lumps.faces[f], j);
            if (v >= 0 && (vertexFaces[v].empty() || vertexFaces[v].back() != f))
                vertexFaces[v].push_back(f);
        }
    }
    const auto normalOf = [&](const Face& face) { return face.planeNum < lumps.planes.size() ? lumps.planes[face.planeNum].normal : Float3{}; };
    std::vector<FaceSmoothing> result(count);
    for (uint32_t f = 0; f < count; ++f) {
        const Face& face = lumps.faces[f];
        FaceSmoothing& smooth = result[f];
        const Float3 normal = normalOf(face);
        const bool displaced = face.dispinfo != -1;
        smooth.vertexNormals.assign(std::max<int>(face.numEdges, 0), Float3{});
        Float3 centre = {};
        for (int j = 0; j < face.numEdges; ++j) {
            const int64_t v = EdgeVertex(lumps, face, j);
            if (v < 0)
                continue;
            centre = Add(centre, lumps.vertices[v]);
            for (uint32_t g : vertexFaces[v]) {
                if (g == f)
                    continue;
                const Face& other = lumps.faces[g];
                if (!displaced && other.dispinfo != -1)
                    continue;
                const Float3 otherNormal = normalOf(other);
                if (!displaced) {
                    if (face.smoothingGroups == 0 && other.smoothingGroups == 0) {
                        if (Dot(otherNormal, normal) < kSmoothingThreshold)
                            continue;
                    } else {
                        const uint32_t shared = face.smoothingGroups & other.smoothingGroups;
                        if ((shared & kHardEdge) != 0 || shared == 0)
                            continue;
                    }
                }
                smooth.vertexNormals[j] = Add(smooth.vertexNormals[j], otherNormal);
                if (std::find(smooth.neighbours.begin(), smooth.neighbours.end(), g) == smooth.neighbours.end())
                    smooth.neighbours.push_back(g);
            }
        }
        if (face.numEdges > 0)
            smooth.centre = Scale(centre, 1.0f / static_cast<float>(face.numEdges));
        for (Float3& vertexNormal : smooth.vertexNormals) {
            vertexNormal = Normalize(Add(vertexNormal, normal));
            smooth.flat = smooth.flat && Dot(normal, vertexNormal) >= 1.0f - 0.001f;
        }
    }
    return result;
}

// GetPhongNormal in src/utils/vrad/lightmap.cpp: on a face that is not flat, the normal at `spot` blends the face
// normal with the vertex normals of the edge whose wedge from the vertex centre holds it.
Float3 PhongNormal(const Lumps& lumps, const Face& face, const FaceSmoothing& smooth, Float3 normal, Float3 spot)
{
    if (smooth.flat)
        return normal;
    for (int j = 0; j < face.numEdges; ++j) {
        const int64_t vertex1 = EdgeVertex(lumps, face, j);
        const int64_t vertex2 = EdgeVertex(lumps, face, j + 1);
        if (vertex1 < 0 || vertex2 < 0)
            continue;
        const Float3 v1 = Sub(lumps.vertices[vertex1], smooth.centre);
        const Float3 v2 = Sub(lumps.vertices[vertex2], smooth.centre);
        const Float3 at = Sub(spot, smooth.centre);
        const float aa = Dot(v1, v1);
        const float bb = Dot(v2, v2);
        const float ab = Dot(v1, v2);
        const float a1 = (bb * Dot(v1, at) - ab * Dot(at, v2)) / (aa * bb - ab * ab);
        const float a2 = (Dot(at, v2) - a1 * ab) / bb;
        if (a1 >= 0.0f && a2 >= 0.0f) {
            const Float3& n1 = smooth.vertexNormals[j];
            const Float3& n2 = smooth.vertexNormals[(j + 1) % face.numEdges];
            return Normalize(Add(Add(Scale(normal, 1.0f - a1 - a2), Scale(n1, a1)), Scale(n2, a2)));
        }
    }
    return normal;
}

// Adds the style-0 lightmap of a world face to `lumps.baked` and returns its index, or kNoFace when the face has
// none. Flat faces (their `polygon`) also get their luxels to bake, sampled as VRAD sampled them (CalcFaceVectors,
// BuildFacesamples, LuxelSpaceToWorld and ComputeIlluminationPointAndNormalsSSE in src/utils/vrad/lightmap.cpp and
// radial.cpp), and a FaceSampling for WeightLuxels; displacements add theirs in AddDisplacement.
uint32_t AddFaceLightmap(const Lumps& lumps, size_t faceIndex, std::span<const Float3> polygon)
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
    const auto& w = texinfo.lightmapVecs;
    const Float3 w0 = {w[0][0], w[0][1], w[0][2]};
    const Float3 w1 = {w[1][0], w[1][1], w[1][2]};
    const Float3 cross = {w[1][1] * w[0][2] - w[1][2] * w[0][1], w[1][2] * w[0][0] - w[1][0] * w[0][2], w[1][0] * w[0][1] - w[1][1] * w[0][0]};
    const float det = -Dot(plane.normal, cross);
    const Float3& n = plane.normal;
    const bool mapped = std::fabs(det) >= 1e-20f;
    if (mapped) {
        lightmap.luxelS = {(n.z * w1.y - n.y * w1.z) / det, (n.x * w1.z - n.z * w1.x) / det, (n.y * w1.x - n.x * w1.y) / det};
        lightmap.luxelT = {(n.y * w0.z - n.z * w0.y) / det, (n.z * w0.x - n.x * w0.z) / det, (n.x * w0.y - n.y * w0.x) / det};
    }
    const uint32_t index = static_cast<uint32_t>(lumps.baked->faces.size());
    lumps.baked->faces.push_back(lightmap);
    if (polygon.empty() || !mapped)
        return index;

    FaceSampling sampling;
    sampling.face = static_cast<uint32_t>(faceIndex);
    sampling.lightmap = index;
    sampling.firstLuxel = lumps.baked->luxels.size();
    sampling.width = width;
    sampling.height = height;
    sampling.toWorldS = lightmap.luxelS;
    sampling.toWorldT = lightmap.luxelT;
    sampling.origin = Scale(cross, -plane.distance / det);
    sampling.origin = Add(sampling.origin, Scale(lightmap.luxelS, -w[0][3] + static_cast<float>(face.lightmapMins[0])));
    sampling.origin = Add(sampling.origin, Scale(lightmap.luxelT, -w[1][3] + static_cast<float>(face.lightmapMins[1])));
    std::vector<Float2> outline;
    for (const Float3& p : polygon)
        outline.push_back(FaceLuxel(lumps, face, p));
    const float partialBelow = 1.0f - 0.001f * std::sqrt(Dot(w0, w0)) * std::sqrt(Dot(w1, w1));
    sampling.cells.resize(luxels);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x)
            sampling.cells[y * width + x] = ClipLuxelCell(outline, x, y, partialBelow);
    }
    const FaceSmoothing& smooth = (*lumps.smoothing)[faceIndex];
    const Face& geometry = lumps.faces[faceIndex];
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const LuxelCell& own = sampling.cells[y * width + x];
            const Float3 position = own.sampled ? sampling.ToWorld(own.centroid) : sampling.ToWorld({static_cast<float>(x), static_cast<float>(y)});
            BakeLuxel luxel = {position, index, PhongNormal(lumps, geometry, smooth, plane.normal, position), y * width + x};
            luxel.subsamples = own.sampled ? own.subsamples | 0x80000000u : 0;
            lumps.baked->luxels.push_back(luxel);
        }
    }
    lumps.sampling->push_back(std::move(sampling));
    return index;
}

// The luxels' weights: VRAD's radial filter mixes into each luxel the samples of its face's four cells around it and
// those of the neighbouring faces whose bounds reach it (BuildLuxelRadial and AddDirectToRadial in
// src/utils/vrad/radial.cpp), each by RadialWeight, normalised over them all.
void WeightLuxels(const Lumps& lumps, std::span<const FaceSmoothing> smoothing, std::span<const FaceSampling> sampling)
{
    BakedLighting& baked = *lumps.baked;
    std::vector<int32_t> samplingOf(lumps.faces.size(), -1);
    for (size_t i = 0; i < sampling.size(); ++i)
        samplingOf[sampling[i].face] = static_cast<int32_t>(i);
    constexpr uint32_t kUnset = 0xFFFFFFFFu;
    for (BakeLuxel& luxel : baked.luxels)
        luxel.neighbourEnd = kUnset;
    std::vector<std::vector<BakeNeighbour>> reached;
    for (const FaceSampling& own : sampling) {
        const Face& face = lumps.lightFaces[own.face];
        const uint32_t width = own.width;
        const uint32_t height = own.height;
        reached.assign(static_cast<size_t>(width) * height, {});
        for (uint32_t g : smoothing[own.face].neighbours) {
            if (samplingOf[g] < 0)
                continue;
            const FaceSampling& other = sampling[samplingOf[g]];
            for (uint32_t c = 0; c < other.cells.size(); ++c) {
                const LuxelCell& cell = other.cells[c];
                if (!cell.sampled)
                    continue;
                const Float2 coord = FaceLuxel(lumps, face, other.ToWorld(cell.centroid));
                const Float2 mins = FaceLuxel(lumps, face, other.ToWorld(cell.mins));
                const Float2 maxs = FaceLuxel(lumps, face, other.ToWorld(cell.maxs));
                const int sMin = std::max(static_cast<int>(mins.u), 0);
                const int tMin = std::max(static_cast<int>(mins.v), 0);
                const int sMax = std::min(static_cast<int>(maxs.u + 0.9999f) + 1, static_cast<int>(width));
                const int tMax = std::min(static_cast<int>(maxs.v + 0.9999f) + 1, static_cast<int>(height));
                for (int ls = sMin; ls < sMax; ++ls) {
                    for (int lt = tMin; lt < tMax; ++lt) {
                        const float weight = RadialWeight(coord, mins, maxs, static_cast<float>(ls), static_cast<float>(lt));
                        if (weight > 0.0f)
                            reached[static_cast<size_t>(lt) * width + ls].push_back({other.lightmap, c, weight, 0});
                    }
                }
            }
        }
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                BakeLuxel& luxel = baked.luxels[own.firstLuxel + y * width + x];
                // The cells at (x - 1, y - 1), (x, y - 1), (x - 1, y) and (x, y): no other cell of the face reaches the luxel.
                std::array<float, 4> weights = {};
                float total = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    const int cx = static_cast<int>(x) - 1 + (k & 1);
                    const int cy = static_cast<int>(y) - 1 + (k >> 1);
                    if (cx < 0 || cy < 0)
                        continue;
                    const LuxelCell& cell = own.cells[cy * width + cx];
                    if (cell.sampled)
                        weights[k] = RadialWeight(cell.centroid, cell.mins, cell.maxs, static_cast<float>(x), static_cast<float>(y));
                    total += weights[k];
                }
                std::vector<BakeNeighbour>& others = reached[y * width + x];
                for (const BakeNeighbour& other : others)
                    total += other.weight;
                for (int k = 0; k < 4; ++k)
                    luxel.weights[k] = total > 0.0f ? static_cast<uint16_t>(std::lround(weights[k] / total * 65535.0f)) : 0;
                for (BakeNeighbour& other : others) {
                    other.weight /= total;
                    baked.neighbours.push_back(other);
                }
                luxel.neighbourEnd = static_cast<uint32_t>(baked.neighbours.size());
            }
        }
    }
    uint32_t end = 0;
    for (BakeLuxel& luxel : baked.luxels) {
        if (luxel.neighbourEnd == kUnset)
            luxel.neighbourEnd = end;
        end = luxel.neighbourEnd;
    }
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
                lumps.baked->luxels.push_back({bilinear(at), lightmap, Normalize(bilinear(normalAt)), y * entry.width + x, {0, 0, 0, 0xFFFF}});
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
            // StaticPropLump_t in the Source SDK's public/gamebspfile.h: m_Skin follows m_Solid and the flags byte.
            staticProp.skin = ReadAt<int32_t>(data, prop + 32).value_or(0);
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

// The origins and _fifty_percent_distance of the light and light_spot entities that have one, the only entities VRAD
// gives that falloff (ParseLightPoint, ParseLightSpot and SetLightFalloffParams in src/utils/vrad/lightmap.cpp).
std::vector<std::pair<Float3, float>> ReadFiftyPercentDistances(std::span<const std::byte> file, const Header& header)
{
    std::vector<std::pair<Float3, float>> result;
    const Lump& lump = header.lumps[kLumpEntities];
    if (lump.offset < 0 || lump.length <= 0 || static_cast<size_t>(lump.offset) + lump.length > file.size())
        return result;
    const std::string_view entities(reinterpret_cast<const char*>(file.data() + lump.offset), lump.length);
    for (size_t start = entities.find('{'); start != std::string_view::npos; start = entities.find('{', start + 1)) {
        const size_t end = entities.find('}', start);
        if (end == std::string_view::npos)
            break;
        std::string_view classname;
        Float3 origin = {};
        bool hasOrigin = false;
        float distance = 0.0f;
        // "key" "value" pairs.
        for (size_t at = start + 1;;) {
            const size_t keyStart = entities.find('"', at);
            const size_t keyEnd = keyStart < end ? entities.find('"', keyStart + 1) : std::string_view::npos;
            const size_t valueStart = keyEnd < end ? entities.find('"', keyEnd + 1) : std::string_view::npos;
            const size_t valueEnd = valueStart < end ? entities.find('"', valueStart + 1) : std::string_view::npos;
            if (valueEnd >= end)
                break;
            const std::string_view key = entities.substr(keyStart + 1, keyEnd - keyStart - 1);
            const std::string value(entities.substr(valueStart + 1, valueEnd - valueStart - 1));
            if (key == "classname")
                classname = entities.substr(valueStart + 1, valueEnd - valueStart - 1);
            else if (key == "origin")
                hasOrigin = sscanf_s(value.c_str(), "%f %f %f", &origin.x, &origin.y, &origin.z) == 3;
            else if (key == "_fifty_percent_distance")
                sscanf_s(value.c_str(), "%f", &distance);
            at = valueEnd + 1;
        }
        if ((classname == "light" || classname == "light_spot") && hasOrigin && distance != 0.0f)
            result.emplace_back(origin, distance);
        start = end;
    }
    return result;
}

void ReadLights(std::span<const std::byte> file, const Header& header, WorldGeometry& world)
{
    const std::vector<std::pair<Float3, float>> fiftyPercent = ReadFiftyPercentDistances(file, header);
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
                // VRAD gives a light its entity's origin (AllocDLight).
                for (const auto& [origin, distance] : fiftyPercent) {
                    if (light.type != static_cast<int32_t>(LightType::Surface) && std::fabs(origin.x - light.origin.x) < 0.01f &&
                        std::fabs(origin.y - light.origin.y) < 0.01f && std::fabs(origin.z - light.origin.z) < 0.01f)
                        world.lights.back().referenceDistance = distance;
                }
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

// A side's polygon: a large square on its plane cut down by the brush's other, non-bevel planes (BaseWindingForPlane
// and ChopWindingInPlace with the flipped plane, as AddBrushToRaytraceEnvironment does).
std::vector<std::array<double, 3>> BrushSideWinding(const Plane& plane, std::span<const Plane> cutters)
{
    const std::array<double, 3> n = {plane.normal.x, plane.normal.y, plane.normal.z};
    const int axis = std::fabs(n[2]) >= std::fabs(n[0]) && std::fabs(n[2]) >= std::fabs(n[1]) ? 2 : std::fabs(n[0]) >= std::fabs(n[1]) ? 0 : 1;
    std::array<double, 3> up = axis == 2 ? std::array<double, 3>{1.0, 0.0, 0.0} : std::array<double, 3>{0.0, 0.0, 1.0};
    const double along = up[0] * n[0] + up[1] * n[1] + up[2] * n[2];
    double length = 0.0;
    for (int i = 0; i < 3; ++i) {
        up[i] -= n[i] * along;
        length += up[i] * up[i];
    }
    length = std::sqrt(length);
    for (double& v : up)
        v /= length;
    const std::array<double, 3> right = {up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0]};
    constexpr double kSize = 131072.0;
    std::vector<std::array<double, 3>> winding;
    for (const auto [r, u] : {std::pair(-1.0, 1.0), std::pair(1.0, 1.0), std::pair(1.0, -1.0), std::pair(-1.0, -1.0)}) {
        std::array<double, 3> point;
        for (int i = 0; i < 3; ++i)
            point[i] = n[i] * plane.distance + (right[i] * r + up[i] * u) * kSize;
        winding.push_back(point);
    }
    for (const Plane& cutter : cutters) {
        // Keep the brush's inside of the cutter: n.p <= d.
        const auto side = [&](const std::array<double, 3>& p) {
            return p[0] * cutter.normal.x + p[1] * cutter.normal.y + p[2] * cutter.normal.z - cutter.distance;
        };
        std::vector<std::array<double, 3>> kept;
        for (size_t i = 0; i < winding.size(); ++i) {
            const auto& a = winding[i];
            const auto& b = winding[(i + 1) % winding.size()];
            const double da = side(a);
            const double db = side(b);
            if (da <= 0.0)
                kept.push_back(a);
            if ((da < 0.0 && db > 0.0) || (da > 0.0 && db < 0.0)) {
                const double t = da / (da - db);
                kept.push_back({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t});
            }
        }
        winding = std::move(kept);
        if (winding.size() < 3)
            return {};
    }
    return winding;
}

void ReadBrushOccluders(std::span<const std::byte> file, const Header& header, WorldGeometry& world)
{
    auto planes = LumpArray<Plane>(file, header, kLumpPlanes);
    auto nodes = LumpArray<Node>(file, header, kLumpNodes);
    auto leafBrushes = LumpArray<uint16_t>(file, header, kLumpLeafBrushes);
    auto brushes = LumpArray<Brush>(file, header, kLumpBrushes);
    auto sides = LumpArray<BrushSide>(file, header, kLumpBrushSides);
    auto texinfos = LumpArray<Texinfo>(file, header, kLumpTexinfo);
    const Lump& leafLump = header.lumps[kLumpLeafs];
    const size_t leafStride = leafLump.version == 0 ? kLeafV0Size : kLeafV1Size;
    if (!planes || !nodes || nodes->empty() || !leafBrushes || !brushes || !sides || !texinfos || leafLump.length <= 0 ||
        static_cast<size_t>(leafLump.offset) + leafLump.length > file.size())
        return;
    const size_t leafCount = static_cast<size_t>(leafLump.length) / leafStride;
    // The world model's brushes, from the leaves under its head node (GetBrushes_r).
    std::vector<bool> taken(brushes->size(), false);
    std::vector<uint32_t> worldBrushes;
    std::vector<int32_t> stack = {0};
    while (!stack.empty()) {
        const int32_t node = stack.back();
        stack.pop_back();
        if (node >= 0) {
            if (static_cast<size_t>(node) >= nodes->size())
                continue;
            stack.push_back((*nodes)[node].children[0]);
            stack.push_back((*nodes)[node].children[1]);
            continue;
        }
        const size_t leafIndex = static_cast<size_t>(-node - 1);
        if (leafIndex >= leafCount)
            continue;
        Leaf leaf;
        std::memcpy(&leaf, file.data() + leafLump.offset + leafIndex * leafStride, sizeof(leaf));
        for (uint32_t i = 0; i < leaf.numLeafBrushes; ++i) {
            const size_t at = static_cast<size_t>(leaf.firstLeafBrush) + i;
            if (at >= leafBrushes->size() || (*leafBrushes)[at] >= brushes->size() || taken[(*leafBrushes)[at]])
                continue;
            taken[(*leafBrushes)[at]] = true;
            worldBrushes.push_back((*leafBrushes)[at]);
        }
    }
    std::vector<Plane> cutters;
    for (uint32_t b : worldBrushes) {
        const Brush& brush = (*brushes)[b];
        if ((brush.contents & kMaskOpaque) == 0 || brush.firstSide < 0 || brush.sideCount <= 0 ||
            static_cast<size_t>(brush.firstSide) + brush.sideCount > sides->size())
            continue;
        for (int32_t i = 0; i < brush.sideCount; ++i) {
            const BrushSide& side = (*sides)[brush.firstSide + i];
            if (side.planeNum >= planes->size() || side.dispInfo != 0)
                continue;
            if (side.texinfo >= 0 && static_cast<size_t>(side.texinfo) < texinfos->size() && ((*texinfos)[side.texinfo].flags & kSurfSky) != 0)
                continue;
            cutters.clear();
            for (int32_t j = 0; j < brush.sideCount; ++j) {
                const BrushSide& other = (*sides)[brush.firstSide + j];
                if (j != i && !other.bevel && other.planeNum < planes->size())
                    cutters.push_back((*planes)[other.planeNum]);
            }
            const std::vector<std::array<double, 3>> winding = BrushSideWinding((*planes)[side.planeNum], cutters);
            const auto point = [&](size_t k) {
                return Float3{static_cast<float>(winding[k][0]), static_cast<float>(winding[k][1]), static_cast<float>(winding[k][2])};
            };
            for (size_t k = 2; k < winding.size(); ++k)
                world.brushOccluders.insert(world.brushOccluders.end(), {point(0), point(k - 1), point(k)});
        }
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
    const std::vector<FaceSmoothing> smoothing = PairEdges(lumps);
    std::vector<FaceSampling> sampling;
    lumps.smoothing = &smoothing;
    lumps.sampling = &sampling;
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
        const bool sky = face.texinfo >= 0 && static_cast<size_t>(face.texinfo) < lumps.texinfos.size() &&
            (lumps.texinfos[face.texinfo].flags & kSurfSky) != 0;
        if (sky && FacePolygon(lumps, face, polygon)) {
            for (size_t k = 2; k < polygon.size(); ++k)
                world.skyOccluders.insert(world.skyOccluders.end(), {polygon[0], polygon[k - 1], polygon[k]});
        }
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
            const uint32_t lightmap = AddFaceLightmap(lumps, static_cast<size_t>(f), {});
            if (AddDisplacement(world.mesh, lumps, face, disp, polygon, lightmap)) {
                ++world.displacementCount;
            } else if (lightmap != kNoFace) {
                world.baked.faces.pop_back();
                world.baked.luxels.resize(luxels);
            }
            continue;
        }
        AddPolygon(world.mesh, lumps, face, polygon, AddFaceLightmap(lumps, static_cast<size_t>(f), polygon));
        ++world.faceCount;
    }
    if (!world.baked.luxels.empty())
        WeightLuxels(lumps, smoothing, sampling);

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
    world.lightMaterials.assign(world.materials.size(), false);
    for (const Texinfo& texinfo : lumps.texinfos) {
        if ((texinfo.flags & kSurfLight) != 0 && texinfo.texdata >= 0 && static_cast<size_t>(texinfo.texdata) < world.lightMaterials.size())
            world.lightMaterials[texinfo.texdata] = true;
    }
    ReadLights(file, header, world);
    ReadVisibility(file, header, world.sun, world.visibility);
    ReadLeafAmbient(file, header, world.visibility, world.baked);
    for (const Light& light : world.lights) {
        if (light.inAmbientCube)
            world.baked.ambientLights.push_back({{light.origin.x, light.origin.y, light.origin.z, light.radius},
                {light.intensity.x, light.intensity.y, light.intensity.z, 0.0f}, {light.normal.x, light.normal.y, light.normal.z, 0.0f}});
    }
    ReadBrushOccluders(file, header, world);
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
