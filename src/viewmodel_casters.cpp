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
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gmodpp::viewmodel_casters {

namespace {

using Microsoft::WRL::ComPtr;

// Rays that hit the viewmodel see a mid-grey surface (only its own indirect light and reflections meet it).
constexpr std::uint32_t kGreyAlbedo = 0xFF595959u;

// Entries in turn: a capture's own, those of the frame's earlier captures and of the frame before (the parts its
// lighting traces too, OtherParts), and one being reused.
constexpr size_t kEntries = 4;

struct Entry {
    ComPtr<ID3D12Resource> positions;
    UINT positionsView = DescriptorHeap::kInvalid;
    UINT positionsUav = DescriptorHeap::kInvalid;
    ComPtr<ID3D12Resource> previous;
    UINT previousView = DescriptorHeap::kInvalid;
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
    // Each vertex buffer's first vertex and vertex count, and the index lists already recorded (a mesh drawn by several
    // passes).
    std::unordered_map<std::uint64_t, std::pair<UINT, UINT>> placed;
    std::unordered_set<std::uint64_t> recorded;
    // The order of its capture, and once its BLAS is built, the frame it was lit in.
    std::uint64_t sequence = 0;
    bool built = false;
    std::uint64_t frame = 0;
};

struct State {
    bool initialized = false;
    bool failed = false;
    bool recording = false;
    bool loggedSkip = false;
    bool loggedFull = false;
    // The sizes already logged: a weapon whose draws are captured in parts gives two sizes a frame.
    std::unordered_set<std::uint64_t> loggedSizes;
    UINT current = 0;
    std::uint64_t sequence = 0;
    std::array<Entry, kEntries> entries;
    ComPtr<ID3D12Resource> albedo;
    UINT albedoView = DescriptorHeap::kInvalid;
    // kNoPrevious for every coordinate of kMaxVertices vertices.
    ComPtr<ID3D12Resource> farPositions;
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
        entry.previous = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, kMaxVertices * 12ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        entry.uploadedIndices = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, kMaxTriangles * 12ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        entry.indices = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, kMaxTriangles * 12ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        entry.blas = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, prebuild.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        entry.scratch = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, std::max<UINT64>(prebuild.ScratchDataSizeInBytes, 256),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!entry.positions || !entry.previous || !entry.uploadedIndices || !entry.indices || !entry.blas || !entry.scratch)
            return false;
        void* mapped = nullptr;
        if (FAILED(entry.uploadedIndices->Map(0, nullptr, &mapped)))
            return false;
        entry.mappedIndices = static_cast<std::uint32_t*>(mapped);
        entry.positionsView = descriptors.CreateRawView(entry.positions.Get());
        entry.positionsUav = descriptors.CreateRawUav(entry.positions.Get());
        entry.previousView = descriptors.CreateRawView(entry.previous.Get());
        entry.indicesView = descriptors.CreateRawView(entry.indices.Get());
        if (entry.positionsView == DescriptorHeap::kInvalid || entry.positionsUav == DescriptorHeap::kInvalid || entry.previousView == DescriptorHeap::kInvalid ||
            entry.indicesView == DescriptorHeap::kInvalid)
            return false;
    }
    g_state.farPositions = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, kMaxVertices * 12ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* farMapped = nullptr;
    if (!g_state.farPositions || FAILED(g_state.farPositions->Map(0, nullptr, &farMapped)))
        return false;
    std::fill(static_cast<float*>(farMapped), static_cast<float*>(farMapped) + kMaxVertices * 3, kNoPrevious);
    g_state.farPositions->Unmap(0, nullptr);
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
    if (!GetSettings().realtimeLighting || g_state.failed)
        return;
    if (!std::exchange(g_state.initialized, true) && !Initialize()) {
        log::Info("Viewmodel shadows: creating the viewmodel's buffers failed; it does not shadow itself and its light history does not follow it");
        g_state.failed = true;
        return;
    }
    Entry& entry = g_state.entries[g_state.current];
    // The lighting may still read this entry's indices and positions from captures before.
    if (entry.fence != nullptr && entry.fence->GetCompletedValue() < entry.fenceValue)
        entry.fence->SetEventOnCompletion(entry.fenceValue, nullptr);
    entry.vertexCount = 0;
    entry.triangleCount = 0;
    entry.placed.clear();
    entry.recorded.clear();
    entry.built = false;
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
    auto [placed, inserted] = entry.placed.try_emplace(vertexKey, entry.vertexCount, vertexCount);
    if (inserted) {
        if (entry.vertexCount + vertexCount > kMaxVertices) {
            entry.placed.erase(placed);
            if (!std::exchange(g_state.loggedFull, true))
                log::Info("Viewmodel shadows: the viewmodel has more than {} vertices; the rest cast no shadow", kMaxVertices);
            return std::nullopt;
        }
        entry.vertexCount += vertexCount;
    }
    const UINT first = placed->second.first;
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
    constexpr size_t kLoggedSizes = 64;
    const std::uint64_t size = (static_cast<std::uint64_t>(entry.vertexCount) << 32) | entry.triangleCount;
    if (GetOptions().devMode && g_state.loggedSizes.size() < kLoggedSizes && g_state.loggedSizes.insert(size).second)
        log::Info("Viewmodel shadows: the viewmodel drew {} vertices and {} triangles", entry.vertexCount, entry.triangleCount);
    entry.sequence = ++g_state.sequence;
    return Geometry{current, entry.positionsView, entry.previousView, entry.indicesView, g_state.albedoView, entry.vertexCount, entry.triangleCount};
}

D3D12_GPU_VIRTUAL_ADDRESS RecordGeometry(ID3D12GraphicsCommandList4* list, const Geometry& geometry, std::uint64_t frame)
{
    if (geometry.entry >= g_state.entries.size())
        return 0;
    Entry& entry = g_state.entries[geometry.entry];
    entry.frame = frame;
    // Buffers are back in COMMON after the game's commands that wrote the positions and after the lighting that read the
    // captures before's (implicit state decay at the end of each ExecuteCommandLists).
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER& barrier = barriers.emplace_back();
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, after};
    };
    transition(entry.indices.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    transition(entry.previous.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    transition(entry.positions.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Each mesh's vertices where last frame's captures placed it, if one drew it with as many vertices: the most recent,
    // as a weapon whose draws are captured in parts draws each mesh in one of them.
    struct Copy {
        ID3D12Resource* source;
        UINT64 from;
        UINT to;
        UINT count;
    };
    std::vector<Copy> copies;
    std::vector<const Entry*> sources;
    for (const auto& [key, range] : entry.placed) {
        const Entry* source = nullptr;
        UINT from = 0;
        for (const Entry& other : g_state.entries) {
            if (&other == &entry || !other.built || other.frame + 1 != frame || (source != nullptr && other.sequence < source->sequence))
                continue;
            const auto found = other.placed.find(key);
            if (found != other.placed.end() && found->second.second == range.second) {
                source = &other;
                from = found->second.first;
            }
        }
        if (source != nullptr && std::find(sources.begin(), sources.end(), source) == sources.end()) {
            sources.push_back(source);
            transition(source->positions.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        copies.push_back({source != nullptr ? source->positions.Get() : g_state.farPositions.Get(), source != nullptr ? from * 12ull : 0ull, range.first, range.second});
    }
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    list->CopyBufferRegion(entry.indices.Get(), 0, entry.uploadedIndices.Get(), 0, geometry.triangleCount * 12ull);
    for (const Copy& copy : copies)
        list->CopyBufferRegion(entry.previous.Get(), copy.to * 12ull, copy.source, copy.from, copy.count * 12ull);
    D3D12_RESOURCE_BARRIER after[2] = {};
    for (D3D12_RESOURCE_BARRIER& barrier : after)
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    after[0].Transition = {entry.indices.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    after[1].Transition = {entry.previous.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(2, after);
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
    entry.built = true;
    return entry.blas->GetGPUVirtualAddress();
}

std::vector<Part> OtherParts(const Geometry& geometry, std::uint64_t frame)
{
    std::vector<Part> parts;
    if (geometry.entry >= g_state.entries.size())
        return parts;
    const Entry& entry = g_state.entries[geometry.entry];
    // The frame's earlier captures, then last frame's, most recent first; one whose meshes a capture taken already has
    // is an older copy of it.
    std::vector<const Entry*> candidates;
    for (const Entry& other : g_state.entries) {
        if (&other != &entry && other.built && (other.frame + 1 == frame || (other.frame == frame && other.sequence < entry.sequence)))
            candidates.push_back(&other);
    }
    std::sort(candidates.begin(), candidates.end(), [](const Entry* a, const Entry* b) { return a->sequence > b->sequence; });
    std::unordered_set<std::uint64_t> taken;
    for (const auto& placed : entry.placed)
        taken.insert(placed.first);
    for (const Entry* other : candidates) {
        const bool overlaps = std::any_of(other->placed.begin(), other->placed.end(), [&](const auto& placed) { return taken.contains(placed.first); });
        if (overlaps)
            continue;
        for (const auto& placed : other->placed)
            taken.insert(placed.first);
        parts.push_back({static_cast<UINT>(other - g_state.entries.data()), other->blas->GetGPUVirtualAddress(), other->positionsView, other->indicesView,
            g_state.albedoView, other->triangleCount});
    }
    return parts;
}

void MarkUsed(UINT entry, ID3D12Fence* fence, std::uint64_t value)
{
    if (entry >= g_state.entries.size())
        return;
    g_state.entries[entry].fence = fence;
    g_state.entries[entry].fenceValue = value;
}

}
