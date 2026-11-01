#pragma once

#include "bsp.h"
#include "descriptor_heap.h"
#include "engine_interfaces.h"
#include "opacity_micromaps.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace gmodpp {

// Instance masks of the scene's acceleration structure.
constexpr UINT kWorldMask = 0x01;
constexpr UINT kStaticPropMask = 0x02;
constexpr UINT kDynamicMask = 0x04;
// The local player's own body and what it holds sit around the camera; viewmodel and indirect rays leave them out (see
// Lighting).
constexpr UINT kLocalPlayerMask = 0x08;
// The 3D skybox's scaled-down scene, which only the skybox view's rays meet.
constexpr UINT kSkyboxMask = 0x10;
// Static props the map marks as casting no shadows (often foliage). VRAD let all light through them; they block
// light by FoliageShadows (src/settings.h).
constexpr UINT kShadowlessPropMask = 0x20;
// Water surfaces (Scene::SetWater), which only rays that look for water meet: the water's own rays and reflections.
constexpr UINT kWaterMask = 0x40;
// Static props that enclose a map light (SealedLights in src/world.cpp). The lights they seal shine through them, as
// through a diffuser; to every other ray they are static props.
constexpr UINT kLampShellMask = 0x80;

// SceneInstance::flags, read by shaders from the instance record.
constexpr uint32_t kInstanceIgnoreNormals = 0x01;
// Set by Scene for models with a self-illuminated material, whose hits add the light it gives off.
constexpr uint32_t kInstanceSelfIllum = 0x02;
// Set by Scene for dynamic models (kDynamicMask, kLocalPlayerMask), whose back faces shadow rays may skip (their
// instances allow culling; shaders/direct.hlsl).
constexpr uint32_t kInstanceModel = 0x04;
// Instances whose self-illuminated triangles are emitters (Scene::LoadMap): with EmissiveLighting::Full their
// glow reaches surfaces directly, so indirect rays from the G-buffer take none at them.
constexpr uint32_t kInstanceEmitter = 0x10;
// Set by Scene for instances of character models (Scene::MarkCharacter), whose $halflambert materials are skin.
constexpr uint32_t kInstanceCharacter = 0x20;
// The water surfaces' instance (kWaterMask).
constexpr uint32_t kInstanceWater = 0x40;

// A body of water for shaders (WaterBody in shaders/water_body.hlsli): the medium its water is, its surface's height,
// tints and normal map (frames from `framesFirst` in Scene::Water's frame list, animated at `frameRate`; bumpS and bumpT
// map (u, v, seconds, 1) to its coordinates), and its bounds.
struct GpuWaterBody {
    float extinction[3];
    float surfaceZ;
    float fogColor[3];
    float reflectTint;
    float refractTint[3];
    uint32_t framesFirst;
    float textureS[4];
    float textureT[4];
    float bumpS[4];
    float bumpT[4];
    float mins[3];
    uint32_t frameCount;
    float maxs[3];
    float frameRate;
};
static_assert(sizeof(GpuWaterBody) == 144);

// The map's water on the GPU (Scene::SetWater): raw views of the GpuWaterBody list, each BSP leaf's body (-1 for none),
// each water surface triangle's body and the normal maps' frame descriptors; `top` is the highest surface. Bodies from
// `brushFirst` on are brush entities' (Scene::PlaceBrushWater), found by their bounds alone.
struct WaterViews {
    UINT bodiesView = DescriptorHeap::kInvalid;
    UINT bodyCount = 0;
    UINT brushFirst = 0;
    UINT leafWaterView = DescriptorHeap::kInvalid;
    UINT trianglesView = DescriptorHeap::kInvalid;
    UINT framesView = DescriptorHeap::kInvalid;
    float top = -1e30f;
};

// A static self-illuminated triangle sampled as an area light (Emitter in shaders/scene.hlsli): its corners in world
// space, its scene instance and triangle, and the share of all emitters' emission times area up to and with it. The
// light grid's cells sample the emitters that light them most by their bound there, the rest by this share.
struct GpuEmitter {
    float p0[3];
    uint32_t instance;
    float p1[3];
    uint32_t primitive;
    float p2[3];
    float cdf;
};
static_assert(sizeof(GpuEmitter) == 48);

// Map light flags (Light::flags in shaders/scene.hlsli): the light sits inside lamp shells (kLampShellMask), which its
// shadow rays pass.
constexpr uint32_t kLightSealed = 0x01;

// Map lights have no size in Hammer; they are lit as spheres of this radius, about a bulb's.
constexpr float kBulbRadius = 2.0f;

// What the scene adds to a map light (bsp::Light): kLightSealed, and the radius it is lit with (kBulbRadius, or its
// lamp shell's).
struct MapLightShape {
    uint32_t flags = 0;
    float radius = kBulbRadius;
};

// Triangles for Scene::AddModel. Alpha-tested and translucent triangles come last, from `opaqueTriangles` on; each
// has an entry in `alphaTriangles` (Scene::PackAlpha or Scene::PackTranslucent), and then the model needs `uvs`.
struct ModelGeometry {
    std::span<const bsp::Float3> positions;
    std::span<const uint32_t> indices;
    // One packed RGBA8 linear colour per triangle.
    std::span<const uint32_t> triangleAlbedo;
    UINT opaqueTriangles;
    std::span<const bsp::Float2> uvs;
    std::span<const uint32_t> alphaTriangles;
    // A Scene::PackTint value for each of alphaTriangles; translucent triangles colour the light through them by it.
    std::span<const uint32_t> alphaTints;
    // Each triangle's Scene::AddMaterial index, or Scene::kNoMaterial; empty when the model has none.
    std::span<const uint32_t> triangleMaterials;
};

// What shaders need of a surface's material beyond its albedo (Material in shaders/scene.hlsli). A mask is
// off (Scene::kMaskOff), constant 1, or a channel of a texture (Scene::PackMask).
struct GpuMaterial {
    float envmapTint[3];
    uint32_t envmapMask;
    float selfIllumTint[3];
    uint32_t selfIllumMask;
    // $envmapcontrast blends the reflection towards its square, $envmapsaturation towards its grey.
    float envmapContrast;
    float envmapSaturation;
    // Scene::kMaterialUnlit.
    uint32_t flags;
    // LightmappedGeneric's $envmaplightscale and the threshold and scale its shader applies to the lightmap
    // ($envmaplightscaleminmax x, and x + y): reflections fade where the surface is dark.
    float envmapLightScale;
    float envmapLightMin;
    float envmapLightRange;
    // The base texture (DescriptorHeap::kInvalid for none) and the luminance of its average colour (vtex's
    // reflectivity), from which reflected and refracted hits take their texture's detail (TexturedAlbedo in
    // shaders/scene.hlsli).
    uint32_t baseTexture;
    float reflectivity;
};

struct SceneInstance {
    int model = -1;
    // Geometry the scene does not own (skinned models): their BLAS and positions view replace the model's.
    D3D12_GPU_VIRTUAL_ADDRESS blas = 0;
    UINT positionsView = DescriptorHeap::kInvalid;
    UINT mask = 0;
    // kInstanceIgnoreNormals.
    uint32_t flags = 0;
    // Brush entity water's body of water (Scene::SetWater's bodies); the world's water surfaces find theirs per
    // triangle.
    static constexpr uint32_t kNoWaterBody = 0xFFFFFFFFu;
    uint32_t waterBody = kNoWaterBody;
    float transform[3][4];
    // Where the instance was last frame, for motion vectors: its transform when `previousKnown`, and a skinned
    // model's vertex positions when the view is valid.
    bool previousKnown = false;
    float previousTransform[3][4];
    UINT previousPositionsView = DescriptorHeap::kInvalid;
};

// A world triangle's lightmap and its corners' luxel coordinates. Mirrors BakedTriangle in shaders/baked.hlsli.
struct BakedTriangle {
    // bsp::BakedLighting::faces index, or bsp::kNoFace.
    uint32_t face;
    float luxels[3][2];
    uint32_t unused;
};
static_assert(sizeof(BakedTriangle) == 32);

// The format of the baked light cache (gmodplusplus\cacheaked): bump when shaders/baked_bounce.hlsl or the data it
// reads changes.
inline constexpr uint32_t kBakeVersion = 16;

// The map's baked bounce light on the GPU (indirect_lighting = 1): raw views of the lightmap layout of
// bsp::BakedLighting and of the world's BakedTriangles; the bounce light shaders/baked_bounce.hlsl traces per lightmap
// sample as half4, then filters within each face; and the leaf ambient samples, whose cubes it traces in place. The
// cache holds the filtered light, then the ambient samples.
struct BakedBuffers {
    UINT facesView = DescriptorHeap::kInvalid;
    UINT trianglesView = DescriptorHeap::kInvalid;
    UINT luxelsView = DescriptorHeap::kInvalid;
    UINT leafAmbientView = DescriptorHeap::kInvalid;
    UINT ambientSamplesView = DescriptorHeap::kInvalid;
    UINT ambientSamplesUav = DescriptorHeap::kInvalid;
    UINT ambientSampleCount = 0;
    UINT rawView = DescriptorHeap::kInvalid;
    UINT rawUav = DescriptorHeap::kInvalid;
    UINT filteredView = DescriptorHeap::kInvalid;
    UINT filteredUav = DescriptorHeap::kInvalid;
    UINT luxelCount = 0;
    // Progress of the bake, which Lighting runs a part of each main view, or whole while the map loads: the luxels
    // traced so far, then the filter and the leaf ambient samples.
    UINT tracedLuxels = 0;
    bool filtered = false;
    // Where the baked light is saved once baked; empty when it was loaded from there.
    std::filesystem::path cachePath;
    bool Ready() const { return filtered; }
};

struct SceneFrame {
    D3D12_GPU_VIRTUAL_ADDRESS tlas = 0;
    // Descriptor of this frame's InstanceRecord buffer, indexed by InstanceID().
    UINT instancesView = DescriptorHeap::kInvalid;
    UINT dynamicLightsView = DescriptorHeap::kInvalid;
    UINT dynamicLightCount = 0;
    // The light style values of Scene::SetLightStyles, as floats.
    UINT lightStylesView = DescriptorHeap::kInvalid;
    // The bodies of water with the brush entities' where Scene::PlaceBrushWater put them (kInvalid when the map has
    // none), and the highest surface.
    UINT waterBodiesView = DescriptorHeap::kInvalid;
    float waterTop = -1e30f;
};

// Where a brush entity's body of water is this frame: its entity's offset from the model's space.
struct BrushWaterPlacement {
    uint32_t body = 0;
    float offset[3] = {};
};

// Ray-tracing acceleration structures and the per-triangle data shaders need at a hit (positions, indices
// and albedo), plus the map's lights. One BLAS per model; the TLAS is rebuilt every frame.
class Scene {
public:
    static constexpr UINT kFrameSlots = 3;
    // Layout of a light in the GPU buffers (Light in shaders/scene.hlsli).
    struct GpuLight;

    // With `opacityMicromaps`, models get opacity micromaps for their alpha-tested and translucent triangles.
    bool Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue, DescriptorHeap& descriptors, bool opacityMicromaps);
    // The map's lights and sun come from `world`, with each light's shape in `lightShapes`; its triangles from
    // `worldGeometry`. `emitters` are its self-illuminated triangles (Emitter in shaders/scene.hlsli), sampled from the
    // light grid's cells; `worldEmits` gives the world kInstanceEmitter.
    // `emitterPower` is their emission times area in all.
    bool LoadMap(const std::string& name, const bsp::WorldGeometry& world, std::span<const MapLightShape> lightShapes, const ModelGeometry& worldGeometry,
        std::span<const GpuEmitter> emitters, bool worldEmits, float emitterPower);
    // Uploads the map's lightmap layout and leaf ambient samples for indirect_lighting = 1, with the world model's
    // triangles in their BLAS order; false (logged) when the map has none or the upload fails. The light of an earlier
    // bake of the same map (`cached`, empty for none) makes the bake unnecessary.
    bool LoadBaked(const bsp::BakedLighting& baked, std::span<const BakedTriangle> triangles, std::span<const std::byte> cached);
    bool HasBaked() const { return m_baked.trianglesView != DescriptorHeap::kInvalid; }
    BakedBuffers& Baked() { return m_baked; }
    ID3D12Resource* BakedRaw() const { return m_bakedRaw.Get(); }
    ID3D12Resource* BakedFiltered() const { return m_bakedFiltered.Get(); }
    ID3D12Resource* BakedAmbient() const { return m_bakedAmbient.Get(); }
    // Builds a BLAS for the geometry and returns its model id, or -1 when the geometry is empty or fails.
    int AddModel(const ModelGeometry& geometry);
    // A texture for alpha tests and material masks: VTF mips in their stored layout, largest first. Returns its
    // descriptor, or DescriptorHeap::kInvalid.
    UINT AddTexture(DXGI_FORMAT format, UINT width, UINT height, std::span<const std::span<const std::byte>> mips);
    // An entry of ModelGeometry::alphaTriangles: the alpha texture and the alpha test reference (0-1).
    static uint32_t PackAlpha(UINT textureView, float reference);
    // An entry of ModelGeometry::alphaTriangles for a translucent surface: its opacity is the texture's alpha (1
    // with DescriptorHeap::kInvalid) times `opacity`. Shadow rays pass through what is left; other rays stop at
    // surfaces at least half opaque.
    static uint32_t PackTranslucent(UINT textureView, float opacity);
    // How a translucent triangle colours the light through it (AlphaTriangle::tint in shaders/scene.hlsli): kTintFilter
    // passes (1 - opacity) and the covered part filtered by the texture's colour times `tint`; kTintRefract passes
    // `tint` times the texture's colour (Refract glass).
    static constexpr uint32_t kTintFilter = 1;
    static constexpr uint32_t kTintRefract = 2;
    // Added to either: the surface is a pane of glass, whose two faces reflect part of the light (GlassTransmission in
    // shaders/scene.hlsli).
    static constexpr uint32_t kTintGlass = 4;
    // Set by AddModel on glass with another glass face of the model close behind it: one face of a thick pane, which
    // reflects at that face alone.
    static constexpr uint32_t kTintGlassFace = 8;
    // How far behind a glass face the pane's other face may be (a window's glass is a few units thick).
    static constexpr float kMaxGlassThickness = 16.0f;
    static uint32_t PackTint(const bsp::Float3& tint, uint32_t mode);

    static constexpr uint32_t kNoMaterial = 0xFFFFFFFF;
    // GpuMaterial::flags: UnlitGeneric and its relatives show their albedo unlit.
    static constexpr uint32_t kMaterialUnlit = 1;
    // A face's light texture, which VRAD made emit_surface lights of: its glow shows but lights nothing again.
    static constexpr uint32_t kMaterialLightTexture = 2;
    static constexpr uint32_t kMaskOff = 0xFFFFFFFF;
    // A mask from one channel (0-3: r, g, b, a) of a texture, optionally inverted; kInvalid as the texture
    // makes it constant 1.
    static uint32_t PackMask(UINT textureView, uint32_t channel, bool invert);
    // Adds a material for ModelGeometry::triangleMaterials and returns its index, or kNoMaterial when full.
    uint32_t AddMaterial(const GpuMaterial& material);
    UINT MaterialsView() const { return m_materialsView; }
    // A model's BLAS geometry: its opaque triangles, then its alpha-tested and translucent ones. Returns the number of
    // descs used.
    static UINT GeometryDescs(D3D12_GPU_VIRTUAL_ADDRESS vertices, UINT vertexCount, D3D12_GPU_VIRTUAL_ADDRESS indices, UINT triangles,
        UINT opaqueTriangles, D3D12_RAYTRACING_GEOMETRY_DESC (&descs)[2]);
    // Points the alpha geometry of a copy of a model (skinned casters) at the model's opacity micromaps; false when it
    // has none. `triangles` and `linkage` must stay alive until the build is recorded. Such BLASes need
    // ALLOW_DISABLE_OMMS, as RecordFrame may disable their micromaps.
    bool LinkMicromaps(int model, D3D12_RAYTRACING_GEOMETRY_DESC& alphaGeometry, D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles,
        D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage) const;
    // A model's geometry, for copies of its BLAS with moved vertices (SwayingProps).
    struct ModelBuffers {
        UINT positionsView;
        D3D12_GPU_VIRTUAL_ADDRESS indices;
        UINT vertexCount;
        UINT triangleCount;
        UINT opaqueTriangles;
    };
    std::optional<ModelBuffers> Buffers(int model) const;
    // Static props, and the 3D skybox's geometry with kSkyboxMask; a mask of 0 means kStaticPropMask.
    void SetStaticInstances(std::vector<SceneInstance> instances);
    // Marks a model as a character's: its instances get kInstanceCharacter.
    void MarkCharacter(int model)
    {
        if (model >= 0 && static_cast<size_t>(model) < m_models.size())
            m_models[model].character = true;
    }
    // The map's bodies of water, the descriptors of their normal maps' frames, each water surface triangle's body (in the
    // water model's BLAS order) and each BSP leaf's body; false (logged) when the upload fails.
    bool SetWater(std::span<const GpuWaterBody> bodies, std::span<const uint32_t> frameViews, std::span<const uint32_t> triangleBodies,
        const bsp::Visibility& visibility, UINT brushFirst);
    // Moves the brush entities' bodies of water (from WaterViews::brushFirst on, given in their model's space) to where
    // their entities are this frame; bodies not placed hold no water.
    void PlaceBrushWater(std::span<const BrushWaterPlacement> placements);
    const WaterViews& Water() const { return m_water; }
    std::span<const GpuWaterBody> WaterBodies() const { return m_waterBodies; }
    // The body of water holding a point (WaterBodyAt in shaders/water_body.hlsli), or DescriptorHeap::kInvalid.
    UINT WaterAt(const float (&point)[3]) const;
    // Whether a body of water's surface lies over part of a square of the ground plane.
    bool WaterWithin(float minX, float minY, float maxX, float maxY) const;
    UINT EmittersView() const { return m_emittersView; }
    UINT EmitterCount() const { return m_emitterCount; }
    float EmitterPower() const { return m_emitterPower; }
    bool HasSkybox() const { return m_hasSkybox; }
    // The engine's dynamic lights for the next RecordFrame.
    void SetDynamicLights(std::span<const engine::DynamicLight> lights) { m_dynamicLights.assign(lights.begin(), lights.end()); }
    // The engine's light style values for the next RecordFrame; every style stays at 1 until they are set.
    void SetLightStyles(const std::array<float, engine::kLightStyles>& values) { m_lightStyles = values; }
    const std::array<float, engine::kLightStyles>& LightStyles() const { return m_lightStyles; }
    // The current value of a light style (bsp::Light::style).
    float LightStyle(int32_t style) const { return style >= 0 && static_cast<size_t>(style) < m_lightStyles.size() ? m_lightStyles[style] : 1.0f; }
    void Clear();

    bool Ready() const { return m_worldModel >= 0; }
    const std::string& Name() const { return m_name; }
    const bsp::SunLight& Sun() const { return m_sun; }
    UINT LightsView() const { return m_lightsView; }
    UINT LightCount() const { return m_lightCount; }

    // The map lights each visibility cluster can see and be reached by, found per point by walking the BSP tree
    // (ClusterLights in shaders/scene.hlsli, for the bake: VRAD's PVSCheck). `rangesView` holds an (offset, count) range
    // of light indices per cluster and one more, after them, with every light for points outside any cluster.
    // The light grid (LightsAt) serves everything else: per cell (x fastest), `gridCellsView` holds two uint4: the range
    // of its listed lights in `gridIndicesView`, the strongest that can reach it, and of its tail in `gridTailView`: every
    // other light that can reach it, with cumulative weights for importance sampling; then the range in `gridTailView` of
    // the emitters that can light it, by the same weights (EmittersAt).
    struct LightLists {
        UINT rangesView = DescriptorHeap::kInvalid;
        UINT indicesView = DescriptorHeap::kInvalid;
        UINT nodesView = DescriptorHeap::kInvalid;
        UINT leafClustersView = DescriptorHeap::kInvalid;
        UINT clusterCount = 0;
        UINT gridCellsView = DescriptorHeap::kInvalid;
        UINT gridIndicesView = DescriptorHeap::kInvalid;
        UINT gridTailView = DescriptorHeap::kInvalid;
        bsp::Float3 gridOrigin = {};
        float gridCellSize = 0.0f;
        uint32_t gridDims[3] = {};
    };
    const LightLists& Lights() const { return m_lightLists; }

    // The model and mask of an instance of the last RecordFrame.
    std::pair<int, UINT> InstanceInfo(UINT instance) const
    {
        return instance < m_recorded.size() ? m_recorded[instance] : std::pair<int, UINT>{-1, 0};
    }

    // Records a TLAS build for this frame into `list`; the caller waits for `slot`'s previous use first. Without
    // `micromaps`, instances ignore their models' opacity micromaps.
    SceneFrame RecordFrame(ID3D12GraphicsCommandList4* list, UINT slot, std::span<const SceneInstance> dynamic, bool micromaps);
    // Alpha triangles with an opacity micromap, and the micromaps they share, over all models.
    std::pair<uint32_t, uint32_t> MicromapCounts() const { return {m_micromapTriangles, m_micromapCount}; }
    bool HasMicromaps() const { return m_micromapModels != 0; }

private:
    struct Model {
        Microsoft::WRL::ComPtr<ID3D12Resource> blas;
        Microsoft::WRL::ComPtr<ID3D12Resource> positions;
        Microsoft::WRL::ComPtr<ID3D12Resource> indices;
        Microsoft::WRL::ComPtr<ID3D12Resource> albedo;
        Microsoft::WRL::ComPtr<ID3D12Resource> uvs;
        Microsoft::WRL::ComPtr<ID3D12Resource> alpha;
        Microsoft::WRL::ComPtr<ID3D12Resource> materials;
        Microsoft::WRL::ComPtr<ID3D12Resource> micromaps;
        Microsoft::WRL::ComPtr<ID3D12Resource> micromapIndices;
        bool hasMicromaps = false;
        bool selfIllum = false;
        bool character = false;
        UINT positionsView = DescriptorHeap::kInvalid;
        UINT indicesView = DescriptorHeap::kInvalid;
        UINT albedoView = DescriptorHeap::kInvalid;
        UINT uvsView = DescriptorHeap::kInvalid;
        UINT alphaView = DescriptorHeap::kInvalid;
        UINT materialsView = DescriptorHeap::kInvalid;
        UINT vertexCount = 0;
        UINT triangleCount = 0;
        UINT opaqueTriangles = 0;
    };
    struct Texture {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        UINT view = DescriptorHeap::kInvalid;
    };
    struct InstanceRecord;
    struct FrameTlas {
        Microsoft::WRL::ComPtr<ID3D12Resource> instances;
        D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> records;
        InstanceRecord* mappedRecords = nullptr;
        UINT recordsView = DescriptorHeap::kInvalid;
        UINT capacity = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> dynamicLights;
        GpuLight* mappedDynamicLights = nullptr;
        UINT dynamicLightsView = DescriptorHeap::kInvalid;
        Microsoft::WRL::ComPtr<ID3D12Resource> lightStyles;
        float* mappedLightStyles = nullptr;
        UINT lightStylesView = DescriptorHeap::kInvalid;
        Microsoft::WRL::ComPtr<ID3D12Resource> waterBodies;
        GpuWaterBody* mappedWaterBodies = nullptr;
        UINT waterBodiesView = DescriptorHeap::kInvalid;
        size_t waterBodyCapacity = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
        Microsoft::WRL::ComPtr<ID3D12Resource> tlas;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);
    // Records a copy of `data` into a new default-heap buffer; `staging` must outlive the command list.
    Microsoft::WRL::ComPtr<ID3D12Resource> UploadToDefault(const void* data, UINT64 size, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& staging);
    bool Submit();
    void ReleaseModel(Model& model);
    static void Link(const Model& model, D3D12_RAYTRACING_GEOMETRY_DESC& alphaGeometry, D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles,
        D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage);
    bool EnsureFrameCapacity(FrameTlas& frame, UINT count);
    bool EnsureDynamicLights(FrameTlas& frame);
    bool EnsureLightStyles(FrameTlas& frame);
    bool EnsureWaterBodies(FrameTlas& frame, size_t count);
    bool BuildLightLists(std::span<const bsp::Light> lights, const bsp::Visibility& visibility);
    // The light grid of LightLists over the world's triangles. A cell lists the lights with the largest bound there
    // (LightBound), lights its clusters' PVS leaves out weighed down but kept, and samples the rest; it samples the
    // emitters by their bound (EmitterBound).
    bool BuildLightGrid(std::span<const GpuLight> lights, std::span<const bsp::Light> mapLights, std::span<const GpuEmitter> emitters,
        const bsp::Visibility& visibility, std::span<const bsp::Float3> worldPositions);
    // Uploads the emitters and, for each instance with some, its triangles' emitter indices plus one (0 for none;
    // InstanceRecord::emitterLights).
    bool UploadEmitters(std::span<const GpuEmitter> emitters, bool worldEmits);
    // Bakes and records the build of a model's opacity micromap array and points its alpha geometry at it.
    bool AddMicromaps(Model& model, const ModelGeometry& geometry, D3D12_RAYTRACING_GEOMETRY_DESC& alphaGeometry,
        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles, D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& staging);

    ID3D12Device5* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    DescriptorHeap* m_descriptors = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> m_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;

    std::vector<Model> m_models;
    std::vector<Texture> m_textures;
    bool m_opacityMicromaps = false;
    // Alpha of the textures alpha tests read, by descriptor, for baking opacity micromaps.
    std::unordered_map<uint32_t, omm::AlphaImage> m_alphaImages;
    uint32_t m_micromapTriangles = 0;
    uint32_t m_micromapCount = 0;
    uint32_t m_micromapModels = 0;
    static constexpr UINT kMaxMaterials = 8192;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materials;
    GpuMaterial* m_mappedMaterials = nullptr;
    UINT m_materialsView = DescriptorHeap::kInvalid;
    UINT m_materialCount = 0;
    // Which materials are self-illuminated (GpuMaterial::selfIllumMask set).
    std::vector<bool> m_selfIllumMaterials;
    std::vector<SceneInstance> m_staticInstances;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_emitters;
    UINT m_emittersView = DescriptorHeap::kInvalid;
    UINT m_emitterCount = 0;
    float m_emitterPower = 0.0f;
    // Per instance of the world and static instances (0 is the world, static instance i is i + 1): the view of its
    // triangles' emitter indices, or kInvalid.
    std::vector<UINT> m_emitterLightViews;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_emitterLights;
    // kInstanceEmitter when the world has emitters.
    uint32_t m_worldFlags = 0;
    std::vector<std::pair<int, UINT>> m_recorded;
    bool m_hasSkybox = false;
    std::vector<engine::DynamicLight> m_dynamicLights;
    std::array<float, engine::kLightStyles> m_lightStyles = [] {
        std::array<float, engine::kLightStyles> values;
        values.fill(1.0f);
        return values;
    }();
    std::array<FrameTlas, kFrameSlots> m_frames;
    int m_worldModel = -1;
    std::string m_name;
    bsp::SunLight m_sun;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lights;
    UINT m_lightsView = DescriptorHeap::kInvalid;
    UINT m_lightCount = 0;
    LightLists m_lightLists;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightRanges;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightIndices;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_gridCells;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_gridIndices;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_gridTail;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bspNodes;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_leafClusters;
    WaterViews m_water;
    std::vector<GpuWaterBody> m_waterBodies;
    // m_waterBodies with the brush entities' where PlaceBrushWater put them, and their highest surface.
    std::vector<GpuWaterBody> m_placedWaterBodies;
    float m_placedWaterTop = -1e30f;
    // The BSP tree and leaves, for WaterAt.
    bsp::Visibility m_waterVisibility;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_waterBuffers;
    BakedBuffers m_baked;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_bakedInputs;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bakedRaw;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bakedFiltered;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bakedAmbient;
};

}
