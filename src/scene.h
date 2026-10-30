#pragma once

#include "bsp.h"
#include "convex_hull.h"
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
// Static props the map marks as casting no shadows (often foliage). They shadow the sun but not local lights, so a
// lamp model marked this way still lets its own light out as VRAD did.
constexpr UINT kShadowlessPropMask = 0x20;
// Static props and brushes as VRAD's shadows saw them (collision models or mesh hulls, every side of opaque brushes),
// traced only by the residual bake, with the sky faces that end VRAD's sun rays.
constexpr UINT kVradOccluderMask = 0x40;
constexpr UINT kVradSkyMask = 0x80;

// SceneInstance::flags, read by shaders from the instance record.
constexpr uint32_t kInstanceIgnoreNormals = 0x01;

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
    float padding[2];
};

// A convex hull that blocks a map light (index into bsp::WorldGeometry::lights) wherever the straight path from
// the light to a point crosses it.
struct LightHull {
    uint32_t light;
    std::vector<hull::Plane> planes;
};

struct SceneInstance {
    int model = -1;
    // Geometry the scene does not own (skinned models): their BLAS and positions view replace the model's.
    D3D12_GPU_VIRTUAL_ADDRESS blas = 0;
    UINT positionsView = DescriptorHeap::kInvalid;
    UINT mask = 0;
    // kInstanceIgnoreNormals.
    uint32_t flags = 0;
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

// The format of the baked residual cache (gmodpp_cache\baked): bump when shaders/baked_residual.hlsl or the data it
// reads changes.
inline constexpr uint32_t kBakeVersion = 11;

// VRAD's baked light on the GPU (indirect_lighting = 1): raw views of bsp::BakedLighting and of the world's
// BakedTriangles, and the residual shaders/baked_residual.hlsl writes: the baked light less the direct light VRAD
// summed into it, per lightmap sample, as half4, then filtered within each face. The leaf ambient samples lose the
// direct light of the lights VRAD added to them in the same bake. The cache holds the filtered residual, then the
// ambient samples.
struct BakedBuffers {
    UINT samplesView = DescriptorHeap::kInvalid;
    UINT facesView = DescriptorHeap::kInvalid;
    UINT trianglesView = DescriptorHeap::kInvalid;
    UINT luxelsView = DescriptorHeap::kInvalid;
    UINT leafAmbientView = DescriptorHeap::kInvalid;
    UINT ambientSamplesView = DescriptorHeap::kInvalid;
    UINT ambientSamplesUav = DescriptorHeap::kInvalid;
    UINT ambientLightsView = DescriptorHeap::kInvalid;
    UINT ambientSampleCount = 0;
    UINT ambientLightCount = 0;
    UINT residualView = DescriptorHeap::kInvalid;
    UINT residualUav = DescriptorHeap::kInvalid;
    UINT filteredView = DescriptorHeap::kInvalid;
    UINT filteredUav = DescriptorHeap::kInvalid;
    // Clamped luxels, luxels the filter changed by more than a tenth, luxels without a VRAD sample and ambient
    // samples clamped (shaders/baked_residual.hlsl).
    UINT countersUav = DescriptorHeap::kInvalid;
    UINT luxelCount = 0;
    UINT leafCount = 0;
    // Progress of the bake, which Lighting runs a part of each main view: luxels done, then the filter.
    UINT bakedLuxels = 0;
    bool filtered = false;
    // Where the filtered residual is saved once baked; empty when it was loaded from there.
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
    // The map's lights and sun come from `world`, with the hulls that block some lights; its triangles from
    // `worldGeometry`.
    bool LoadMap(const std::string& name, const bsp::WorldGeometry& world, std::span<const LightHull> lightHulls, const ModelGeometry& worldGeometry);
    // Uploads the map's baked light for indirect_lighting = 1, with the world model's triangles in their BLAS order;
    // false (logged) when the map has none or the upload fails. A filtered residual from an earlier bake of the same
    // map (`cachedResidual`, empty for none) makes the bake unnecessary.
    bool LoadBaked(const bsp::BakedLighting& baked, std::span<const BakedTriangle> triangles, std::span<const std::byte> cachedResidual);
    bool HasBaked() const { return m_baked.trianglesView != DescriptorHeap::kInvalid; }
    BakedBuffers& Baked() { return m_baked; }
    ID3D12Resource* BakedResidual() const { return m_bakedResidual.Get(); }
    ID3D12Resource* BakedFiltered() const { return m_bakedFiltered.Get(); }
    ID3D12Resource* BakedCounters() const { return m_bakedCounters.Get(); }
    ID3D12Resource* BakedAmbient() const { return m_bakedAmbient.Get(); }
    // A baked face's first luxel and its normal, for the log of the most clamped faces.
    struct BakedFaceInfo {
        bsp::Float3 position;
        bsp::Float3 normal;
    };
    const std::vector<BakedFaceInfo>& BakedFaces() const { return m_bakedFaces; }
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

    static constexpr uint32_t kNoMaterial = 0xFFFFFFFF;
    // GpuMaterial::flags: UnlitGeneric and its relatives show their albedo unlit.
    static constexpr uint32_t kMaterialUnlit = 1;
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
    bool HasSkybox() const { return m_hasSkybox; }
    // The engine's dynamic lights for the next RecordFrame.
    void SetDynamicLights(std::span<const engine::DynamicLight> lights) { m_dynamicLights.assign(lights.begin(), lights.end()); }
    // The engine's light style values for the next RecordFrame; every style stays at 1 until they are set.
    void SetLightStyles(const std::array<float, engine::kLightStyles>& values) { m_lightStyles = values; }
    // The current value of a light style (bsp::Light::style).
    float LightStyle(int32_t style) const { return style >= 0 && static_cast<size_t>(style) < m_lightStyles.size() ? m_lightStyles[style] : 1.0f; }
    void Clear();

    bool Ready() const { return m_worldModel >= 0; }
    const std::string& Name() const { return m_name; }
    const bsp::SunLight& Sun() const { return m_sun; }
    UINT LightsView() const { return m_lightsView; }
    UINT LightCount() const { return m_lightCount; }

    // The map lights each visibility cluster can see and be reached by, found per point by walking the BSP tree
    // (LightsAt in shaders/scene.hlsli). `rangesView` holds an (offset, count) range of light indices per cluster
    // and one more, after them, with every light for points outside any cluster.
    struct LightLists {
        UINT rangesView = DescriptorHeap::kInvalid;
        UINT indicesView = DescriptorHeap::kInvalid;
        UINT nodesView = DescriptorHeap::kInvalid;
        UINT leafClustersView = DescriptorHeap::kInvalid;
        // bsp::Visibility::sunClusters as bits, or kInvalid for every cluster.
        UINT sunClustersView = DescriptorHeap::kInvalid;
        UINT clusterCount = 0;
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
    bool BuildLightLists(std::span<const bsp::Light> lights, const bsp::Visibility& visibility);
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
    std::vector<SceneInstance> m_staticInstances;
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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bspNodes;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_leafClusters;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sunClusters;
    BakedBuffers m_baked;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_bakedInputs;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bakedResidual;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bakedFiltered;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bakedCounters;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bakedAmbient;
    std::vector<BakedFaceInfo> m_bakedFaces;
};

}
