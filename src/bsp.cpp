#include "bsp.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>

namespace gmodpp::bsp {

namespace {

constexpr int32_t kIdent = 'V' | ('B' << 8) | ('S' << 16) | ('P' << 24);
constexpr int kLumpCount = 64;
constexpr int kLumpEntities = 0;
constexpr int kLumpPlanes = 1;
constexpr int kLumpVertexes = 3;
constexpr int kLumpNodes = 5;
constexpr int kLumpLeafs = 10;
constexpr int kLumpTexinfo = 6;
constexpr int kLumpFaces = 7;
constexpr int kLumpEdges = 12;
constexpr int kLumpSurfedges = 13;
constexpr int kLumpModels = 14;
constexpr int kLumpWorldLights = 15;
constexpr int kLumpDispInfo = 26;
constexpr int kLumpGameLump = 35;
constexpr int kLumpDispVerts = 33;
constexpr int kLumpWorldLightsHdr = 54;

constexpr int32_t kSurfSky2D = 0x0002;
constexpr int32_t kSurfSky = 0x0004;
constexpr int32_t kSurfWarp = 0x0008;
constexpr int32_t kSurfTrans = 0x0010;
constexpr int32_t kSurfTrigger = 0x0040;
constexpr int32_t kSurfNoDraw = 0x0080;
constexpr int32_t kSurfHint = 0x0100;
constexpr int32_t kSurfSkip = 0x0200;
constexpr int32_t kSkippedSurfaces = kSurfSky2D | kSurfSky | kSurfWarp | kSurfTrans | kSurfTrigger | kSurfNoDraw | kSurfHint | kSurfSkip;

constexpr int32_t kStaticPropLumpId = ('s' << 24) | ('p' << 16) | ('r' << 8) | 'p';
constexpr size_t kStaticPropNameLength = 128;
constexpr uint32_t kStaticPropNoShadow = 0x10;
constexpr float kDegreesToRadians = 3.14159265358979f / 180.0f;

constexpr int kEmitSkyLight = 3;
constexpr int kEmitSkyAmbient = 5;

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
#pragma pack(pop)

static_assert(sizeof(Header) == 1036);
static_assert(sizeof(Plane) == 20);
static_assert(sizeof(Node) == 32);
static_assert(sizeof(Leaf) == 30);
static_assert(sizeof(Face) == 56);
static_assert(sizeof(Texinfo) == 72);
static_assert(sizeof(Model) == 48);
static_assert(sizeof(DispVert) == 20);
static_assert(sizeof(DispInfo) == 176);
static_assert(sizeof(WorldLight) == 88);

constexpr size_t kWorldLightV1Size = 100;
constexpr size_t kLeafV0Size = 56;
constexpr size_t kLeafV1Size = 32;

template <class T>
std::optional<std::span<const T>> LumpArray(std::span<const std::byte> file, const Header& header, int index, size_t stride = sizeof(T))
{
    const Lump& lump = header.lumps[index];
    if (lump.offset < 0 || lump.length < 0 || static_cast<size_t>(lump.offset) + lump.length > file.size() || lump.length % stride != 0)
        return std::nullopt;
    if (lump.fourCC != 0)
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
    std::span<const DispInfo> dispinfos;
    std::span<const DispVert> dispVerts;
};

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

void AddPolygon(WorldGeometry& world, const std::vector<Float3>& polygon)
{
    uint32_t base = static_cast<uint32_t>(world.positions.size());
    world.positions.insert(world.positions.end(), polygon.begin(), polygon.end());
    for (uint32_t i = 1; i + 1 < polygon.size(); ++i) {
        world.indices.push_back(base);
        world.indices.push_back(base + i);
        world.indices.push_back(base + i + 1);
    }
}

bool AddDisplacement(WorldGeometry& world, const Lumps& lumps, const DispInfo& disp, const std::vector<Float3>& corners)
{
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
    uint32_t base = static_cast<uint32_t>(world.positions.size());
    for (int i = 0; i < post; ++i) {
        Float3 end0 = Add(p[0], Scale(edge0, static_cast<float>(i)));
        Float3 end1 = Add(p[3], Scale(edge1, static_cast<float>(i)));
        Float3 segment = Scale(Sub(end1, end0), step);
        for (int j = 0; j < post; ++j) {
            const DispVert& dv = lumps.dispVerts[disp.dispVertStart + i * post + j];
            Float3 flat = Add(end0, Scale(segment, static_cast<float>(j)));
            world.positions.push_back(Add(flat, Scale(dv.vec, dv.dist)));
        }
    }
    for (int i = 0; i + 1 < post; ++i) {
        for (int j = 0; j + 1 < post; ++j) {
            uint32_t a = base + i * post + j;
            uint32_t b = a + 1;
            uint32_t c = a + post;
            uint32_t d = c + 1;
            world.indices.insert(world.indices.end(), {a, b, d, a, d, c});
        }
    }
    return true;
}

std::optional<Float3> SkyCameraOrigin(std::span<const std::byte> file, const Header& header)
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
    Float3 origin;
    if (sscanf_s(block.c_str() + originPos + 8, " \"%f %f %f\"", &origin.x, &origin.y, &origin.z) != 3)
        return std::nullopt;
    return origin;
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
};

std::optional<AreaLocator> MakeAreaLocator(std::span<const std::byte> file, const Header& header)
{
    auto planes = LumpArray<Plane>(file, header, kLumpPlanes);
    auto nodes = LumpArray<Node>(file, header, kLumpNodes);
    const Lump& leafLump = header.lumps[kLumpLeafs];
    const size_t leafStride = leafLump.version == 0 ? kLeafV0Size : kLeafV1Size;
    if (!planes || !nodes || nodes->empty() || leafLump.fourCC != 0 || leafLump.length <= 0 ||
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
        if ((*flags & 1) != 0 || *offset < 0 || *length <= 0 || static_cast<size_t>(*offset) + *length > file.size())
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
            if (propFlags && (*propFlags & kStaticPropNoShadow) != 0)
                continue;
            StaticProp staticProp;
            staticProp.model = *model;
            PropTransform(*origin, *angles, staticProp.transform);
            world.props.push_back(staticProp);
        }
        return;
    }
}

void ReadSun(std::span<const std::byte> file, const Header& header, SunLight& sun)
{
    for (int lumpIndex : {kLumpWorldLightsHdr, kLumpWorldLights}) {
        const Lump& lump = header.lumps[lumpIndex];
        size_t stride = lump.version == 1 ? kWorldLightV1Size : sizeof(WorldLight);
        if (lump.length <= 0 || lump.fourCC != 0 || static_cast<size_t>(lump.offset) + lump.length > file.size() || lump.length % stride != 0)
            continue;
        for (size_t offset = 0; offset < static_cast<size_t>(lump.length); offset += stride) {
            const std::byte* entry = file.data() + lump.offset + offset;
            WorldLight light;
            std::memcpy(&light, entry, 36);
            const size_t tail = stride == kWorldLightV1Size ? 48 : 36;
            std::memcpy(&light.cluster, entry + tail, sizeof(WorldLight) - 36);
            if (light.type == kEmitSkyLight) {
                sun.present = true;
                sun.direction = Scale(light.normal, -1.0f);
                sun.color = light.intensity;
            } else if (light.type == kEmitSkyAmbient) {
                sun.ambient = light.intensity;
            }
        }
        if (sun.present)
            return;
    }
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

    auto vertices = LumpArray<Float3>(file, header, kLumpVertexes);
    auto surfedges = LumpArray<int32_t>(file, header, kLumpSurfedges);
    auto edges = LumpArray<uint16_t>(file, header, kLumpEdges, sizeof(uint16_t) * 2);
    auto faces = LumpArray<Face>(file, header, kLumpFaces);
    auto texinfos = LumpArray<Texinfo>(file, header, kLumpTexinfo);
    auto models = LumpArray<Model>(file, header, kLumpModels);
    auto dispinfos = LumpArray<DispInfo>(file, header, kLumpDispInfo);
    auto dispVerts = LumpArray<DispVert>(file, header, kLumpDispVerts);
    if (!vertices || !surfedges || !edges || !faces || !texinfos || !models || models->empty() || !dispinfos || !dispVerts) {
        error = "a required lump is missing, compressed or malformed";
        return false;
    }

    Lumps lumps;
    lumps.vertices = *vertices;
    lumps.surfedges = *surfedges;
    lumps.edges = std::span<const uint16_t>(reinterpret_cast<const uint16_t*>(edges->data()), edges->size() * 2);
    lumps.faces = *faces;
    lumps.texinfos = *texinfos;
    lumps.dispinfos = *dispinfos;
    lumps.dispVerts = *dispVerts;

    world = {};
    // The scaled-down 3D skybox scene is world geometry placed outside the playable space; it must not
    // occlude sun rays, so faces in the sky camera's area are skipped.
    const std::optional<AreaLocator> areas = MakeAreaLocator(file, header);
    const std::optional<Float3> skyCamera = SkyCameraOrigin(file, header);
    const int skyArea = areas && skyCamera ? areas->AreaAt(*skyCamera) : -1;
    auto planes = LumpArray<Plane>(file, header, kLumpPlanes);
    const Model& worldModel = (*models)[0];
    std::vector<Float3> polygon;
    for (int32_t f = worldModel.firstFace; f < worldModel.firstFace + worldModel.numFaces; ++f) {
        if (f < 0 || static_cast<size_t>(f) >= lumps.faces.size())
            break;
        const Face& face = lumps.faces[f];
        if (face.texinfo >= 0 && static_cast<size_t>(face.texinfo) < lumps.texinfos.size() &&
            (lumps.texinfos[face.texinfo].flags & kSkippedSurfaces) != 0)
            continue;
        if (!FacePolygon(lumps, face, polygon))
            continue;
        if (skyArea >= 0 && planes && face.planeNum < planes->size()) {
            Float3 center = {};
            for (const Float3& p : polygon)
                center = Add(center, p);
            center = Scale(center, 1.0f / static_cast<float>(polygon.size()));
            Float3 normal = (*planes)[face.planeNum].normal;
            if (face.side != 0)
                normal = Scale(normal, -1.0f);
            if (areas->AreaAt(Add(center, normal)) == skyArea) {
                ++world.skyboxFaceCount;
                continue;
            }
        }
        if (face.dispinfo >= 0 && static_cast<size_t>(face.dispinfo) < lumps.dispinfos.size()) {
            if (AddDisplacement(world, lumps, lumps.dispinfos[face.dispinfo], polygon))
                ++world.displacementCount;
            continue;
        }
        AddPolygon(world, polygon);
        ++world.faceCount;
    }

    ReadSun(file, header, world.sun);
    ReadStaticProps(file, header, world);
    if (world.indices.empty()) {
        error = "the world model has no drawable faces";
        return false;
    }
    return true;
}

}
