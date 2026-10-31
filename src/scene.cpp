#include "scene.h"

#include "log.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <execution>
#include <limits>
#include <numeric>
#include <optional>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

// Mirrors InstanceRecord in shaders/scene.hlsli.
struct Scene::InstanceRecord {
    uint32_t positions;
    uint32_t indices;
    uint32_t albedo;
    uint32_t uvs;
    uint32_t alpha;
    uint32_t opaqueTriangles;
    uint32_t materials;
    uint32_t previousPositions;
    float previousTransform[3][4];
    uint32_t flags;
    uint32_t emitterLights;
    uint32_t padding[2];
};
static_assert(sizeof(GpuMaterial) == 64);

// Mirrors AlphaTriangle in shaders/scene.hlsli: an alpha-tested or translucent triangle's ModelGeometry::alphaTriangles
// entry with its texture coordinates, so a candidate's alpha test reads one record.
struct AlphaTriangle {
    uint32_t packed;
    bsp::Float2 uvs[3];
    // ModelGeometry::alphaTints.
    uint32_t tint;
};
static_assert(sizeof(AlphaTriangle) == 32);

// Mirrors Light in shaders/scene.hlsli.
struct Scene::GpuLight {
    float origin[3];
    uint32_t type;
    float intensity[3];
    float radius;
    float normal[3];
    float stopdot;
    float stopdot2;
    float exponent;
    float constantAttn;
    float linearAttn;
    float quadraticAttn;
    // kLightSealed.
    uint32_t flags;
    // MapLightShape::radius; 0 for the engine's dynamic lights.
    float size;
    // bsp::Light::style, or kNoLightStyle.
    uint32_t style;
    // VRAD's falloff at the light's reference distance (bsp::Light::referenceDistance) times its square: point and spot
    // lights fall off with the inverse square scaled by it, which gives the mapper's brightness there.
    float referenceScale;
    uint32_t padding[3];
};
static_assert(sizeof(Scene::GpuLight) == 96);
// Read by ClusterAt in shaders/scene.hlsli.
static_assert(sizeof(bsp::BspNode) == 24);

namespace {

constexpr UINT kMaxDynamicLights = 32;
constexpr uint32_t kDynamicLightType = 3;
constexpr uint32_t kNoLightStyle = 0xFFFFFFFF;
constexpr float kDegreesToRadians = 3.14159265358979f / 180.0f;
// A light is binned as far as its irradiance can reach 1/1024 of white with up to 4x light intensity and a 4x
// tonemap scale.
constexpr float kLightThreshold = 1.0f / 16384.0f;

// The light grid (Scene::BuildLightGrid): at most kGridCells cubic cells of at least kGridCellMin units over the world,
// and few enough that their tails hold about kGridTailEntries lights in all.
constexpr float kGridCellMin = 256.0f;
constexpr double kGridCells = 4096.0;
constexpr double kGridTailEntries = 4.0 * 1024.0 * 1024.0;
// Lights a cell lists for the light loop (shaders/direct.hlsl); its other lights are sampled from its tail.
constexpr size_t kGridListLights = 64;
// Emitters a cell samples by their bound there; the others are sampled by their power over the whole map.
constexpr size_t kGridEmitters = 256;
// The weight, relative to its bound, of a light the PVS of a cell's clusters leaves out: such light rarely arrives, but
// nothing proves it cannot.
constexpr float kHiddenWeight = 1.0f / 16.0f;
// Marks a listed light the PVS leaves out (kHiddenLight in shaders/scene.hlsli).
constexpr uint32_t kHiddenLight = 0x80000000u;

// The most a light gives a point of a box before shadows, with a Lambert term of 1: the inverse square from its sphere
// at the box's nearest point (LightIrradiance in shaders/scene.hlsli); 0 where no point of the box is in front of a
// surface light or near a spot light's cone.
float LightBound(const Scene::GpuLight& light, const bsp::Float3& low, const bsp::Float3& high)
{
    const float intensity = std::max({light.intensity[0], light.intensity[1], light.intensity[2], 0.0f});
    const float lo[3] = {low.x, low.y, low.z};
    const float hi[3] = {high.x, high.y, high.z};
    float nearest = 0.0f;
    float centre[3];
    float halfDiagonal = 0.0f;
    float reach = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const float d = std::max({lo[a] - light.origin[a], 0.0f, light.origin[a] - hi[a]});
        nearest += d * d;
        centre[a] = 0.5f * (lo[a] + hi[a]) - light.origin[a];
        halfDiagonal += 0.25f * (hi[a] - lo[a]) * (hi[a] - lo[a]);
        reach += std::max((lo[a] - light.origin[a]) * light.normal[a], (hi[a] - light.origin[a]) * light.normal[a]);
    }
    halfDiagonal = std::sqrt(halfDiagonal);
    if (light.type == static_cast<uint32_t>(bsp::LightType::Surface))
        return reach > 0.0f ? intensity / std::max(nearest, 1.0f) : 0.0f;
    if (light.type == static_cast<uint32_t>(bsp::LightType::Spot)) {
        const float distance = std::sqrt(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]);
        if (distance > halfDiagonal) {
            const float cosine = (centre[0] * light.normal[0] + centre[1] * light.normal[1] + centre[2] * light.normal[2]) / distance;
            const float spread = std::asin(halfDiagonal / distance);
            // SpotFactor's fringe widens the cone by up to kSpotEdgeBlur (8) units either side; twice that is left.
            const float margin = std::asin(16.0f / std::max(distance - halfDiagonal, 16.0f));
            if (std::acos(std::clamp(cosine, -1.0f, 1.0f)) - spread > std::acos(std::clamp(light.stopdot2, -1.0f, 1.0f)) + margin)
                return 0.0f;
        }
    }
    return intensity * light.referenceScale / std::max(nearest, light.size * light.size);
}

// The most an emitter of `power` (its emission times its area, in any unit) gives a point of a box before shadows, with
// a Lambert term of 1: its power over the square of the distance between its bounds and the box (at least 1); 0 where
// the box lies wholly behind it.
float EmitterBound(const GpuEmitter& emitter, float power, const bsp::Float3& low, const bsp::Float3& high)
{
    const float* p[3] = {emitter.p0, emitter.p1, emitter.p2};
    const float lo[3] = {low.x, low.y, low.z};
    const float hi[3] = {high.x, high.y, high.z};
    const float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
    const float e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
    // Source's front faces: cross(p1 - p0, p2 - p0) points out of the back.
    const float front[3] = {e2[1] * e1[2] - e2[2] * e1[1], e2[2] * e1[0] - e2[0] * e1[2], e2[0] * e1[1] - e2[1] * e1[0]};
    float reach = 0.0f;
    float nearest = 0.0f;
    for (int a = 0; a < 3; ++a) {
        reach += std::max((lo[a] - p[0][a]) * front[a], (hi[a] - p[0][a]) * front[a]);
        const float triangleLow = std::min({p[0][a], p[1][a], p[2][a]});
        const float triangleHigh = std::max({p[0][a], p[1][a], p[2][a]});
        const float d = std::max({lo[a] - triangleHigh, 0.0f, triangleLow - hi[a]});
        nearest += d * d;
    }
    return reach > 0.0f ? power / std::max(nearest, 1.0f) : 0.0f;
}

// GpuLight::referenceScale: the inverse square matched to VRAD's attenuation at the reference distance, where VRAD's
// falloff is 1 / (constant + linear d + quadratic d^2) (GatherSampleStandardLightSSE in src/utils/vrad/lightmap.cpp).
float ReferenceScale(const bsp::Light& light)
{
    if (light.type == bsp::LightType::Surface)
        return 1.0f;
    const float d = light.referenceDistance;
    return d * d / std::max(light.constantAttn + light.linearAttn * d + light.quadraticAttn * d * d, 1e-4f);
}

// How far a light can reach visibly, following the falloff in LightIrradiance (shaders/scene.hlsli).
float LightRange(const bsp::Light& light)
{
    const float intensity = std::max({light.intensity.x, light.intensity.y, light.intensity.z, 0.0f});
    return std::sqrt(intensity * ReferenceScale(light) / kLightThreshold);
}

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TlasInputs(UINT count, D3D12_GPU_VIRTUAL_ADDRESS instances)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.NumDescs = count;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.InstanceDescs = instances;
    return inputs;
}

}

bool Scene::Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue, DescriptorHeap& descriptors, bool opacityMicromaps)
{
    m_device = device;
    m_opacityMicromaps = opacityMicromaps;
    m_queue = queue;
    m_descriptors = &descriptors;
    HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocator.Get(), nullptr, IID_PPV_ARGS(&m_list));
    if (SUCCEEDED(hr))
        hr = m_list->Close();
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        log::Info("Scene: initialization failed ({:#010x})", Hex(hr));
        return false;
    }
    m_materials = CreateBuffer(sizeof(GpuMaterial) * kMaxMaterials, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_materials || FAILED(m_materials->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedMaterials)))) {
        log::Info("Scene: creating the material table failed");
        return false;
    }
    m_materialsView = descriptors.CreateRawView(m_materials.Get());
    if (m_materialsView == DescriptorHeap::kInvalid)
        return false;
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

uint32_t Scene::PackMask(UINT textureView, uint32_t channel, bool invert)
{
    return (textureView & 0xFFFFFFu) | (channel & 3u) << 24 | (invert ? 1u << 26 : 0u);
}

uint32_t Scene::AddMaterial(const GpuMaterial& material)
{
    if (m_materialCount == kMaxMaterials)
        return kNoMaterial;
    m_mappedMaterials[m_materialCount] = material;
    m_selfIllumMaterials.push_back(material.selfIllumMask != kMaskOff);
    return m_materialCount++;
}

ComPtr<ID3D12Resource> Scene::CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heapProperties = {heap};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<UINT64>((size + 3) & ~UINT64(3), 4);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> buffer;
    HRESULT hr = m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    if (FAILED(hr))
        log::Info("Scene: buffer of {} bytes failed ({:#010x})", size, Hex(hr));
    return buffer;
}

ComPtr<ID3D12Resource> Scene::UploadToDefault(const void* data, UINT64 size, std::vector<ComPtr<ID3D12Resource>>& staging)
{
    ComPtr<ID3D12Resource> upload = CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    ComPtr<ID3D12Resource> buffer = CreateBuffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped = nullptr;
    if (!upload || !buffer || FAILED(upload->Map(0, nullptr, &mapped)))
        return nullptr;
    std::memcpy(mapped, data, size);
    upload->Unmap(0, nullptr);
    m_list->CopyBufferRegion(buffer.Get(), 0, upload.Get(), 0, size);
    D3D12_RESOURCE_BARRIER barrier = Transition(buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    m_list->ResourceBarrier(1, &barrier);
    staging.push_back(upload);
    return buffer;
}

bool Scene::Submit()
{
    HRESULT hr = m_list->Close();
    if (FAILED(hr)) {
        log::Info("Scene: closing the upload commands failed ({:#010x})", Hex(hr));
        return false;
    }
    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    m_queue->Signal(m_fence.Get(), ++m_fenceValue);
    m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);
    return true;
}

UINT Scene::GeometryDescs(D3D12_GPU_VIRTUAL_ADDRESS vertices, UINT vertexCount, D3D12_GPU_VIRTUAL_ADDRESS indices, UINT triangles,
    UINT opaqueTriangles, D3D12_RAYTRACING_GEOMETRY_DESC (&descs)[2])
{
    UINT count = 0;
    auto add = [&](UINT first, UINT size, D3D12_RAYTRACING_GEOMETRY_FLAGS flags) {
        if (size == 0)
            return;
        D3D12_RAYTRACING_GEOMETRY_DESC& geometry = descs[count++];
        geometry = {};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = flags;
        geometry.Triangles.VertexBuffer.StartAddress = vertices;
        geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(bsp::Float3);
        geometry.Triangles.VertexCount = vertexCount;
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.IndexBuffer = indices + static_cast<UINT64>(first) * 3 * sizeof(uint32_t);
        geometry.Triangles.IndexCount = size * 3;
        geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    };
    add(0, opaqueTriangles, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
    // Shaders find a triangle's data by its index within this geometry plus opaqueTriangles. Shadow rays multiply
    // the transmittance of each translucent candidate, so none may be reported twice.
    add(opaqueTriangles, triangles - opaqueTriangles, D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION);
    return count;
}

uint32_t Scene::PackAlpha(UINT textureView, float reference)
{
    return (textureView & 0xFFFFFFu) | static_cast<uint32_t>(std::lround(std::clamp(reference, 0.0f, 1.0f) * 127.0f)) << 24;
}

uint32_t Scene::PackTranslucent(UINT textureView, float opacity)
{
    return (textureView & 0xFFFFFFu) | static_cast<uint32_t>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 127.0f)) << 24 | 1u << 31;
}

uint32_t Scene::PackTint(const bsp::Float3& tint, uint32_t mode)
{
    const auto channel = [](float c) { return static_cast<uint32_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); };
    return channel(tint.x) | channel(tint.y) << 8 | channel(tint.z) << 16 | mode << 24;
}

UINT Scene::AddTexture(DXGI_FORMAT format, UINT width, UINT height, std::span<const std::span<const std::byte>> mips)
{
    constexpr UINT kMaxMips = 16;
    const UINT mipCount = static_cast<UINT>(mips.size());
    if (mipCount == 0 || mipCount > kMaxMips)
        return DescriptorHeap::kInvalid;
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(mipCount);
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    Texture texture;
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.resource))))
        return DescriptorHeap::kInvalid;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[kMaxMips] = {};
    UINT rows[kMaxMips] = {};
    UINT64 rowBytes[kMaxMips] = {};
    UINT64 total = 0;
    m_device->GetCopyableFootprints(&desc, 0, mipCount, 0, footprints, rows, rowBytes, &total);
    for (UINT mip = 0; mip < mipCount; ++mip) {
        if (rowBytes[mip] * rows[mip] != mips[mip].size())
            return DescriptorHeap::kInvalid;
    }
    ComPtr<ID3D12Resource> upload = CreateBuffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    std::byte* mapped = nullptr;
    if (!upload || FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return DescriptorHeap::kInvalid;
    for (UINT mip = 0; mip < mipCount; ++mip) {
        for (UINT row = 0; row < rows[mip]; ++row)
            std::memcpy(mapped + footprints[mip].Offset + static_cast<size_t>(row) * footprints[mip].Footprint.RowPitch,
                mips[mip].data() + row * rowBytes[mip], rowBytes[mip]);
    }
    upload->Unmap(0, nullptr);

    m_allocator->Reset();
    m_list->Reset(m_allocator.Get(), nullptr);
    for (UINT mip = 0; mip < mipCount; ++mip) {
        D3D12_TEXTURE_COPY_LOCATION destination = {texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        destination.SubresourceIndex = mip;
        D3D12_TEXTURE_COPY_LOCATION source = {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        source.PlacedFootprint = footprints[mip];
        m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    D3D12_RESOURCE_BARRIER barrier = Transition(texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    m_list->ResourceBarrier(1, &barrier);
    if (!Submit())
        return DescriptorHeap::kInvalid;

    texture.view = m_descriptors->Allocate();
    if (texture.view == DescriptorHeap::kInvalid)
        return DescriptorHeap::kInvalid;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = mipCount;
    m_device->CreateShaderResourceView(texture.resource.Get(), &srv, m_descriptors->Cpu(texture.view));
    if (m_opacityMicromaps) {
        if (std::optional<omm::AlphaImage> alpha = omm::DecodeAlpha(format, width, height, mips[0]))
            m_alphaImages[texture.view] = std::move(*alpha);
    }
    m_textures.push_back(std::move(texture));
    return m_textures.back().view;
}

int Scene::AddModel(const ModelGeometry& geometry)
{
    const UINT triangles = static_cast<UINT>(geometry.indices.size() / 3);
    const UINT alphaTriangles = triangles - std::min(geometry.opaqueTriangles, triangles);
    const bool uvs = geometry.uvs.size() == geometry.positions.size();
    const bool materials = geometry.triangleMaterials.size() == triangles;
    if (triangles == 0 || geometry.triangleAlbedo.size() != triangles || geometry.alphaTriangles.size() != alphaTriangles || (alphaTriangles != 0 && !uvs))
        return -1;
    m_allocator->Reset();
    m_list->Reset(m_allocator.Get(), nullptr);
    std::vector<ComPtr<ID3D12Resource>> staging;
    Model model;
    model.vertexCount = static_cast<UINT>(geometry.positions.size());
    model.triangleCount = triangles;
    model.opaqueTriangles = triangles - alphaTriangles;
    model.positions = UploadToDefault(geometry.positions.data(), geometry.positions.size_bytes(), staging);
    model.indices = UploadToDefault(geometry.indices.data(), geometry.indices.size_bytes(), staging);
    model.albedo = UploadToDefault(geometry.triangleAlbedo.data(), geometry.triangleAlbedo.size_bytes(), staging);
    if (uvs)
        model.uvs = UploadToDefault(geometry.uvs.data(), geometry.uvs.size_bytes(), staging);
    if (alphaTriangles != 0) {
        std::vector<AlphaTriangle> records(alphaTriangles);
        for (UINT t = 0; t < alphaTriangles; ++t) {
            const uint32_t* triangle = geometry.indices.data() + static_cast<size_t>(model.opaqueTriangles + t) * 3;
            records[t] = {geometry.alphaTriangles[t], {geometry.uvs[triangle[0]], geometry.uvs[triangle[1]], geometry.uvs[triangle[2]]},
                t < geometry.alphaTints.size() ? geometry.alphaTints[t] : 0u};
        }
        model.alpha = UploadToDefault(records.data(), records.size() * sizeof(AlphaTriangle), staging);
    }
    if (materials) {
        model.materials = UploadToDefault(geometry.triangleMaterials.data(), geometry.triangleMaterials.size_bytes(), staging);
        model.selfIllum = std::any_of(geometry.triangleMaterials.begin(), geometry.triangleMaterials.end(),
            [&](uint32_t material) { return material < m_selfIllumMaterials.size() && m_selfIllumMaterials[material]; });
    }
    if (!model.positions || !model.indices || !model.albedo || (uvs && !model.uvs) || (alphaTriangles != 0 && !model.alpha) ||
        (materials && !model.materials)) {
        m_list->Close();
        return -1;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC descs[2];
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = GeometryDescs(model.positions->GetGPUVirtualAddress(), static_cast<UINT>(geometry.positions.size()),
        model.indices->GetGPUVirtualAddress(), triangles, model.opaqueTriangles, descs);
    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC alphaTriangleDesc;
    D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC linkage;
    if (m_opacityMicromaps && alphaTriangles != 0) {
        if (!AddMicromaps(model, geometry, descs[inputs.NumDescs - 1], alphaTriangleDesc, linkage, staging)) {
            m_list->Close();
            return -1;
        }
        // Turning opacity micromaps off in the settings disables them per instance.
        inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS;
    }
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = descs;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    ComPtr<ID3D12Resource> scratch = CreateBuffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    model.blas = CreateBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    if (!scratch || !model.blas) {
        m_list->Close();
        return -1;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
    desc.Inputs = inputs;
    desc.DestAccelerationStructureData = model.blas->GetGPUVirtualAddress();
    desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    m_list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    if (!Submit())
        return -1;

    model.positionsView = m_descriptors->CreateRawView(model.positions.Get());
    model.indicesView = m_descriptors->CreateRawView(model.indices.Get());
    model.albedoView = m_descriptors->CreateRawView(model.albedo.Get());
    if (uvs)
        model.uvsView = m_descriptors->CreateRawView(model.uvs.Get());
    if (alphaTriangles != 0)
        model.alphaView = m_descriptors->CreateRawView(model.alpha.Get());
    if (materials)
        model.materialsView = m_descriptors->CreateRawView(model.materials.Get());
    if (model.positionsView == DescriptorHeap::kInvalid || model.indicesView == DescriptorHeap::kInvalid || model.albedoView == DescriptorHeap::kInvalid ||
        (uvs && model.uvsView == DescriptorHeap::kInvalid) || (alphaTriangles != 0 && model.alphaView == DescriptorHeap::kInvalid) ||
        (materials && model.materialsView == DescriptorHeap::kInvalid)) {
        ReleaseModel(model);
        return -1;
    }
    m_models.push_back(std::move(model));
    return static_cast<int>(m_models.size() - 1);
}

bool Scene::AddMicromaps(Model& model, const ModelGeometry& geometry, D3D12_RAYTRACING_GEOMETRY_DESC& alphaGeometry,
    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles, D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage, std::vector<ComPtr<ID3D12Resource>>& staging)
{
    std::vector<omm::AlphaTriangle> alpha(geometry.alphaTriangles.size());
    for (size_t t = 0; t < alpha.size(); ++t) {
        const uint32_t* triangle = geometry.indices.data() + (model.opaqueTriangles + t) * 3;
        // A Refract tint texture's alpha is no opacity: the shader decides everywhere, and fast alpha tests let rays pass.
        const bool refract = t < geometry.alphaTints.size() && geometry.alphaTints[t] >> 24 == kTintRefract;
        alpha[t] = {refract ? PackTranslucent(DescriptorHeap::kInvalid, 0.25f) : geometry.alphaTriangles[t],
            {geometry.uvs[triangle[0]], geometry.uvs[triangle[1]], geometry.uvs[triangle[2]]}};
    }
    const omm::Micromaps baked = omm::Bake(alpha, m_alphaImages);
    model.micromapIndices = UploadToDefault(baked.indices.data(), baked.indices.size() * sizeof(int32_t), staging);
    if (!model.micromapIndices)
        return false;
    if (!baked.descs.empty()) {
        ComPtr<ID3D12Resource> data = UploadToDefault(baked.data.data(), baked.data.size(), staging);
        ComPtr<ID3D12Resource> descs = UploadToDefault(baked.descs.data(), baked.descs.size() * sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC), staging);
        if (!data || !descs)
            return false;
        staging.push_back(data);
        staging.push_back(descs);
        D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC arrayDesc = {};
        arrayDesc.NumOmmHistogramEntries = static_cast<UINT>(baked.histogram.size());
        arrayDesc.pOmmHistogram = baked.histogram.data();
        arrayDesc.InputBuffer = data->GetGPUVirtualAddress();
        arrayDesc.PerOmmDescs = {descs->GetGPUVirtualAddress(), sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC)};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.NumDescs = 1;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.pOpacityMicromapArrayDesc = &arrayDesc;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
        m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        ComPtr<ID3D12Resource> scratch = CreateBuffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        model.micromaps = CreateBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        if (!scratch || !model.micromaps)
            return false;
        staging.push_back(scratch);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
        build.Inputs = inputs;
        build.DestAccelerationStructureData = model.micromaps->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        m_list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        D3D12_RESOURCE_BARRIER barrier = UavBarrier(model.micromaps.Get());
        m_list->ResourceBarrier(1, &barrier);
    }
    model.hasMicromaps = true;
    Link(model, alphaGeometry, triangles, linkage);
    ++m_micromapModels;
    m_micromapTriangles += static_cast<uint32_t>(std::count_if(baked.indices.begin(), baked.indices.end(), [](int32_t index) { return index >= 0; }));
    m_micromapCount += static_cast<uint32_t>(baked.descs.size());
    return true;
}

void Scene::Link(const Model& model, D3D12_RAYTRACING_GEOMETRY_DESC& alphaGeometry, D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles,
    D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage)
{
    triangles = alphaGeometry.Triangles;
    linkage = {};
    linkage.OpacityMicromapIndexBuffer = {model.micromapIndices->GetGPUVirtualAddress(), sizeof(int32_t)};
    linkage.OpacityMicromapIndexFormat = DXGI_FORMAT_R32_UINT;
    linkage.OpacityMicromapArray = model.micromaps ? model.micromaps->GetGPUVirtualAddress() : 0;
    alphaGeometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
    alphaGeometry.OmmTriangles = {&triangles, &linkage};
}

bool Scene::LinkMicromaps(int model, D3D12_RAYTRACING_GEOMETRY_DESC& alphaGeometry, D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles,
    D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage) const
{
    if (model < 0 || static_cast<size_t>(model) >= m_models.size() || !m_models[model].hasMicromaps)
        return false;
    Link(m_models[model], alphaGeometry, triangles, linkage);
    return true;
}

void Scene::ReleaseModel(Model& model)
{
    m_descriptors->Free(model.positionsView);
    m_descriptors->Free(model.indicesView);
    m_descriptors->Free(model.albedoView);
    m_descriptors->Free(model.uvsView);
    m_descriptors->Free(model.alphaView);
    m_descriptors->Free(model.materialsView);
    model = {};
}

bool Scene::LoadBaked(const bsp::BakedLighting& baked, std::span<const BakedTriangle> triangles, std::span<const std::byte> cachedResidual)
{
    if (baked.samples.empty() || baked.faces.empty() || baked.luxels.empty() || triangles.empty()) {
        log::Info("Scene: the map has no baked light; indirect_lighting 1 traces indirect light instead");
        return false;
    }
    m_allocator->Reset();
    m_list->Reset(m_allocator.Get(), nullptr);
    std::vector<ComPtr<ID3D12Resource>> staging;
    auto upload = [&](const void* data, size_t bytes) {
        // Empty inputs (maps without leaf ambient samples) still get a buffer, so every view is valid.
        static constexpr std::byte kEmpty[16] = {};
        ComPtr<ID3D12Resource> buffer = bytes != 0 ? UploadToDefault(data, bytes, staging) : UploadToDefault(kEmpty, sizeof(kEmpty), staging);
        if (buffer)
            m_bakedInputs.push_back(buffer);
        return buffer ? m_descriptors->CreateRawView(buffer.Get()) : DescriptorHeap::kInvalid;
    };
    // Raw views cover multiples of 16 bytes.
    std::vector<uint32_t> samples(baked.samples);
    samples.resize((samples.size() + 3) & ~size_t(3));
    m_baked.samplesView = upload(samples.data(), samples.size() * sizeof(uint32_t));
    m_baked.facesView = upload(baked.faces.data(), baked.faces.size() * sizeof(bsp::FaceLightmap));
    m_baked.luxelsView = upload(baked.luxels.data(), baked.luxels.size() * sizeof(bsp::BakeLuxel));
    m_baked.neighboursView = upload(baked.neighbours.data(), baked.neighbours.size() * sizeof(bsp::BakeNeighbour));
    std::vector<uint32_t> leafAmbient;
    for (const auto& [first, count] : baked.leafAmbient)
        leafAmbient.insert(leafAmbient.end(), {first, count});
    leafAmbient.resize((leafAmbient.size() + 3) & ~size_t(3));
    m_baked.leafAmbientView = upload(leafAmbient.data(), leafAmbient.size() * sizeof(uint32_t));
    m_baked.ambientLightsView = upload(baked.ambientLights.data(), baked.ambientLights.size() * sizeof(bsp::AmbientLight));
    const UINT trianglesView = upload(triangles.data(), triangles.size() * sizeof(BakedTriangle));
    const UINT64 residualBytes = static_cast<UINT64>(samples.size()) * 8;
    const size_t ambientBytes = baked.ambientSamples.size() * sizeof(bsp::AmbientSample);
    const bool cached = cachedResidual.size() == residualBytes + ambientBytes;
    if (cached) {
        m_bakedFiltered = UploadToDefault(cachedResidual.data(), residualBytes, staging);
        m_bakedResidual = m_bakedFiltered;
        m_baked.ambientSamplesView = upload(cachedResidual.data() + residualBytes, ambientBytes);
    } else {
        m_bakedResidual = CreateBuffer(residualBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_bakedFiltered = CreateBuffer(residualBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // The bake takes the ambient-cube lights out of the samples in place.
        if (ambientBytes != 0) {
            ComPtr<ID3D12Resource> source = UploadToDefault(baked.ambientSamples.data(), ambientBytes, staging);
            m_bakedAmbient = CreateBuffer(ambientBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            if (source && m_bakedAmbient) {
                D3D12_RESOURCE_BARRIER toCopy = Transition(source.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
                m_list->ResourceBarrier(1, &toCopy);
                m_list->CopyBufferRegion(m_bakedAmbient.Get(), 0, source.Get(), 0, ambientBytes);
                D3D12_RESOURCE_BARRIER toWrite = Transition(m_bakedAmbient.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                m_list->ResourceBarrier(1, &toWrite);
                staging.push_back(source);
                m_baked.ambientSamplesView = m_descriptors->CreateRawView(m_bakedAmbient.Get());
                m_baked.ambientSamplesUav = m_descriptors->CreateRawUav(m_bakedAmbient.Get());
            }
        } else {
            m_baked.ambientSamplesView = upload(nullptr, 0);
        }
    }
    m_baked.ambientSampleCount = static_cast<UINT>(baked.ambientSamples.size());
    m_baked.ambientLightCount = static_cast<UINT>(baked.ambientLights.size());
    m_bakedCounters = CreateBuffer(32 + ((baked.faces.size() * 4 + 15) & ~size_t(15)), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_bakedFaces.assign(baked.faces.size(), BakedFaceInfo{});
    std::vector<bool> placed(baked.faces.size(), false);
    for (const bsp::BakeLuxel& luxel : baked.luxels) {
        if (luxel.face < placed.size() && !placed[luxel.face]) {
            placed[luxel.face] = true;
            m_bakedFaces[luxel.face] = {luxel.position, luxel.normal};
        }
    }
    const bool uploaded = m_baked.samplesView != DescriptorHeap::kInvalid && m_baked.facesView != DescriptorHeap::kInvalid &&
        m_baked.luxelsView != DescriptorHeap::kInvalid && m_baked.neighboursView != DescriptorHeap::kInvalid &&
        m_baked.leafAmbientView != DescriptorHeap::kInvalid &&
        m_baked.ambientSamplesView != DescriptorHeap::kInvalid && m_baked.ambientLightsView != DescriptorHeap::kInvalid &&
        (cached || ambientBytes == 0 || m_baked.ambientSamplesUav != DescriptorHeap::kInvalid) && trianglesView != DescriptorHeap::kInvalid;
    if (!uploaded || !m_bakedResidual || !m_bakedFiltered || !m_bakedCounters || !Submit()) {
        log::Info("Scene: uploading the baked light failed; indirect_lighting 1 traces indirect light instead");
        m_descriptors->Free(trianglesView);
        return false;
    }
    m_baked.filteredView = m_descriptors->CreateRawView(m_bakedFiltered.Get());
    if (!cached) {
        m_baked.residualView = m_descriptors->CreateRawView(m_bakedResidual.Get());
        m_baked.residualUav = m_descriptors->CreateRawUav(m_bakedResidual.Get());
        m_baked.filteredUav = m_descriptors->CreateRawUav(m_bakedFiltered.Get());
        m_baked.countersUav = m_descriptors->CreateRawUav(m_bakedCounters.Get());
    }
    m_baked.luxelCount = static_cast<UINT>(baked.luxels.size());
    m_baked.leafCount = static_cast<UINT>(baked.leafAmbient.size());
    m_baked.trianglesView = trianglesView;
    if (cached) {
        m_baked.stage = BakedBuffers::Stage::Gather;
        m_baked.filtered = true;
        log::Info("Scene: the baked light's residual was loaded from the cache");
    }
    log::Info("Scene: baked light ({}): {} lightmapped faces, {} luxels to bake, {} leaf ambient samples, {} surface lights in them", baked.hdr ? "HDR" : "LDR",
        baked.faces.size(), baked.luxels.size(), baked.ambientSamples.size(), baked.ambientLights.size());
    return true;
}

void Scene::Clear()
{
    for (UINT* view : {&m_baked.samplesView, &m_baked.facesView, &m_baked.trianglesView, &m_baked.luxelsView, &m_baked.neighboursView, &m_baked.leafAmbientView,
             &m_baked.ambientSamplesView, &m_baked.ambientSamplesUav, &m_baked.ambientLightsView, &m_baked.residualView, &m_baked.residualUav, &m_baked.filteredView, &m_baked.filteredUav, &m_baked.countersUav}) {
        if (*view != DescriptorHeap::kInvalid)
            m_descriptors->Free(*view);
    }
    m_baked = {};
    m_bakedInputs.clear();
    m_bakedResidual.Reset();
    m_bakedFiltered.Reset();
    m_bakedCounters.Reset();
    m_bakedAmbient.Reset();
    m_bakedFaces.clear();
    for (Model& model : m_models)
        ReleaseModel(model);
    m_models.clear();
    for (Texture& texture : m_textures)
        m_descriptors->Free(texture.view);
    m_textures.clear();
    m_alphaImages.clear();
    m_micromapTriangles = 0;
    m_micromapCount = 0;
    m_micromapModels = 0;
    m_materialCount = 0;
    m_selfIllumMaterials.clear();
    m_staticInstances.clear();
    m_descriptors->Free(m_emittersView);
    m_emittersView = DescriptorHeap::kInvalid;
    m_emitters.Reset();
    m_emitterCount = 0;
    m_emitterPower = 0.0f;
    for (UINT view : m_emitterLightViews)
        m_descriptors->Free(view);
    m_emitterLightViews.clear();
    m_emitterLights.clear();
    m_worldFlags = 0;
    m_hasSkybox = false;
    m_worldModel = -1;
    m_name.clear();
    m_sun = {};
    m_descriptors->Free(m_lightsView);
    m_lightsView = DescriptorHeap::kInvalid;
    m_lights.Reset();
    m_lightCount = 0;
    m_descriptors->Free(m_lightLists.rangesView);
    m_descriptors->Free(m_lightLists.indicesView);
    m_descriptors->Free(m_lightLists.nodesView);
    m_descriptors->Free(m_lightLists.leafClustersView);
    m_descriptors->Free(m_lightLists.sunClustersView);
    m_descriptors->Free(m_lightLists.gridCellsView);
    m_descriptors->Free(m_lightLists.gridIndicesView);
    m_descriptors->Free(m_lightLists.gridTailView);
    m_lightLists = {};
    m_lightRanges.Reset();
    m_lightIndices.Reset();
    m_gridCells.Reset();
    m_gridIndices.Reset();
    m_gridTail.Reset();
    m_bspNodes.Reset();
    m_leafClusters.Reset();
    m_sunClusters.Reset();
}

bool Scene::LoadMap(const std::string& name, const bsp::WorldGeometry& world, std::span<const MapLightShape> lightShapes, const ModelGeometry& worldGeometry,
    std::span<const GpuEmitter> emitters, bool worldEmits, float emitterPower)
{
    m_emitterPower = emitterPower;
    m_worldModel = AddModel(worldGeometry);
    if (m_worldModel < 0)
        return false;

    std::vector<GpuLight> lights;
    for (size_t i = 0; i < world.lights.size(); ++i) {
        const bsp::Light& light = world.lights[i];
        const MapLightShape shape = i < lightShapes.size() ? lightShapes[i] : MapLightShape{};
        GpuLight& gpu = lights.emplace_back();
        // VRAD's lightmaps never applied a light's _distance (radius): GatherSampleStandardLightSSE in
        // src/utils/vrad/lightmap.cpp ignores it, and only leaf_ambient_lighting.cpp reads it. It is left out.
        gpu = {{light.origin.x, light.origin.y, light.origin.z}, static_cast<uint32_t>(light.type), {light.intensity.x, light.intensity.y, light.intensity.z},
            0.0f, {light.normal.x, light.normal.y, light.normal.z}, light.stopdot, light.stopdot2, light.exponent, light.constantAttn,
            light.linearAttn, light.quadraticAttn, shape.flags, shape.radius, static_cast<uint32_t>(light.style), ReferenceScale(light), {}};
    }
    if (!lights.empty() && !BuildLightLists(world.lights, world.visibility))
        return false;
    if ((!lights.empty() || !emitters.empty()) && !BuildLightGrid(lights, world.lights, emitters, world.visibility, worldGeometry.positions))
        return false;
    if (!UploadEmitters(emitters, worldEmits))
        log::Info("Scene: the {} self-illuminated triangles could not be uploaded; glowing surfaces light only what indirect rays meet", emitters.size());
    else if (!emitters.empty())
        log::Info("Scene: {} self-illuminated triangles light the scene", emitters.size());
    if (!lights.empty()) {
        m_allocator->Reset();
        m_list->Reset(m_allocator.Get(), nullptr);
        std::vector<ComPtr<ID3D12Resource>> staging;
        m_lights = UploadToDefault(lights.data(), lights.size() * sizeof(GpuLight), staging);
        if (!m_lights || !Submit())
            return false;
        m_lightsView = m_descriptors->CreateRawView(m_lights.Get());
        m_lightCount = static_cast<UINT>(lights.size());
    }
    m_name = name;
    m_sun = world.sun;
    return true;
}

bool Scene::BuildLightLists(std::span<const bsp::Light> lights, const bsp::Visibility& visibility)
{
    const UINT clusterCount = visibility.nodes.empty() ? 0 : visibility.clusterCount;
    std::vector<bsp::Float3> low(clusterCount, {FLT_MAX, FLT_MAX, FLT_MAX});
    std::vector<bsp::Float3> high(clusterCount, {-FLT_MAX, -FLT_MAX, -FLT_MAX});
    for (const bsp::BspLeaf& leaf : visibility.leaves) {
        if (leaf.cluster < 0 || static_cast<UINT>(leaf.cluster) >= clusterCount)
            continue;
        bsp::Float3& lo = low[leaf.cluster];
        bsp::Float3& hi = high[leaf.cluster];
        lo = {std::min(lo.x, leaf.mins.x), std::min(lo.y, leaf.mins.y), std::min(lo.z, leaf.mins.z)};
        hi = {std::max(hi.x, leaf.maxs.x), std::max(hi.y, leaf.maxs.y), std::max(hi.z, leaf.maxs.z)};
    }

    std::vector<uint32_t> ranges;
    std::vector<uint32_t> indices;
    ranges.reserve((clusterCount + 1) * 2);
    std::vector<float> lightRanges(lights.size());
    for (size_t i = 0; i < lights.size(); ++i)
        lightRanges[i] = LightRange(lights[i]);
    size_t maxPerCluster = 0;
    for (UINT c = 0; c < clusterCount; ++c) {
        const size_t first = indices.size();
        for (uint32_t i = 0; i < lights.size(); ++i) {
            const bsp::Light& light = lights[i];
            if (!visibility.Sees(light.cluster, static_cast<int32_t>(c)))
                continue;
            // Distance from the light to the cluster's bounds.
            const float d[3] = {std::max({low[c].x - light.origin.x, 0.0f, light.origin.x - high[c].x}),
                std::max({low[c].y - light.origin.y, 0.0f, light.origin.y - high[c].y}),
                std::max({low[c].z - light.origin.z, 0.0f, light.origin.z - high[c].z})};
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= lightRanges[i] * lightRanges[i])
                indices.push_back(i);
        }
        ranges.push_back(static_cast<uint32_t>(first));
        ranges.push_back(static_cast<uint32_t>(indices.size() - first));
        maxPerCluster = std::max(maxPerCluster, indices.size() - first);
    }
    const size_t binned = indices.size();
    ranges.push_back(static_cast<uint32_t>(indices.size()));
    ranges.push_back(static_cast<uint32_t>(lights.size()));
    for (uint32_t i = 0; i < lights.size(); ++i)
        indices.push_back(i);

    std::vector<int32_t> leafClusters;
    for (const bsp::BspLeaf& leaf : visibility.leaves)
        leafClusters.push_back(leaf.cluster);
    if (clusterCount == 0) {
        leafClusters.assign(1, -1);
    }

    m_allocator->Reset();
    m_list->Reset(m_allocator.Get(), nullptr);
    std::vector<ComPtr<ID3D12Resource>> staging;
    m_lightRanges = UploadToDefault(ranges.data(), ranges.size() * sizeof(uint32_t), staging);
    m_lightIndices = UploadToDefault(indices.data(), indices.size() * sizeof(uint32_t), staging);
    m_leafClusters = UploadToDefault(leafClusters.data(), leafClusters.size() * sizeof(int32_t), staging);
    if (clusterCount > 0)
        m_bspNodes = UploadToDefault(visibility.nodes.data(), visibility.nodes.size() * sizeof(bsp::BspNode), staging);
    std::vector<uint8_t> sunClusters = visibility.sunClusters;
    sunClusters.resize((sunClusters.size() + 15) & ~size_t(15));
    if (!sunClusters.empty())
        m_sunClusters = UploadToDefault(sunClusters.data(), sunClusters.size(), staging);
    if (!m_lightRanges || !m_lightIndices || !m_leafClusters || (clusterCount > 0 && !m_bspNodes) || !Submit())
        return false;
    LightLists lists;
    lists.clusterCount = clusterCount;
    lists.rangesView = m_descriptors->CreateRawView(m_lightRanges.Get());
    lists.indicesView = m_descriptors->CreateRawView(m_lightIndices.Get());
    lists.leafClustersView = m_descriptors->CreateRawView(m_leafClusters.Get());
    lists.nodesView = clusterCount > 0 ? m_descriptors->CreateRawView(m_bspNodes.Get()) : DescriptorHeap::kInvalid;
    lists.sunClustersView = m_sunClusters ? m_descriptors->CreateRawView(m_sunClusters.Get()) : DescriptorHeap::kInvalid;
    m_lightLists = lists;
    if (lists.rangesView == DescriptorHeap::kInvalid || lists.indicesView == DescriptorHeap::kInvalid ||
        lists.leafClustersView == DescriptorHeap::kInvalid || (clusterCount > 0 && lists.nodesView == DescriptorHeap::kInvalid))
        return false;
    if (clusterCount == 0)
        log::Info("Scene: the map has no visibility data; every point is lit by all {} lights", lights.size());
    else
        log::Info("Scene: {} visibility clusters see {:.1f} lights on average, {} at most", clusterCount,
            static_cast<double>(binned) / clusterCount, maxPerCluster);
    return true;
}

bool Scene::BuildLightGrid(std::span<const GpuLight> lights, std::span<const bsp::Light> mapLights, std::span<const GpuEmitter> emitters,
    const bsp::Visibility& visibility, std::span<const bsp::Float3> worldPositions)
{
    bsp::Float3 low = {0.0f, 0.0f, 0.0f};
    bsp::Float3 high = {0.0f, 0.0f, 0.0f};
    if (!worldPositions.empty()) {
        low = high = worldPositions[0];
        for (const bsp::Float3& p : worldPositions) {
            low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
    }
    const float extent[3] = {std::max(high.x - low.x, 1.0f), std::max(high.y - low.y, 1.0f), std::max(high.z - low.z, 1.0f)};
    const double cells = std::clamp(kGridTailEntries / static_cast<double>(std::max<size_t>(lights.size(), 1)), 1.0, kGridCells);
    const float size = std::max(kGridCellMin, static_cast<float>(std::cbrt(static_cast<double>(extent[0]) * extent[1] * extent[2] / cells)));
    const uint32_t dims[3] = {static_cast<uint32_t>(std::ceil(extent[0] / size)), static_cast<uint32_t>(std::ceil(extent[1] / size)),
        static_cast<uint32_t>(std::ceil(extent[2] / size))};
    const size_t cellCount = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    const auto cellRange = [&](float minimum, float maximum, float origin, uint32_t count) {
        const auto first = static_cast<int64_t>(std::floor((minimum - origin) / size));
        const auto last = static_cast<int64_t>(std::floor((maximum - origin) / size));
        return std::pair<uint32_t, uint32_t>{static_cast<uint32_t>(std::clamp<int64_t>(first, 0, count - 1)),
            static_cast<uint32_t>(std::clamp<int64_t>(last, 0, count - 1))};
    };

    // The clusters of the leaves each cell overlaps, and the clusters each light's cluster sees, for the PVS weight.
    std::vector<std::vector<int32_t>> cellClusters(cellCount);
    for (const bsp::BspLeaf& leaf : visibility.leaves) {
        if (leaf.cluster < 0)
            continue;
        const auto [x0, x1] = cellRange(leaf.mins.x, leaf.maxs.x, low.x, dims[0]);
        const auto [y0, y1] = cellRange(leaf.mins.y, leaf.maxs.y, low.y, dims[1]);
        const auto [z0, z1] = cellRange(leaf.mins.z, leaf.maxs.z, low.z, dims[2]);
        for (uint32_t z = z0; z <= z1; ++z)
            for (uint32_t y = y0; y <= y1; ++y)
                for (uint32_t x = x0; x <= x1; ++x)
                    cellClusters[(static_cast<size_t>(z) * dims[1] + y) * dims[0] + x].push_back(leaf.cluster);
    }
    for (std::vector<int32_t>& clusters : cellClusters) {
        std::sort(clusters.begin(), clusters.end());
        clusters.erase(std::unique(clusters.begin(), clusters.end()), clusters.end());
    }
    std::vector<std::vector<bool>> sees;
    if (!visibility.pvs.empty()) {
        sees.resize(lights.size());
        for (size_t i = 0; i < lights.size(); ++i) {
            sees[i].resize(visibility.clusterCount);
            for (uint32_t c = 0; c < visibility.clusterCount; ++c)
                sees[i][c] = visibility.Sees(mapLights[i].cluster, static_cast<int32_t>(c));
        }
    }

    struct CellLights {
        std::vector<uint32_t> list;
        std::vector<std::pair<uint32_t, float>> tail;
        std::vector<std::pair<uint32_t, float>> emitterTail;
        float emitterRest = 0.0f;
    };
    // Each emitter's share of all emitters' power (GpuEmitter::cdf).
    std::vector<float> emitterPower(emitters.size());
    for (size_t e = 0; e < emitters.size(); ++e)
        emitterPower[e] = emitters[e].cdf - (e > 0 ? emitters[e - 1].cdf : 0.0f);
    // (index, cumulative weight up to 1) of each weighted entry from `first` on.
    const auto cumulativeTail = [](const std::vector<std::pair<float, uint32_t>>& weighted, size_t first, std::vector<std::pair<uint32_t, float>>& tail) {
        double total = 0.0;
        for (size_t k = first; k < weighted.size(); ++k)
            total += weighted[k].first;
        double cumulative = 0.0;
        for (size_t k = first; k < weighted.size(); ++k) {
            cumulative += weighted[k].first;
            tail.emplace_back(weighted[k].second, static_cast<float>(cumulative / total));
        }
        if (!tail.empty())
            tail.back().second = 1.0f;
    };
    std::vector<CellLights> cellLights(cellCount);
    std::vector<size_t> order(cellCount);
    std::iota(order.begin(), order.end(), size_t{0});
    std::for_each(std::execution::par, order.begin(), order.end(), [&](size_t c) {
        const uint32_t x = static_cast<uint32_t>(c % dims[0]);
        const uint32_t y = static_cast<uint32_t>(c / dims[0] % dims[1]);
        const uint32_t z = static_cast<uint32_t>(c / (static_cast<size_t>(dims[0]) * dims[1]));
        const bsp::Float3 cellLow = {low.x + x * size, low.y + y * size, low.z + z * size};
        const bsp::Float3 cellHigh = {cellLow.x + size, cellLow.y + size, cellLow.z + size};
        std::vector<std::pair<float, uint32_t>> ranked;
        for (uint32_t i = 0; i < lights.size(); ++i) {
            const float bound = LightBound(lights[i], cellLow, cellHigh);
            if (bound <= 0.0f)
                continue;
            const int32_t from = mapLights[i].cluster;
            const bool visible = sees.empty() || from < 0 || cellClusters[c].empty() ||
                std::any_of(cellClusters[c].begin(), cellClusters[c].end(), [&](int32_t to) { return sees[i][to]; });
            ranked.emplace_back(visible ? bound : bound * kHiddenWeight, visible ? i : i | kHiddenLight);
        }
        const size_t listed = std::min(ranked.size(), kGridListLights);
        std::partial_sort(ranked.begin(), ranked.begin() + listed, ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        CellLights& result = cellLights[c];
        for (size_t k = 0; k < listed; ++k)
            result.list.push_back(ranked[k].second);
        for (size_t k = listed; k < ranked.size(); ++k)
            ranked[k].second &= ~kHiddenLight;
        cumulativeTail(ranked, listed, result.tail);
        // The emitters with the largest bound, in the order of their indices, with cumulative weights that leave the
        // others' share of the summed bound (`emitterRest`) to the global distribution (SampleEmitter).
        std::vector<std::pair<float, uint32_t>> glowing;
        double glowTotal = 0.0;
        for (uint32_t e = 0; e < emitters.size(); ++e) {
            const float bound = EmitterBound(emitters[e], emitterPower[e], cellLow, cellHigh);
            if (bound > 0.0f) {
                glowing.emplace_back(bound, e);
                glowTotal += bound;
            }
        }
        const size_t kept = std::min(glowing.size(), kGridEmitters);
        std::partial_sort(glowing.begin(), glowing.begin() + kept, glowing.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        glowing.resize(kept);
        std::sort(glowing.begin(), glowing.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        double cumulative = 0.0;
        for (const auto& [bound, e] : glowing) {
            cumulative += bound;
            result.emitterTail.emplace_back(e, static_cast<float>(cumulative / glowTotal));
        }
        result.emitterRest = glowTotal > 0.0 ? static_cast<float>(1.0 - cumulative / glowTotal) : 0.0f;
        if (result.emitterRest < 1e-6f && !result.emitterTail.empty()) {
            result.emitterRest = 0.0f;
            result.emitterTail.back().second = 1.0f;
        }
    });

    std::vector<uint32_t> cellRanges;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> tail;
    cellRanges.reserve(cellCount * 8);
    size_t maxListed = 0;
    size_t emitterTotal = 0;
    for (const CellLights& cell : cellLights) {
        const auto tailFirst = static_cast<uint32_t>(tail.size() / 2);
        for (const auto& [light, cdf] : cell.tail)
            tail.insert(tail.end(), {light, std::bit_cast<uint32_t>(cdf)});
        const auto emitterFirst = static_cast<uint32_t>(tail.size() / 2);
        for (const auto& [emitter, cdf] : cell.emitterTail)
            tail.insert(tail.end(), {emitter, std::bit_cast<uint32_t>(cdf)});
        cellRanges.insert(cellRanges.end(), {static_cast<uint32_t>(indices.size()), static_cast<uint32_t>(cell.list.size()), tailFirst,
                                                static_cast<uint32_t>(cell.tail.size()), emitterFirst, static_cast<uint32_t>(cell.emitterTail.size()),
                                                std::bit_cast<uint32_t>(cell.emitterRest), 0u});
        indices.insert(indices.end(), cell.list.begin(), cell.list.end());
        maxListed = std::max(maxListed, cell.list.size());
        emitterTotal += cell.emitterTail.size();
    }
    const size_t listedTotal = indices.size();
    const size_t tailTotal = tail.size() / 2 - emitterTotal;
    // Buffers may not be empty.
    if (indices.empty())
        indices.push_back(0);
    if (tail.empty())
        tail.insert(tail.end(), {0u, 0u});

    m_allocator->Reset();
    m_list->Reset(m_allocator.Get(), nullptr);
    std::vector<ComPtr<ID3D12Resource>> staging;
    m_gridCells = UploadToDefault(cellRanges.data(), cellRanges.size() * sizeof(uint32_t), staging);
    m_gridIndices = UploadToDefault(indices.data(), indices.size() * sizeof(uint32_t), staging);
    m_gridTail = UploadToDefault(tail.data(), tail.size() * sizeof(uint32_t), staging);
    if (!m_gridCells || !m_gridIndices || !m_gridTail || !Submit())
        return false;
    m_lightLists.gridCellsView = m_descriptors->CreateRawView(m_gridCells.Get());
    m_lightLists.gridIndicesView = m_descriptors->CreateRawView(m_gridIndices.Get());
    m_lightLists.gridTailView = m_descriptors->CreateRawView(m_gridTail.Get());
    m_lightLists.gridOrigin = low;
    m_lightLists.gridCellSize = size;
    std::copy_n(dims, 3, m_lightLists.gridDims);
    if (m_lightLists.gridCellsView == DescriptorHeap::kInvalid || m_lightLists.gridIndicesView == DescriptorHeap::kInvalid ||
        m_lightLists.gridTailView == DescriptorHeap::kInvalid)
        return false;
    log::Info("Scene: light grid of {}x{}x{} cells of {:.0f} units; cells list {:.1f} lights on average, {} at most, and sample {:.1f} more lights and "
              "{:.1f} glowing triangles",
        dims[0], dims[1], dims[2], size, static_cast<double>(listedTotal) / cellCount, maxListed, static_cast<double>(tailTotal) / cellCount,
        static_cast<double>(emitterTotal) / cellCount);
    return true;
}

bool Scene::EnsureDynamicLights(FrameTlas& frame)
{
    if (frame.dynamicLights)
        return true;
    frame.dynamicLights = CreateBuffer(sizeof(GpuLight) * kMaxDynamicLights, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!frame.dynamicLights || FAILED(frame.dynamicLights->Map(0, nullptr, reinterpret_cast<void**>(&frame.mappedDynamicLights)))) {
        frame.dynamicLights.Reset();
        return false;
    }
    frame.dynamicLightsView = m_descriptors->CreateRawView(frame.dynamicLights.Get());
    return frame.dynamicLightsView != DescriptorHeap::kInvalid;
}

bool Scene::EnsureLightStyles(FrameTlas& frame)
{
    if (frame.lightStyles)
        return true;
    frame.lightStyles = CreateBuffer(sizeof(m_lightStyles), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!frame.lightStyles || FAILED(frame.lightStyles->Map(0, nullptr, reinterpret_cast<void**>(&frame.mappedLightStyles)))) {
        frame.lightStyles.Reset();
        return false;
    }
    frame.lightStylesView = m_descriptors->CreateRawView(frame.lightStyles.Get());
    return frame.lightStylesView != DescriptorHeap::kInvalid;
}

bool Scene::EnsureFrameCapacity(FrameTlas& frame, UINT count)
{
    if (frame.tlas && frame.capacity >= count)
        return true;
    const UINT capacity = std::max<UINT>(count * 2, 256);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = TlasInputs(capacity, 0);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    const UINT recordsView = frame.recordsView != DescriptorHeap::kInvalid ? frame.recordsView : m_descriptors->Allocate();
    // Views drawn into textures may still trace the TLAS being replaced (Lighting::ForwardScene of an earlier view).
    if (frame.tlas) {
        m_queue->Signal(m_fence.Get(), ++m_fenceValue);
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    frame.instances.Reset();
    frame.records.Reset();
    frame.scratch.Reset();
    frame.tlas.Reset();
    frame.recordsView = recordsView;
    frame.instances = CreateBuffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * capacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    frame.records = CreateBuffer(sizeof(InstanceRecord) * capacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    frame.scratch = CreateBuffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    frame.tlas = CreateBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    if (recordsView == DescriptorHeap::kInvalid || !frame.instances || !frame.records || !frame.scratch || !frame.tlas ||
        FAILED(frame.instances->Map(0, nullptr, reinterpret_cast<void**>(&frame.mapped))) ||
        FAILED(frame.records->Map(0, nullptr, reinterpret_cast<void**>(&frame.mappedRecords)))) {
        frame.tlas.Reset();
        frame.capacity = 0;
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = capacity * sizeof(InstanceRecord) / 4;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    m_device->CreateShaderResourceView(frame.records.Get(), &srv, m_descriptors->Cpu(recordsView));
    frame.capacity = capacity;
    return true;
}

std::optional<Scene::ModelBuffers> Scene::Buffers(int model) const
{
    if (model < 0 || static_cast<size_t>(model) >= m_models.size())
        return std::nullopt;
    const Model& m = m_models[model];
    return ModelBuffers{m.positionsView, m.indices->GetGPUVirtualAddress(), m.vertexCount, m.triangleCount, m.opaqueTriangles};
}

void Scene::SetStaticInstances(std::vector<SceneInstance> instances)
{
    m_staticInstances = std::move(instances);
    m_hasSkybox = std::any_of(m_staticInstances.begin(), m_staticInstances.end(), [](const SceneInstance& instance) { return instance.mask == kSkyboxMask; });
}

bool Scene::UploadEmitters(std::span<const GpuEmitter> emitters, bool worldEmits)
{
    m_worldFlags = worldEmits ? kInstanceEmitter : 0u;
    if (emitters.empty())
        return true;
    m_allocator->Reset();
    m_list->Reset(m_allocator.Get(), nullptr);
    std::vector<ComPtr<ID3D12Resource>> staging;
    m_emitters = UploadToDefault(emitters.data(), emitters.size_bytes(), staging);
    if (!m_emitters || !Submit())
        return false;
    m_emittersView = m_descriptors->CreateRawView(m_emitters.Get());
    m_emitterCount = m_emittersView != DescriptorHeap::kInvalid ? static_cast<UINT>(emitters.size()) : 0u;
    if (m_emitterCount == 0)
        return false;
    // Rays that meet a glowing triangle find its emitter (RTXDI's BRDF samples, shaders/nvidia/rtxdi_bridge.hlsli).
    std::vector<std::vector<uint32_t>> tables;
    for (uint32_t e = 0; e < emitters.size(); ++e) {
        const GpuEmitter& emitter = emitters[e];
        if (emitter.instance >= tables.size())
            tables.resize(emitter.instance + 1);
        std::vector<uint32_t>& table = tables[emitter.instance];
        if (emitter.primitive >= table.size())
            table.resize(emitter.primitive + 1, 0u);
        table[emitter.primitive] = e + 1;
    }
    m_allocator->Reset();
    m_list->Reset(m_allocator.Get(), nullptr);
    std::vector<ComPtr<ID3D12Resource>> tableStaging;
    m_emitterLights.assign(tables.size(), nullptr);
    m_emitterLightViews.assign(tables.size(), DescriptorHeap::kInvalid);
    for (size_t i = 0; i < tables.size(); ++i) {
        if (!tables[i].empty())
            m_emitterLights[i] = UploadToDefault(tables[i].data(), tables[i].size() * sizeof(uint32_t), tableStaging);
    }
    if (!Submit())
        return false;
    for (size_t i = 0; i < tables.size(); ++i) {
        if (m_emitterLights[i])
            m_emitterLightViews[i] = m_descriptors->CreateRawView(m_emitterLights[i].Get());
    }
    return true;
}

SceneFrame Scene::RecordFrame(ID3D12GraphicsCommandList4* list, UINT slot, std::span<const SceneInstance> dynamic, bool micromaps)
{
    static_assert(sizeof(InstanceRecord) == 96);
    FrameTlas& frame = m_frames[slot % kFrameSlots];
    const UINT count = static_cast<UINT>(1 + m_staticInstances.size() + dynamic.size());
    if (!EnsureFrameCapacity(frame, count))
        return {};

    UINT written = 0;
    m_recorded.clear();
    // `staticIndex`: 0 for the world, i + 1 for static instance i, none for dynamic ones.
    auto write = [&](const SceneInstance& source, const float (*transform)[4], UINT mask, std::optional<size_t> staticIndex) {
        if (source.model < 0 || static_cast<size_t>(source.model) >= m_models.size())
            return;
        const Model& model = m_models[source.model];
        D3D12_GPU_VIRTUAL_ADDRESS blas = source.blas != 0 ? source.blas : model.blas->GetGPUVirtualAddress();
        D3D12_RAYTRACING_INSTANCE_DESC& instance = frame.mapped[written];
        instance = {};
        if (transform != nullptr)
            std::memcpy(instance.Transform, transform, sizeof(instance.Transform));
        else
            instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
        instance.InstanceID = written;
        instance.InstanceMask = mask;
        const bool dynamicModel = (mask & (kDynamicMask | kLocalPlayerMask)) != 0;
        instance.Flags = dynamicModel ? D3D12_RAYTRACING_INSTANCE_FLAG_NONE : D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        if (model.hasMicromaps && !micromaps)
            instance.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS;
        instance.AccelerationStructure = blas;
        InstanceRecord& record = frame.mappedRecords[written];
        record = {source.positionsView != DescriptorHeap::kInvalid ? source.positionsView : model.positionsView, model.indicesView, model.albedoView,
            model.uvsView, model.alphaView, model.opaqueTriangles, model.materialsView, source.previousPositionsView, {},
            source.flags | (model.selfIllum ? kInstanceSelfIllum : 0u) | (dynamicModel ? kInstanceModel : 0u) | (model.character ? kInstanceCharacter : 0u),
            staticIndex && *staticIndex < m_emitterLightViews.size() ? m_emitterLightViews[*staticIndex] : DescriptorHeap::kInvalid, {}};
        std::memcpy(record.previousTransform, source.previousKnown ? source.previousTransform : instance.Transform, sizeof(record.previousTransform));
        m_recorded.emplace_back(source.model, mask);
        ++written;
    };
    SceneInstance world;
    world.model = m_worldModel;
    world.flags = m_worldFlags;
    write(world, nullptr, kWorldMask, 0);
    for (size_t i = 0; i < m_staticInstances.size(); ++i)
        write(m_staticInstances[i], m_staticInstances[i].transform, m_staticInstances[i].mask != 0 ? m_staticInstances[i].mask : kStaticPropMask, i + 1);
    for (const SceneInstance& instance : dynamic)
        write(instance, instance.transform, instance.mask != 0 ? instance.mask : kDynamicMask, std::nullopt);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
    desc.Inputs = TlasInputs(written, frame.instances->GetGPUVirtualAddress());
    desc.DestAccelerationStructureData = frame.tlas->GetGPUVirtualAddress();
    desc.ScratchAccelerationStructureData = frame.scratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier = UavBarrier(frame.tlas.Get());
    list->ResourceBarrier(1, &barrier);

    SceneFrame result{frame.tlas->GetGPUVirtualAddress(), frame.recordsView};
    if (!m_dynamicLights.empty() && EnsureDynamicLights(frame)) {
        for (const engine::DynamicLight& light : m_dynamicLights) {
            if (result.dynamicLightCount == kMaxDynamicLights || light.radius <= 0.0f)
                continue;
            GpuLight& gpu = frame.mappedDynamicLights[result.dynamicLightCount++];
            const bool spot = light.outerAngle > 0.0f;
            gpu = {{light.origin[0], light.origin[1], light.origin[2]}, kDynamicLightType, {light.color[0], light.color[1], light.color[2]},
                light.radius, {light.direction[0], light.direction[1], light.direction[2]}, spot ? std::cos(light.innerAngle * kDegreesToRadians) : 0.0f,
                spot ? std::cos(light.outerAngle * kDegreesToRadians) : -1.0f, spot ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f, 0u, 0.0f, kNoLightStyle, 0.0f, {}};
        }
        result.dynamicLightsView = frame.dynamicLightsView;
    }
    if (EnsureLightStyles(frame)) {
        std::memcpy(frame.mappedLightStyles, m_lightStyles.data(), sizeof(m_lightStyles));
        result.lightStylesView = frame.lightStylesView;
    }
    return result;
}

}
