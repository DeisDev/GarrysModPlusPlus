#include "viewmodel_casters.h"

#include "log.h"
#include "options.h"
#include "scene.h"
#include "settings.h"
#include "shaderapi_device.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace gmodpp::viewmodel_casters {

namespace {

using Microsoft::WRL::ComPtr;

// Rays that hit the viewmodel see a mid-grey surface (only its own indirect light and reflections meet it).
constexpr std::uint32_t kGreyAlbedo = 0xFF595959u;

struct Entry {
    ComPtr<ID3D12Resource> positions;
    UINT positionsView = DescriptorHeap::kInvalid;
    UINT positionsUav = DescriptorHeap::kInvalid;
    ComPtr<ID3D12Resource> uploadedIndices;
    std::uint32_t* mappedIndices = nullptr;
    ComPtr<ID3D12Resource> indices;
    UINT indicesView = DescriptorHeap::kInvalid;
    ComPtr<ID3D12Resource> blas;
    ComPtr<ID3D12Resource> scratch;
    ID3D12Fence* fence = nullptr;
    std::uint64_t fenceValue = 0;
    UINT vertexCount = 0;
    UINT triangleCount = 0;
    // Each vertex buffer's first vertex, and the index lists already recorded (a mesh drawn by several passes).
    std::unordered_map<std::uint64_t, UINT> placed;
    std::unordered_set<std::uint64_t> recorded;
};

struct State {
    bool initialized = false;
    bool failed = false;
    bool recording = false;
    bool loggedSkip = false;
    bool loggedFull = false;
    UINT loggedVertices = 0;
    UINT loggedTriangles = 0;
    UINT current = 0;
    std::array<Entry, Scene::kFrameSlots> entries;
    ComPtr<ID3D12Resource> albedo;
    UINT albedoView = DescriptorHeap::kInvalid;
};

State g_state;

std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) { return (hash ^ value) * 0x100000001B3ull; }

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    const D3D12_HEAP_PROPERTIES heap = {type};
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer))))
        return nullptr;
    return buffer;
}

bool Initialize()
{
    native::Backend& backend = native::GetBackend();
    ComPtr<ID3D12Device5> device;
    if (FAILED(backend.Device()->QueryInterface(IID_PPV_ARGS(&device))))
        return false;
    DescriptorHeap& descriptors = backend.Descriptors();
    // A BLAS of the most triangles is as large as any smaller one (D3D12 raytracing spec, prebuild info).
    D3D12_RAYTRACING_GEOMETRY_DESC geometry[2] = {};
    const UINT count = Scene::GeometryDescs(0, kMaxVertices, 0, kMaxTriangles, kMaxTriangles, geometry);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = count;
    inputs.pGeometryDescs = geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    if (prebuild.ResultDataMaxSizeInBytes == 0)
        return false;
    for (Entry& entry : g_state.entries) {
        entry.positions = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, kMaxVertices * 12ull, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON);
        entry.uploadedIndices = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, kMaxTriangles * 12ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        entry.indices = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, kMaxTriangles * 12ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        entry.blas = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, prebuild.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        entry.scratch = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, std::max<UINT64>(prebuild.ScratchDataSizeInBytes, 256),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!entry.positions || !entry.uploadedIndices || !entry.indices || !entry.blas || !entry.scratch)
            return false;
        void* mapped = nullptr;
        if (FAILED(entry.uploadedIndices->Map(0, nullptr, &mapped)))
            return false;
        entry.mappedIndices = static_cast<std::uint32_t*>(mapped);
        entry.positionsView = descriptors.CreateRawView(entry.positions.Get());
        entry.positionsUav = descriptors.CreateRawUav(entry.positions.Get());
        entry.indicesView = descriptors.CreateRawView(entry.indices.Get());
        if (entry.positionsView == DescriptorHeap::kInvalid || entry.positionsUav == DescriptorHeap::kInvalid || entry.indicesView == DescriptorHeap::kInvalid)
            return false;
    }
    g_state.albedo = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, kMaxTriangles * 4ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* albedo = nullptr;
    if (!g_state.albedo || FAILED(g_state.albedo->Map(0, nullptr, &albedo)))
        return false;
    std::uint32_t* grey = static_cast<std::uint32_t*>(albedo);
    std::fill(grey, grey + kMaxTriangles, kGreyAlbedo);
    g_state.albedo->Unmap(0, nullptr);
    g_state.albedoView = descriptors.CreateRawView(g_state.albedo.Get());
    return g_state.albedoView != DescriptorHeap::kInvalid;
}

}

void BeginCapture()
{
    g_state.recording = false;
    const Settings& settings = GetSettings();
    if (!settings.viewmodelSelfShadows || !settings.realtimeLighting || g_state.failed)
        return;
    if (!std::exchange(g_state.initialized, true) && !Initialize()) {
        log::Info("Viewmodel shadows: creating the viewmodel's buffers failed; it does not shadow itself");
        g_state.failed = true;
        return;
    }
    Entry& entry = g_state.entries[g_state.current];
    // The lighting may still read this entry's indices and positions from three captures back.
    if (entry.fence != nullptr && entry.fence->GetCompletedValue() < entry.fenceValue)
        entry.fence->SetEventOnCompletion(entry.fenceValue, nullptr);
    entry.vertexCount = 0;
    entry.triangleCount = 0;
    entry.placed.clear();
    entry.recorded.clear();
    g_state.recording = true;
}

bool Recording() { return g_state.recording; }

std::optional<Positions> RecordDraw(const Mesh& mesh)
{
    if (!g_state.recording)
        return std::nullopt;
    if (!mesh.triangleList || mesh.indexData == nullptr || mesh.vertices == 0 || mesh.stride == 0) {
        if (!std::exchange(g_state.loggedSkip, true))
            log::Info("Viewmodel shadows: a draw of the viewmodel without indexed triangle lists casts no shadow on it");
        return std::nullopt;
    }
    Entry& entry = g_state.entries[g_state.current];
    const UINT vertexCount = mesh.vertexBytes / mesh.stride;
    const std::uint64_t vertexKey = Mix(Mix(14695981039346656037ull, mesh.vertices), mesh.vertexBytes);
    auto [placed, inserted] = entry.placed.try_emplace(vertexKey, entry.vertexCount);
    if (inserted) {
        if (entry.vertexCount + vertexCount > kMaxVertices) {
            entry.placed.erase(placed);
            if (!std::exchange(g_state.loggedFull, true))
                log::Info("Viewmodel shadows: the viewmodel has more than {} vertices; the rest cast no shadow", kMaxVertices);
            return std::nullopt;
        }
        entry.vertexCount += vertexCount;
    }
    const UINT first = placed->second;
    for (const auto& [firstIndex, count] : mesh.primitives) {
        if (firstIndex < 0 || count < 3)
            continue;
        const std::uint64_t listKey = Mix(Mix(Mix(vertexKey, mesh.indexBuffer), static_cast<std::uint64_t>(firstIndex)), static_cast<std::uint64_t>(count));
        if (!entry.recorded.insert(listKey).second)
            continue;
        const UINT triangles = static_cast<UINT>(count) / 3;
        for (UINT t = 0; t < triangles; ++t) {
            if (entry.triangleCount == kMaxTriangles) {
                if (!std::exchange(g_state.loggedFull, true))
                    log::Info("Viewmodel shadows: the viewmodel has more than {} triangles; the rest cast no shadow", kMaxTriangles);
                break;
            }
            const std::uint16_t* index = mesh.indexData + firstIndex + 3 * t;
            if (index[0] >= vertexCount || index[1] >= vertexCount || index[2] >= vertexCount)
                continue;
            std::uint32_t* out = entry.mappedIndices + 3 * entry.triangleCount;
            // Mirrored models are drawn with the other winding; the BLAS keeps Source's.
            out[0] = first + index[0];
            out[1] = first + index[mesh.mirrored ? 2 : 1];
            out[2] = first + index[mesh.mirrored ? 1 : 2];
            ++entry.triangleCount;
        }
    }
    return Positions{entry.positionsUav, first, first + vertexCount};
}

std::optional<Geometry> EndCapture()
{
    if (!std::exchange(g_state.recording, false))
        return std::nullopt;
    const UINT current = g_state.current;
    g_state.current = (current + 1) % static_cast<UINT>(g_state.entries.size());
    Entry& entry = g_state.entries[current];
    if (entry.triangleCount == 0)
        return std::nullopt;
    if (GetOptions().devMode && (entry.vertexCount != g_state.loggedVertices || entry.triangleCount != g_state.loggedTriangles)) {
        g_state.loggedVertices = entry.vertexCount;
        g_state.loggedTriangles = entry.triangleCount;
        log::Info("Viewmodel shadows: the viewmodel drew {} vertices and {} triangles", entry.vertexCount, entry.triangleCount);
    }
    return Geometry{current, entry.positions.Get(), entry.positionsView, entry.uploadedIndices.Get(), entry.indices.Get(), entry.indicesView, g_state.albedoView,
        entry.vertexCount, entry.triangleCount};
}

D3D12_GPU_VIRTUAL_ADDRESS RecordBlas(ID3D12GraphicsCommandList4* list, const Geometry& geometry)
{
    if (geometry.entry >= g_state.entries.size())
        return 0;
    Entry& entry = g_state.entries[geometry.entry];
    // Buffers are back in COMMON after the game's commands that wrote the positions (implicit state decay).
    D3D12_RESOURCE_BARRIER before[2] = {};
    before[0].Type = before[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    before[0].Transition = {entry.positions.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    before[1].Transition = {entry.indices.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(2, before);
    list->CopyBufferRegion(entry.indices.Get(), 0, entry.uploadedIndices.Get(), 0, geometry.triangleCount * 12ull);
    D3D12_RESOURCE_BARRIER after = {};
    after.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    after.Transition = {entry.indices.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &after);
    // All opaque: alpha-tested parts (lanyards, reticles) shadow as solid.
    D3D12_RAYTRACING_GEOMETRY_DESC descs[2] = {};
    const UINT count = Scene::GeometryDescs(entry.positions->GetGPUVirtualAddress(), geometry.vertexCount, entry.indices->GetGPUVirtualAddress(),
        geometry.triangleCount, geometry.triangleCount, descs);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
    build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    build.Inputs.NumDescs = count;
    build.Inputs.pGeometryDescs = descs;
    build.DestAccelerationStructureData = entry.blas->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = entry.scratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    D3D12_RESOURCE_BARRIER built = {};
    built.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    built.UAV.pResource = entry.blas.Get();
    list->ResourceBarrier(1, &built);
    return entry.blas->GetGPUVirtualAddress();
}

void MarkUsed(UINT entry, ID3D12Fence* fence, std::uint64_t value)
{
    if (entry >= g_state.entries.size())
        return;
    g_state.entries[entry].fence = fence;
    g_state.entries[entry].fenceValue = value;
}

}
