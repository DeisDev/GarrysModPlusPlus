#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gmodpp::bsp {

struct Float3 {
    float x, y, z;
};

struct Float2 {
    float u, v;
};

inline constexpr uint32_t kNoMaterial = 0xFFFFFFFF;
inline constexpr uint32_t kNoFace = 0xFFFFFFFF;

struct SunLight {
    bool present = false;
    // The light_environment's position, whose cluster sees the sky light (BuildVisForLightEnvironment in VRAD).
    Float3 origin = {};
    Float3 direction = {};
    Float3 color = {};
    Float3 ambient = {};
    // Light styles of the sun and the sky light (Light::style).
    int32_t style = 0;
    int32_t ambientStyle = 0;
    // The 3D skybox's camera: VRAD continues sun rays that reach the sky into the skybox, whose scene shadows the map
    // (TestLine_DoesHitSky in src/utils/vrad/trace.cpp). Scale 0 without one.
    Float3 skyCameraOrigin = {};
    float skyCameraScale = 0.0f;
};

enum class LightType : int32_t { Surface = 0, Point = 1, Spot = 2, Sky = 3, QuakeLight = 4, SkyAmbient = 5 };

// A baked light from the worldlights lump, in VRAD's units and falloff model.
struct Light {
    LightType type;
    Float3 origin;
    Float3 intensity;
    Float3 normal;
    float stopdot, stopdot2, exponent, radius;
    float constantAttn, linearAttn, quadraticAttn;
    // Visibility cluster holding the light, or -1.
    int32_t cluster;
    // Light style: 0 for plain lights, 1-31 for animated patterns, 32 and up for switchable (named) lights.
    int32_t style;
    // VRAD added the light's direct light to the leaf ambient samples (DWL_FLAGS_INAMBIENTCUBE: dim surface lights).
    bool inAmbientCube;
    // Where the mapper's brightness holds: 100 units, where VRAD scales point and spot lights' intensity to it, or a
    // light entity's _fifty_percent_distance, whose falloff VRAD solves for instead (SetLightFalloffParams in
    // src/utils/vrad/lightmap.cpp).
    float referenceDistance = 100.0f;
};

// A world BSP node; a negative child is leaf -child - 1.
struct BspNode {
    Float3 normal;
    float distance;
    int32_t children[2];
};

struct BspLeaf {
    // -1 for solid leaves.
    int32_t cluster;
    int32_t area;
    Float3 mins, maxs;
};

// The world's BSP tree and potentially visible sets. VRAD only lights a sample from lights whose cluster sees
// the sample's cluster. Everything is empty when the map has no visibility data.
struct Visibility {
    uint32_t clusterCount = 0;
    // One row of (clusterCount + 7) / 8 bytes per cluster.
    std::vector<uint8_t> pvs;
    std::vector<BspNode> nodes;
    std::vector<BspLeaf> leaves;
    // The clusters VRAD let the sun and sky light reach (one PVS row): those the light_environment's cluster or a
    // cluster holding a sky face sees (BuildVisForLightEnvironment and PVSCheck in src/utils/vrad/lightmap.cpp).
    // Empty for every cluster.
    std::vector<uint8_t> sunClusters;

    bool Sees(int32_t from, int32_t to) const
    {
        if (from < 0 || to < 0 || pvs.empty())
            return true;
        const size_t row = (clusterCount + 7) / 8;
        return (pvs[from * row + (to >> 3)] & (1u << (to & 7))) != 0;
    }

    // The index in `leaves` of the leaf holding a point, or -1 without visibility data.
    int32_t LeafAt(Float3 point) const;
};

struct StaticProp {
    uint32_t model;
    float transform[3][4];
    // Hammer's "Disable shadows" (STATIC_PROP_NO_SHADOW): VRAD let all light through the prop.
    bool noShadow = false;
    // STATIC_PROP_IGNORE_NORMALS: VRAD lit the prop the same from every direction, as foliage usually is.
    bool ignoreNormals = false;
    // The model's skin family (m_Skin).
    int32_t skin = 0;
};

struct Mesh {
    std::vector<Float3> positions;
    // Texture coordinates of each vertex.
    std::vector<Float2> uvs;
    std::vector<uint32_t> indices;
    // Linear reflectivity of each triangle's texture as packed RGBA8.
    std::vector<uint32_t> triangleAlbedo;
    // Each triangle's index into WorldGeometry::materials, or kNoMaterial.
    std::vector<uint32_t> triangleMaterials;
    // Each triangle's index into BakedLighting::faces, or kNoFace, and each vertex's luxel coordinates in that
    // face's lightmap.
    std::vector<uint32_t> triangleFaces;
    std::vector<Float2> luxels;
};

// The style-0 lightmap of a world face as VRAD stored it (src/utils/vrad/lightmap.cpp, ExportLightmaps): `width` x
// `height` luxels from `sample` on, then, when `bumped`, three more of the same size for the bump basis
// (GetBumpNormals: tangent space `tangentS`, `tangentT`, `normal`). Mirrors FaceLightmap in shaders/baked.hlsli.
struct FaceLightmap {
    uint32_t sample;
    uint32_t width;
    uint32_t height;
    uint32_t bumped;
    Float3 tangentS;
    float unused0;
    Float3 tangentT;
    float unused1;
    Float3 normal;
    float unused2;
    // World units per luxel along s and t (LuxelSpaceToWorld).
    Float3 luxelS;
    float unused3;
    Float3 luxelT;
    float unused4;
};
static_assert(sizeof(FaceLightmap) == 96);

// A style-0 luxel of a world face, `luxel` = s + t * width in the face's lightmap, with the point and normal where
// VRAD sampled the cell from it to the next luxel (its luxel point when VRAD had none there). `weights` (unorm16) mix
// the samples of the cells at (s - 1, t - 1), (s, t - 1), (s - 1, t) and (s, t) into the luxel, and
// BakedLighting::neighbours up to `neighbourEnd` (from the previous luxel's) add those of neighbouring faces; all
// zero when no sample reaches the luxel. `subsamples`: bit 31 when the cell has a sample, bits 0-15 the points VRAD
// would supersample it at (see LuxelCell in src/bsp.cpp). Mirrors BakeLuxel in shaders/baked.hlsli.
struct BakeLuxel {
    Float3 position;
    uint32_t face;
    Float3 normal;
    uint32_t luxel;
    std::array<uint16_t, 4> weights = {0, 0, 0, 0xFFFF};
    uint32_t subsamples = 0;
    uint32_t neighbourEnd = 0;
};
static_assert(sizeof(BakeLuxel) == 48);

// A sample of a neighbouring face that VRAD's radial filter added to a luxel (BuildLuxelRadial in
// src/utils/vrad/radial.cpp): cell `cell` of the FaceLightmap `lightmap`, with its share of the luxel. Mirrors
// BakeNeighbour in shaders/baked.hlsli.
struct BakeNeighbour {
    uint32_t lightmap;
    uint32_t cell;
    float weight;
    uint32_t unused;
};
static_assert(sizeof(BakeNeighbour) == 16);

// A leaf ambient sample (dleafambientlighting_t): the light arriving from each axis direction (+x, -x, +y, -y,
// +z, -z) at a point of a leaf. Mirrors AmbientSample in shaders/baked.hlsli.
struct AmbientSample {
    float position[4];
    float cube[6][4];
};
static_assert(sizeof(AmbientSample) == 112);

// VRAD's baked light: the lightmaps of style 0 (direct light from style-0 lights, sky ambient and bounced light,
// summed) and the leaf ambient samples, HDR when the map has them.
// A light whose direct light VRAD added to the leaf ambient samples (Light::inAmbientCube), as
// shaders/baked_residual.hlsl reads it: origin and radius, intensity, the surface's normal.
struct AmbientLight {
    float origin[4];
    float intensity[4];
    float normal[4];
};
static_assert(sizeof(AmbientLight) == 48);

struct BakedLighting {
    // ColorRGBExp32 samples (r, g, b, exponent): c * 2^exponent / 255.
    std::vector<uint32_t> samples;
    bool hdr = false;
    std::vector<FaceLightmap> faces;
    std::vector<BakeLuxel> luxels;
    std::vector<BakeNeighbour> neighbours;
    // Per BSP leaf (Visibility::leaves): its first sample in `ambientSamples` and their number.
    std::vector<std::array<uint32_t, 2>> leafAmbient;
    std::vector<AmbientSample> ambientSamples;
    std::vector<AmbientLight> ambientLights;
};

struct WorldGeometry {
    Mesh mesh;
    // Brush entity models ("*1", "*2", ...: doors, func_brush) indexed by model number, in their entity's space.
    // Entry 0 is the world, which is `mesh`, and stays empty.
    std::vector<Mesh> brushModels;
    // Material name of each texdata entry, as in "materials/<name>.vmt".
    std::vector<std::string> materials;
    // The texdata entries VRAD made emit_surface lights of (a texinfo with SURF_LIGHT uses them).
    std::vector<bool> lightMaterials;
    SunLight sun;
    // Point, spot and surface lights; the sky lights are in `sun`.
    std::vector<Light> lights;
    Visibility visibility;
    uint32_t faceCount = 0;
    uint32_t displacementCount = 0;
    uint32_t skyboxFaceCount = 0;
    std::vector<std::string> propModels;
    std::vector<StaticProp> props;
    // The 3D skybox's own faces and static props: a scaled-down scene elsewhere in the map, kept apart so that
    // only rays of the skybox view meet it.
    Mesh skybox;
    std::vector<StaticProp> skyboxProps;
    // VRAD's brush occluders as triangle lists, for the baked light only: every side of every opaque brush of the
    // world model but sky and displacement sides, whatever its texture, 3D skybox included; and the world model's sky
    // faces, where VRAD's sun rays end unblocked (AddBrushesForRayTrace in src/utils/vrad/trace.cpp).
    std::vector<Float3> brushOccluders;
    std::vector<Float3> skyOccluders;
    BakedLighting baked;
    // The sky camera's area (BspLeaf::area), or -1, and the bounds of its leaves.
    int32_t skyArea = -1;
    Float3 skyMins = {};
    Float3 skyMaxs = {};

    // Whether a point belongs to the 3D skybox: in its area, or inside its bounds (points on its faces can fall
    // into solid leaves of no area).
    bool InSkybox(Float3 point) const;
};

bool InBounds(Float3 point, Float3 mins, Float3 maxs);

// Returns false with a reason in `error` when the data is not a supported Source BSP.
bool LoadWorld(std::span<const std::byte> file, WorldGeometry& world, std::string& error);

}
