#include "shaderapi_meshes.h"

#include "log.h"
#include "shaderapi.h"

#include <d3d9types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <map>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp::native {

namespace {

// MaterialPrimitiveType_t (public/materialsystem/imaterialsystem.h).
constexpr int kPrimitivePoints = 0;
constexpr int kPrimitiveLines = 1;
constexpr int kPrimitiveTriangles = 2;
constexpr int kPrimitiveTriangleStrip = 3;
constexpr int kPrimitiveLineStrip = 4;
// IShaderUtil (materialsystem.dll), in the SDK's order (shaderapi_slots.cpp).
constexpr unsigned kShaderUtilOnDrawMeshLists = 20;
constexpr unsigned kShaderUtilOnDrawMesh = 21;
// IMaterial::GetVertexFormat (public/materialsystem/imaterial.h).
constexpr unsigned kMaterialGetVertexFormat = 20;
// GMod's dynamic vertex buffer size and dynamic index count (caps.txt: GetCurrentDynamicVBSize,
// GetMaxIndicesToRender), which bound how much materialsystem batches into one dynamic mesh.
constexpr int kDynamicVertexBytes = 1572864;
constexpr int kDynamicIndices = 32768;
constexpr int kMaxVertices = 0xffff;
constexpr size_t kMeshSlots = 44;
constexpr size_t kIndexBufferSlots = 13;
constexpr UINT64 kVertexAlignment = 16;

// VertexDesc_t, IndexDesc_t and MeshDesc_t (public/materialsystem/imesh.h) as GMod's 64-bit build lays them out
// (the offsets shaderapidx9.dll's ComputeVertexDescription at 0x180005a60 writes).
struct VertexDesc {
    int sizePosition;
    int sizeBoneWeight;
    int sizeBoneMatrixIndex;
    int sizeNormal;
    int sizeColor;
    int sizeSpecular;
    int sizeTexCoord[kMaxTexCoords];
    int sizeTangentS;
    int sizeTangentT;
    int sizeWrinkle;
    int sizeUserData;
    int actualVertexSize;
    int compressionType;
    int numBoneWeights;
    float* position;
    float* boneWeight;
    unsigned char* boneMatrixIndex;
    float* normal;
    unsigned char* color;
    unsigned char* specular;
    float* texCoord[kMaxTexCoords];
    float* tangentS;
    float* tangentT;
    float* wrinkle;
    float* userData;
    int firstVertex;
    unsigned offset;
};
static_assert(offsetof(VertexDesc, position) == 88 && offsetof(VertexDesc, userData) == 224 && sizeof(VertexDesc) == 240);

struct IndexDesc {
    unsigned short* indices;
    unsigned offset;
    unsigned firstIndex;
    unsigned char indexSize;
};

struct MeshDesc : VertexDesc, IndexDesc {};
static_assert(sizeof(MeshDesc) == 264);

struct CPrimList {
    int firstIndex;
    int indexCount;
};

// shaderapidx9.dll points absent elements at a zeroed static buffer with a stride of 0 (0x1800a3600).
alignas(16) std::uint8_t g_dummy[256];

// A default-heap buffer whose state follows buffer promotion and decay: it is COMMON again at the start of each
// command list (D3D12 "Implicit state transitions").
struct GpuBuffer {
    TrackedResource resource;
    UINT64 size = 0;
    std::uint64_t generation = 0;

    void Require(D3D12_RESOURCE_STATES state)
    {
        Backend& backend = GetBackend();
        if (generation != backend.ListGeneration()) {
            generation = backend.ListGeneration();
            resource.state = D3D12_RESOURCE_STATE_COMMON;
        }
        if (resource.state == D3D12_RESOURCE_STATE_COMMON)
            resource.state = state;
        else
            backend.Transition(resource, state);
    }
};

constexpr D3D12_RESOURCE_STATES kBufferRead = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER;

bool Resize(GpuBuffer& buffer, UINT64 size)
{
    if (buffer.size >= size)
        return true;
    Backend& backend = GetBackend();
    if (buffer.resource.resource != nullptr)
        backend.Retire(std::move(buffer.resource.resource));
    buffer = {};
    const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&buffer.resource.resource));
    if (FAILED(hr)) {
        log::Info("Native: creating a {}-byte mesh buffer failed: {:#010x}", size, static_cast<unsigned>(hr));
        return false;
    }
    buffer.size = size;
    return true;
}

void UploadBytes(GpuBuffer& buffer, size_t total, size_t begin, const std::uint8_t* bytes, size_t count)
{
    if (!Resize(buffer, total))
        return;
    Backend& backend = GetBackend();
    Backend::Upload staging = backend.AllocateStaging(count, 16);
    if (staging.data == nullptr)
        return;
    std::memcpy(staging.data, bytes, count);
    buffer.Require(D3D12_RESOURCE_STATE_COPY_DEST);
    backend.Commands()->CopyBufferRegion(buffer.resource.resource.Get(), begin, staging.buffer, staging.offset, count);
}

// Copies [begin, end) of a static mesh's CPU copy into its buffer, sized to the whole copy.
void Upload(GpuBuffer& buffer, const std::vector<std::uint8_t>& data, size_t begin, size_t end)
{
    if (begin >= end)
        return;
    Backend& backend = GetBackend();
    if (backend.OnRenderThread()) {
        UploadBytes(buffer, data.size(), begin, data.data() + begin, end - begin);
        return;
    }
    backend.RunOnRenderThread([&buffer, total = data.size(), begin, bytes = std::vector<std::uint8_t>(data.begin() + begin, data.begin() + end)] {
        UploadBytes(buffer, total, begin, bytes.data(), bytes.size());
    });
}

// GMod's IMesh object: its vtable (44 entries), then IIndexBuffer's at offset 8, as CMeshDX8 lays them out.
struct Mesh {
    const void* const* vtable;
    const void* const* indexVtable;
    bool dynamic = false;
    std::uint64_t format = 0;
    int primitiveType = kPrimitiveTriangles;
    int vertexCount = 0;
    int indexCount = 0;
    // Static meshes: CPU copies uploaded on unlock; dynamic meshes: the last lock's upload memory.
    std::vector<std::uint8_t> vertices;
    std::vector<std::uint8_t> indices;
    GpuBuffer vertexBuffer;
    GpuBuffer indexBuffer;
    Backend::Upload dynamicVertices = {};
    Backend::Upload dynamicIndices = {};
    UINT64 dynamicVertexBytes = 0;
    UINT64 dynamicIndexBytes = 0;
    Mesh* colorMesh = nullptr;
    int colorOffset = 0;
    // Stream 2 from SetFlexMesh, and the flex mesh's vertex format.
    D3D12_VERTEX_BUFFER_VIEW flexStream = {};
    std::uint64_t flexFormat = 0;
    Mesh* vertexOverride = nullptr;
    Mesh* indexOverride = nullptr;
    bool modifyReadOnly = true;
    size_t modifyVertexBegin = 0;
    size_t modifyVertexEnd = 0;
    size_t modifyIndexBegin = 0;
    size_t modifyIndexEnd = 0;
};
static_assert(offsetof(Mesh, indexVtable) == 8);

Mesh* FromIndexBuffer(void* self) { return reinterpret_cast<Mesh*>(static_cast<std::uint8_t*>(self) - 8); }

template <class Fn>
Fn UtilSlot(unsigned slot)
{
    return reinterpret_cast<Fn>((*static_cast<void***>(GetBackend().shaderUtil))[slot]);
}

void FillVertexDesc(std::uint8_t* base, std::uint64_t format, VertexDesc& desc);

int Stride(const Mesh& mesh) { return VertexSize(mesh.format); }

// Vertex streams of a draw: the mesh's (or its override's) vertices, its colour mesh and its flex mesh.
void Streams(Mesh& mesh, MeshDraw& draw)
{
    Mesh& source = mesh.vertexOverride != nullptr ? *mesh.vertexOverride : mesh;
    const int stride = Stride(source);
    if (source.dynamic) {
        draw.vertexData = source.dynamicVertices.data;
        if (source.dynamicVertices.buffer != nullptr) {
            draw.streams[0] = { source.dynamicVertices.buffer->GetGPUVirtualAddress() + source.dynamicVertices.offset,
                static_cast<UINT>(source.dynamicVertexBytes), static_cast<UINT>(stride) };
        }
    } else if (source.vertexBuffer.resource.resource != nullptr) {
        source.vertexBuffer.Require(kBufferRead);
        draw.streams[0] = { source.vertexBuffer.resource.resource->GetGPUVirtualAddress(), static_cast<UINT>(source.vertexBuffer.size),
            static_cast<UINT>(stride) };
        draw.buffers[0] = source.vertexBuffer.resource.resource.Get();
    }
    draw.vertexFormat = source.format;
    draw.vertexCount = source.vertexCount;
    if (mesh.colorMesh != nullptr && mesh.colorMesh->vertexBuffer.resource.resource != nullptr) {
        GpuBuffer& colors = mesh.colorMesh->vertexBuffer;
        colors.Require(kBufferRead);
        const UINT64 offset = static_cast<UINT64>(mesh.colorOffset);
        draw.streams[1] = { colors.resource.resource->GetGPUVirtualAddress() + offset, static_cast<UINT>(colors.size - offset),
            static_cast<UINT>(Stride(*mesh.colorMesh)) };
    }
    if (mesh.flexFormat != 0) {
        draw.streams[2] = mesh.flexStream;
        draw.flexFormat = mesh.flexFormat;
    }
}

void Indices(Mesh& mesh, MeshDraw& draw)
{
    Mesh& source = mesh.indexOverride != nullptr ? *mesh.indexOverride : mesh;
    if (source.dynamic) {
        if (source.dynamicIndices.buffer != nullptr && source.indexCount > 0)
            draw.indices = { source.dynamicIndices.buffer->GetGPUVirtualAddress() + source.dynamicIndices.offset,
                static_cast<UINT>(source.dynamicIndexBytes), DXGI_FORMAT_R16_UINT };
    } else if (source.indexBuffer.resource.resource != nullptr && source.indexCount > 0) {
        source.indexBuffer.Require(kBufferRead);
        draw.indices = { source.indexBuffer.resource.resource->GetGPUVirtualAddress(), static_cast<UINT>(source.indexBuffer.size), DXGI_FORMAT_R16_UINT };
    }
}

D3D12_PRIMITIVE_TOPOLOGY Topology(int type)
{
    switch (type) {
    case kPrimitivePoints: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case kPrimitiveLines: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case kPrimitiveTriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case kPrimitiveLineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

void Submit(Mesh& mesh, std::span<const std::pair<int, int>> primitives)
{
    GetBackend().RunPending();
    MeshDraw draw;
    Streams(mesh, draw);
    Indices(mesh, draw);
    draw.topology = Topology(mesh.primitiveType);
    draw.primitives = primitives;
    if (draw.streams[0].SizeInBytes == 0) {
        static bool logged = false;
        if (!std::exchange(logged, true))
            log::Info("Native: a mesh was drawn without vertices");
        return;
    }
    DrawMesh(draw);
}

void MarkAsDrawn(Mesh* self)
{
    if (self->dynamic) {
        self->vertexCount = 0;
        self->indexCount = 0;
    }
}

// CDynamicMeshDX8::Draw (0x180009100) and CMeshDX8::Draw (0x180009310): materialsystem's OnDrawMesh may take the
// draw (buffering, or no material), otherwise all indices, or the requested range, go to the material.
void DrawRange(Mesh* self, int first, int count)
{
    using OnDrawFn = bool (*)(void*, void*, int, int);
    if (!UtilSlot<OnDrawFn>(kShaderUtilOnDrawMesh)(GetBackend().shaderUtil, self, first, count)) {
        MarkAsDrawn(self);
        return;
    }
    Mesh& indexSource = self->indexOverride != nullptr ? *self->indexOverride : *self;
    if (indexSource.indexCount == 0) {
        Mesh& vertexSource = self->vertexOverride != nullptr ? *self->vertexOverride : *self;
        const std::pair<int, int> vertices = { 0, vertexSource.vertexCount };
        Submit(*self, { &vertices, 1 });
        return;
    }
    if (first == -1 || count == 0) {
        first = 0;
        count = indexSource.indexCount;
    }
    const std::pair<int, int> indices = { first, count };
    Submit(*self, { &indices, 1 });
}

void DrawLists(Mesh* self, CPrimList* lists, int count)
{
    using OnDrawFn = bool (*)(void*, void*, CPrimList*, int);
    if (!UtilSlot<OnDrawFn>(kShaderUtilOnDrawMeshLists)(GetBackend().shaderUtil, self, lists, count)) {
        MarkAsDrawn(self);
        return;
    }
    // Reused across draws (render thread only) to avoid an allocation per draw.
    static std::vector<std::pair<int, int>> primitives;
    primitives.clear();
    for (int i = 0; i < count; ++i)
        primitives.emplace_back(lists[i].firstIndex, lists[i].indexCount);
    Submit(*self, primitives);
}

void LockVertices(Mesh* self, int count, VertexDesc& desc)
{
    const int stride = Stride(*self);
    std::uint8_t* base = g_dummy;
    if (self->vertexOverride == nullptr && count > 0) {
        const UINT64 bytes = static_cast<UINT64>(count) * stride;
        if (self->dynamic) {
            self->dynamicVertices = GetBackend().AllocateUpload(bytes, kVertexAlignment);
            self->dynamicVertexBytes = bytes;
            base = self->dynamicVertices.data != nullptr ? self->dynamicVertices.data : g_dummy;
        } else {
            self->vertices.resize(static_cast<size_t>(bytes));
            base = self->vertices.data();
        }
    }
    FillVertexDesc(base, self->format, desc);
    desc.firstVertex = 0;
    desc.offset = 0;
}

void LockIndices(Mesh* self, int count, IndexDesc& desc)
{
    desc.indices = reinterpret_cast<unsigned short*>(g_dummy);
    desc.indexSize = 0;
    desc.offset = 0;
    desc.firstIndex = 0;
    if (self->indexOverride != nullptr || count <= 0)
        return;
    const UINT64 bytes = static_cast<UINT64>(count) * sizeof(unsigned short);
    if (self->dynamic) {
        self->dynamicIndices = GetBackend().AllocateUpload(bytes, 4);
        self->dynamicIndexBytes = bytes;
        if (self->dynamicIndices.data == nullptr)
            return;
        desc.indices = reinterpret_cast<unsigned short*>(self->dynamicIndices.data);
    } else {
        self->indices.resize(static_cast<size_t>(bytes));
        desc.indices = reinterpret_cast<unsigned short*>(self->indices.data());
    }
    desc.indexSize = 1;
}

void LockMesh(Mesh* self, int vertexCount, int indexCount, MeshDesc& desc)
{
    LockVertices(self, vertexCount, desc);
    LockIndices(self, indexCount, desc);
}

void UnlockMesh(Mesh* self, int vertexCount, int indexCount, MeshDesc&)
{
    if (self->vertexOverride == nullptr)
        self->vertexCount = vertexCount;
    if (self->indexOverride == nullptr)
        self->indexCount = indexCount;
    // Direct3D 9 keeps everything written to a locked static buffer, whatever count the unlock reports: the engine
    // fills static prop colour meshes through the locked pointer and unlocks them with a count of 0.
    if (!self->dynamic) {
        Upload(self->vertexBuffer, self->vertices, 0, self->vertices.size());
        Upload(self->indexBuffer, self->indices, 0, self->indices.size());
    }
}

void ModifyBeginEx(Mesh* self, bool readOnly, int firstVertex, int vertexCount, int firstIndex, int indexCount, MeshDesc& desc)
{
    const int stride = Stride(*self);
    std::uint8_t* base = g_dummy;
    self->modifyReadOnly = readOnly;
    self->modifyVertexBegin = static_cast<size_t>(firstVertex) * stride;
    self->modifyVertexEnd = std::min(self->vertices.size(), static_cast<size_t>(firstVertex + vertexCount) * stride);
    self->modifyIndexBegin = static_cast<size_t>(firstIndex) * sizeof(unsigned short);
    self->modifyIndexEnd = std::min(self->indices.size(), static_cast<size_t>(firstIndex + indexCount) * sizeof(unsigned short));
    if (self->dynamic) {
        log::Info("Native: ModifyBegin on a dynamic mesh is not supported");
    } else if (vertexCount > 0 && self->modifyVertexBegin < self->vertices.size()) {
        base = self->vertices.data() + self->modifyVertexBegin;
    }
    VertexDesc& vertexDesc = desc;
    IndexDesc& indexDesc = desc;
    FillVertexDesc(base, self->format, vertexDesc);
    vertexDesc.firstVertex = firstVertex;
    vertexDesc.offset = 0;
    indexDesc.indices = reinterpret_cast<unsigned short*>(g_dummy);
    indexDesc.indexSize = 0;
    indexDesc.offset = 0;
    indexDesc.firstIndex = 0;
    if (!self->dynamic && indexCount > 0 && self->modifyIndexBegin < self->indices.size()) {
        indexDesc.indices = reinterpret_cast<unsigned short*>(self->indices.data() + self->modifyIndexBegin);
        indexDesc.indexSize = 1;
    }
}

void ModifyBegin(Mesh* self, int firstVertex, int vertexCount, int firstIndex, int indexCount, MeshDesc& desc)
{
    ModifyBeginEx(self, false, firstVertex, vertexCount, firstIndex, indexCount, desc);
}

void ModifyEnd(Mesh* self, MeshDesc&)
{
    if (self->dynamic || self->modifyReadOnly)
        return;
    Upload(self->vertexBuffer, self->vertices, self->modifyVertexBegin, self->modifyVertexEnd);
    Upload(self->indexBuffer, self->indices, self->modifyIndexBegin, self->modifyIndexEnd);
}

void Destructor(Mesh*, unsigned)
{
    static bool logged = false;
    if (!std::exchange(logged, true))
        log::Info("Native: a mesh's destructor was called through its vtable; meshes are destroyed by DestroyStaticMesh");
}

int VertexCount(Mesh* self) { return self->vertexCount; }
std::uint64_t GetVertexFormat(Mesh* self) { return self->format; }
bool IsDynamic(Mesh* self) { return self->dynamic; }
void BeginCastBuffer(Mesh*, std::uint64_t) {}
void EndCastBuffer(Mesh*) {}
int GetRoomRemaining(Mesh* self) { return self->dynamic ? std::min(kMaxVertices, kDynamicVertexBytes / std::max(1, Stride(*self))) : 0; }

bool LockVertexBuffer(Mesh* self, int count, bool, VertexDesc& desc)
{
    LockVertices(self, count, desc);
    return true;
}

void UnlockVertexBuffer(Mesh* self, int count, VertexDesc&)
{
    self->vertexCount = count;
    if (!self->dynamic)
        Upload(self->vertexBuffer, self->vertices, 0, self->vertices.size());
}

void Ignore(Mesh*, int, const void*) {}
void IgnoreMesh(Mesh*, int, int, const void*) {}
void SetPrimitiveType(Mesh* self, int type) { self->primitiveType = type; }

void SetColorMesh(Mesh* self, Mesh* colorMesh, int offset)
{
    self->colorMesh = colorMesh;
    self->colorOffset = offset;
}

void CopyToMeshBuilder(Mesh*, int, int, int, int, int, void*)
{
    static bool logged = false;
    if (!std::exchange(logged, true))
        log::Info("Native: IMesh::CopyToMeshBuilder is not implemented");
}

void DisableFlexMesh(Mesh* self)
{
    self->flexStream = {};
    self->flexFormat = 0;
}

// CMeshDX8::SetFlexMesh and its stream 2 (0x18000d6c0): the deltas start offset bytes into the flex mesh's vertices.
// The flex mesh's locks report offset 0, so the engine's offsets are into its last lock, which is kept here.
void SetFlexMesh(Mesh* self, Mesh* flexMesh, int offset)
{
    DisableFlexMesh(self);
    if (flexMesh == nullptr)
        return;
    if (!flexMesh->dynamic || flexMesh->dynamicVertices.buffer == nullptr || offset < 0 || static_cast<UINT64>(offset) >= flexMesh->dynamicVertexBytes) {
        static bool logged = false;
        if (!std::exchange(logged, true))
            log::Info("Native: a flex mesh offset of {} bytes is outside its last lock of {} bytes", offset, flexMesh->dynamicVertexBytes);
        return;
    }
    self->flexStream = { flexMesh->dynamicVertices.buffer->GetGPUVirtualAddress() + flexMesh->dynamicVertices.offset + static_cast<UINT64>(offset),
        static_cast<UINT>(flexMesh->dynamicVertexBytes - static_cast<UINT64>(offset)), static_cast<UINT>(Stride(*flexMesh)) };
    self->flexFormat = flexMesh->format;
}
unsigned ComputeMemoryUsed(Mesh* self) { return static_cast<unsigned>(self->vertexBuffer.size + self->indexBuffer.size); }

// The IIndexBuffer subobject, at offset 8.
void IndexDestructor(void* self, unsigned flags) { Destructor(FromIndexBuffer(self), flags); }
int IndexCount(void* self) { return FromIndexBuffer(self)->indexCount; }
int IndexFormat(void*) { return 0; }
bool IndexIsDynamic(void* self) { return FromIndexBuffer(self)->dynamic; }
void IndexCast(void*, int) {}
void IndexEndCast(void*) {}
int IndexRoomRemaining(void* self) { return FromIndexBuffer(self)->dynamic ? kDynamicIndices : 0; }
// CMeshDX8's IIndexBuffer::Lock returns false (its slot 7 is shaderapidx9.dll's 0x1800041b0).
bool IndexLock(void*, int, bool, IndexDesc&) { return false; }
void IndexUnlock(void*, int, IndexDesc&) {}

void IndexModifyBegin(void* self, bool readOnly, int first, int count, IndexDesc& desc)
{
    Mesh* mesh = FromIndexBuffer(self);
    mesh->modifyReadOnly = readOnly;
    mesh->modifyVertexBegin = mesh->modifyVertexEnd = 0;
    mesh->modifyIndexBegin = static_cast<size_t>(first) * sizeof(unsigned short);
    mesh->modifyIndexEnd = std::min(mesh->indices.size(), static_cast<size_t>(first + count) * sizeof(unsigned short));
    desc.indices = reinterpret_cast<unsigned short*>(g_dummy);
    desc.indexSize = 0;
    desc.offset = 0;
    desc.firstIndex = 0;
    if (!mesh->dynamic && mesh->modifyIndexBegin < mesh->indices.size()) {
        desc.indices = reinterpret_cast<unsigned short*>(mesh->indices.data() + mesh->modifyIndexBegin);
        desc.indexSize = 1;
    }
}

void IndexModifyEnd(void* self, IndexDesc&)
{
    Mesh* mesh = FromIndexBuffer(self);
    if (!mesh->dynamic && !mesh->modifyReadOnly)
        Upload(mesh->indexBuffer, mesh->indices, mesh->modifyIndexBegin, mesh->modifyIndexEnd);
}

void IndexIgnore(void*, int, const void*) {}

// CMeshDX8's own slots (27-43), which only shaderapidx9.dll calls; any other caller is logged once per slot.
template <size_t Slot>
void OwnSlot(Mesh*)
{
    static std::atomic<bool> logged = false;
    if (!logged.exchange(true))
        log::Info("Native: IMesh slot {} is not implemented", Slot);
}

template <size_t... Slots>
void FillOwnSlots(std::array<const void*, kMeshSlots + 1>& table, std::index_sequence<Slots...>)
{
    ((table[27 + Slots] = reinterpret_cast<const void*>(&OwnSlot<27 + Slots>)), ...);
}

const void* const* MeshVtable()
{
    static const std::array<const void*, kMeshSlots + 1> table = [] {
        std::array<const void*, kMeshSlots + 1> t = {};
        auto set = [&](size_t slot, auto function) { t[slot] = reinterpret_cast<const void*>(function); };
        set(0, &Destructor);
        set(1, &VertexCount);
        set(2, &GetVertexFormat);
        set(3, &IsDynamic);
        set(4, &BeginCastBuffer);
        set(5, &EndCastBuffer);
        set(6, &GetRoomRemaining);
        set(7, &LockVertexBuffer);
        set(8, &UnlockVertexBuffer);
        set(9, &Ignore);
        set(10, &Ignore);
        set(11, &SetPrimitiveType);
        set(12, &DrawLists);
        set(13, &DrawRange);
        set(14, &SetColorMesh);
        set(15, &CopyToMeshBuilder);
        set(16, &IgnoreMesh);
        set(17, &IgnoreMesh);
        set(18, &LockMesh);
        set(19, &ModifyBegin);
        set(20, &ModifyEnd);
        set(21, &UnlockMesh);
        set(22, &ModifyBeginEx);
        set(23, &SetFlexMesh);
        set(24, &DisableFlexMesh);
        set(25, &MarkAsDrawn);
        set(26, &ComputeMemoryUsed);
        FillOwnSlots(t, std::make_index_sequence<kMeshSlots - 27>());
        return t;
    }();
    return table.data();
}

const void* const* IndexBufferVtable()
{
    static const std::array<const void*, kIndexBufferSlots + 1> table = {
        reinterpret_cast<const void*>(&IndexDestructor),
        reinterpret_cast<const void*>(&IndexCount),
        reinterpret_cast<const void*>(&IndexFormat),
        reinterpret_cast<const void*>(&IndexIsDynamic),
        reinterpret_cast<const void*>(&IndexCast),
        reinterpret_cast<const void*>(&IndexEndCast),
        reinterpret_cast<const void*>(&IndexRoomRemaining),
        reinterpret_cast<const void*>(&IndexLock),
        reinterpret_cast<const void*>(&IndexUnlock),
        reinterpret_cast<const void*>(&IndexModifyBegin),
        reinterpret_cast<const void*>(&IndexModifyEnd),
        reinterpret_cast<const void*>(&IndexIgnore),
        reinterpret_cast<const void*>(&IndexIgnore),
        nullptr,
    };
    return table.data();
}

Mesh* NewMesh(bool dynamic, std::uint64_t format)
{
    Mesh* mesh = new Mesh{ MeshVtable(), IndexBufferVtable() };
    mesh->dynamic = dynamic;
    mesh->format = format;
    return mesh;
}

Mesh& DynamicMesh()
{
    static Mesh* mesh = NewMesh(true, 0);
    return *mesh;
}

// CMeshMgr::GetFlexMesh (0x18000a280): a dynamic mesh of position, wrinkle and normal (28 bytes), the wrinkle only
// with ps_2_b, which the hardware config reports.
Mesh* GetFlexMesh(NativeObject*)
{
    static Mesh* mesh = NewMesh(true, kVertexPosition | kVertexNormal | kVertexWrinkle | kVertexFormatUseExactFormat);
    return mesh;
}

// CMeshMgr::GetDynamicMesh (0x18000a020): the vertex format comes from the override, the given format or the
// material, with two bone weights and bone indices when skinned; buffering is not emulated, so materialsystem's
// draws reach the dynamic mesh at once.
Mesh* GetDynamicMeshEx(NativeObject*, void* material, std::uint64_t format, int bones, bool, Mesh* vertexOverride, Mesh* indexOverride)
{
    Mesh& mesh = DynamicMesh();
    mesh.vertexOverride = vertexOverride;
    mesh.indexOverride = indexOverride;
    mesh.colorMesh = nullptr;
    mesh.flexStream = {};
    mesh.flexFormat = 0;
    if (vertexOverride != nullptr) {
        mesh.format = vertexOverride->format;
        return &mesh;
    }
    std::uint64_t base = 0;
    if (material != nullptr)
        base = reinterpret_cast<std::uint64_t (*)(void*)>((*static_cast<void***>(material))[kMaterialGetVertexFormat])(material) & ~kVertexFormatCompressed;
    if (format != 0) {
        base = format;
        bones = std::max(bones, BoneWeights(format));
    }
    const std::uint64_t boneBits = 7ull << kVertexBoneWeightBit;
    mesh.format = bones > 0 ? ((base & ~boneBits) | (2ull << kVertexBoneWeightBit) | kVertexBoneIndex) : (base & ~boneBits);
    if (mesh.format == 0)
        log::Info("Native: GetDynamicMesh was asked for vertex format 0");
    return &mesh;
}

Mesh* GetDynamicMesh(NativeObject* self, void* material, int bones, bool buffered, Mesh* vertexOverride, Mesh* indexOverride)
{
    return GetDynamicMeshEx(self, material, 0, bones, buffered, vertexOverride, indexOverride);
}

int GetCurrentDynamicVBSize(NativeObject*) { return kDynamicVertexBytes; }

// CMeshMgr::GetMaxToRender (0x18000a2f0), GetMaxVerticesToRender (0x18000a490) and GetMaxIndicesToRender
// (0x18000a2e0), with our dynamic buffers as large as GMod's.
void GetMaxToRender(NativeObject*, Mesh* mesh, bool, int* maxVertices, int* maxIndices)
{
    if (mesh == nullptr) {
        *maxVertices = 0;
        *maxIndices = kDynamicIndices;
    } else if (!mesh->dynamic) {
        *maxVertices = kMaxVertices;
        *maxIndices = kMaxVertices;
    } else {
        *maxVertices = std::min(kMaxVertices, kDynamicVertexBytes / std::max(1, Stride(*mesh)));
        *maxIndices = kDynamicIndices;
    }
}

int GetMaxVerticesToRender(NativeObject*, void* material)
{
    if (material == nullptr)
        return 0;
    const std::uint64_t format = reinterpret_cast<std::uint64_t (*)(void*)>((*static_cast<void***>(material))[kMaterialGetVertexFormat])(material);
    const int size = VertexSize(format & ~kVertexFormatCompressed);
    return size == 0 ? 0 : std::min(kMaxVertices, kDynamicVertexBytes / size);
}

int GetMaxIndicesToRender(NativeObject*) { return kDynamicIndices; }

void ComputeVertexDescription(NativeObject*, std::uint8_t* buffer, std::uint64_t format, MeshDesc& desc) { FillVertexDesc(buffer, format, desc); }

void* GetVertexModifyBuilder(NativeObject*) { return nullptr; }

Mesh* CreateStaticMesh(NativeObject*, std::uint64_t format, const char*, void*)
{
    return NewMesh(false, format);
}

void DestroyStaticMesh(NativeObject*, Mesh* mesh)
{
    if (mesh == nullptr || mesh == &DynamicMesh())
        return;
    Backend& backend = GetBackend();
    // After the uploads queued for it.
    backend.RunOnRenderThread([mesh] {
        Backend& backend = GetBackend();
        backend.RunPending();
        backend.Retire(std::move(mesh->vertexBuffer.resource.resource));
        backend.Retire(std::move(mesh->indexBuffer.resource.resource));
        delete mesh;
    });
}

// The layout walk of shaderapidx9.dll's ComputeVertexDescription (0x180005a60); see VertexElements for the same walk
// as a vertex declaration.
void FillVertexDesc(std::uint8_t* base, std::uint64_t format, VertexDesc& desc)
{
    const bool compressed = (format & kVertexFormatCompressed) != 0;
    std::array<int*, 24> present;
    size_t presentCount = 0;
    int offset = 0;
    auto take = [&](int* size, auto*& pointer, int bytes) {
        using Pointer = std::remove_reference_t<decltype(pointer)>;
        pointer = reinterpret_cast<Pointer>(base + offset);
        present[presentCount++] = size;
        offset += bytes;
    };
    auto absent = [&](int* size, auto*& pointer) {
        using Pointer = std::remove_reference_t<decltype(pointer)>;
        pointer = reinterpret_cast<Pointer>(g_dummy);
        *size = 0;
    };
    desc.compressionType = compressed ? 1 : 0;
    desc.numBoneWeights = BoneWeights(format);
    if (format & kVertexPosition) {
        desc.position = reinterpret_cast<float*>(base);
        present[presentCount++] = &desc.sizePosition;
        offset = 12;
        if (format & kVertexWrinkle) {
            desc.wrinkle = reinterpret_cast<float*>(base + 12);
            present[presentCount++] = &desc.sizeWrinkle;
            offset = 16;
        } else {
            absent(&desc.sizeWrinkle, desc.wrinkle);
        }
    } else {
        absent(&desc.sizePosition, desc.position);
        absent(&desc.sizeWrinkle, desc.wrinkle);
    }
    if (format & kVertexBoneIndex) {
        if (BoneWeights(format) != 0)
            take(&desc.sizeBoneWeight, desc.boneWeight, compressed ? 4 : 8);
        else
            absent(&desc.sizeBoneWeight, desc.boneWeight);
        take(&desc.sizeBoneMatrixIndex, desc.boneMatrixIndex, 4);
    } else {
        absent(&desc.sizeBoneWeight, desc.boneWeight);
        absent(&desc.sizeBoneMatrixIndex, desc.boneMatrixIndex);
    }
    if (format & kVertexNormal)
        take(&desc.sizeNormal, desc.normal, compressed ? 4 : 12);
    else
        absent(&desc.sizeNormal, desc.normal);
    if (format & kVertexColor)
        take(&desc.sizeColor, desc.color, 4);
    else
        absent(&desc.sizeColor, desc.color);
    if (format & kVertexSpecular)
        take(&desc.sizeSpecular, desc.specular, 4);
    else
        absent(&desc.sizeSpecular, desc.specular);
    for (int t = 0; t < kMaxTexCoords; ++t) {
        const int size = TexCoordSize(t, format);
        if (size != 0)
            take(&desc.sizeTexCoord[t], desc.texCoord[t], 4 * size);
        else
            absent(&desc.sizeTexCoord[t], desc.texCoord[t]);
    }
    if (format & kVertexTangentS)
        take(&desc.sizeTangentS, desc.tangentS, 12);
    else
        absent(&desc.sizeTangentS, desc.tangentS);
    if (format & kVertexTangentT)
        take(&desc.sizeTangentT, desc.tangentT, 12);
    else
        absent(&desc.sizeTangentT, desc.tangentT);
    const int userData = UserDataSize(format);
    if (userData != 0)
        take(&desc.sizeUserData, desc.userData, compressed && userData == 4 ? 0 : 4 * userData);
    else
        absent(&desc.sizeUserData, desc.userData);
    if (!(format & kVertexFormatUseExactFormat) && offset > 16)
        offset = (offset + 15) & ~15;
    desc.actualVertexSize = offset;
    for (size_t i = 0; i < presentCount; ++i)
        *present[i] = offset;
}

}

std::uint64_t ComputeVertexFormat(std::uint64_t flags, int texCoords, const int* dimensions, int boneWeights, int userDataSize)
{
    // SupportsCompressedVertices is 1 in the reported caps, so the compressed flag stays.
    std::uint64_t format = flags & ~kVertexFormatUseExactFormat;
    if (boneWeights > 0)
        format |= 2ull << kVertexBoneWeightBit;
    format |= static_cast<std::uint64_t>(userDataSize) << kUserDataSizeBit;
    for (int t = 0; t < std::min(texCoords, kMaxTexCoords); ++t)
        format |= static_cast<std::uint64_t>(dimensions != nullptr ? dimensions[t] : 2) << (kTexCoordSizeBit + 3 * t);
    return format;
}

int VertexSize(std::uint64_t format)
{
    VertexDesc desc;
    FillVertexDesc(g_dummy, format, desc);
    return desc.actualVertexSize;
}

namespace {

std::vector<VertexElement> BuildVertexElements(std::uint64_t format, bool colorMesh, std::uint64_t flexFormat)
{
    std::vector<VertexElement> elements;
    VertexDesc desc;
    FillVertexDesc(g_dummy, format, desc);
    auto offsetOf = [&](const void* pointer) { return static_cast<UINT>(static_cast<const std::uint8_t*>(pointer) - g_dummy); };
    const bool compressed = (format & kVertexFormatCompressed) != 0;
    if (format & kVertexPosition)
        elements.push_back({ D3DDECLUSAGE_POSITION, 0, 0, DXGI_FORMAT_R32G32B32_FLOAT, 0 });
    if (desc.sizeBoneWeight != 0)
        elements.push_back({ D3DDECLUSAGE_BLENDWEIGHT, 0, 0, compressed ? DXGI_FORMAT_R16G16_SINT : DXGI_FORMAT_R32G32_FLOAT, offsetOf(desc.boneWeight) });
    if (desc.sizeBoneMatrixIndex != 0)
        elements.push_back({ D3DDECLUSAGE_BLENDINDICES, 0, 0, DXGI_FORMAT_B8G8R8A8_UNORM, offsetOf(desc.boneMatrixIndex) });
    if (desc.sizeNormal != 0)
        elements.push_back({ D3DDECLUSAGE_NORMAL, 0, 0, compressed ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_R32G32B32_FLOAT, offsetOf(desc.normal) });
    if (desc.sizeColor != 0)
        elements.push_back({ D3DDECLUSAGE_COLOR, 0, 0, DXGI_FORMAT_B8G8R8A8_UNORM, offsetOf(desc.color) });
    if (desc.sizeSpecular != 0)
        elements.push_back({ D3DDECLUSAGE_COLOR, 1, 0, DXGI_FORMAT_B8G8R8A8_UNORM, offsetOf(desc.specular) });
    static constexpr DXGI_FORMAT kFloats[] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT };
    for (int t = 0; t < kMaxTexCoords; ++t) {
        const int size = TexCoordSize(t, format);
        if (size >= 1 && size <= 4)
            elements.push_back({ D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(t), 0, kFloats[size], offsetOf(desc.texCoord[t]) });
    }
    if (desc.sizeTangentS != 0)
        elements.push_back({ D3DDECLUSAGE_TANGENT, 0, 0, DXGI_FORMAT_R32G32B32_FLOAT, offsetOf(desc.tangentS) });
    if (desc.sizeTangentT != 0)
        elements.push_back({ D3DDECLUSAGE_BINORMAL, 0, 0, DXGI_FORMAT_R32G32B32_FLOAT, offsetOf(desc.tangentT) });
    const int userData = UserDataSize(format);
    if (userData >= 1 && userData <= 4) {
        // Compressed vertices pack the tangent into the normal's four bytes.
        if (compressed && userData == 4)
            elements.push_back({ D3DDECLUSAGE_TANGENT, 0, 0, DXGI_FORMAT_R8G8B8A8_UINT, offsetOf(desc.normal) });
        else
            elements.push_back({ D3DDECLUSAGE_TANGENT, 0, 0, kFloats[userData], offsetOf(desc.userData) });
    }
    if (colorMesh)
        elements.push_back({ D3DDECLUSAGE_COLOR, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0 });
    // Flex deltas: the position with the wrinkle weight in w, then the normal.
    if (flexFormat != 0) {
        const bool wrinkle = (flexFormat & kVertexWrinkle) != 0;
        elements.push_back({ D3DDECLUSAGE_POSITION, 1, 2, wrinkle ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32G32B32_FLOAT, 0 });
        elements.push_back({ D3DDECLUSAGE_NORMAL, 1, 2, DXGI_FORMAT_R32G32B32_FLOAT, wrinkle ? 16u : 12u });
    }
    return elements;
}

}

const std::vector<VertexElement>& VertexElements(std::uint64_t format, bool colorMesh, std::uint64_t flexFormat)
{
    static std::map<std::tuple<std::uint64_t, bool, std::uint64_t>, std::vector<VertexElement>> layouts;
    const auto key = std::make_tuple(format, colorMesh, flexFormat);
    auto found = layouts.find(key);
    if (found == layouts.end())
        found = layouts.emplace(key, BuildVertexElements(format, colorMesh, flexFormat)).first;
    return found->second;
}

void BindMeshDeviceMethods(NativeClass& c)
{
    c.Bind("CreateStaticMesh", &CreateStaticMesh);
    c.Bind("DestroyStaticMesh", &DestroyStaticMesh);
}

void BindMeshMethods(NativeClass& c)
{
    c.Bind("GetDynamicMesh", &GetDynamicMesh);
    c.Bind("GetDynamicMeshEx", &GetDynamicMeshEx);
    c.Bind("GetFlexMesh", &GetFlexMesh);
    c.Bind("GetCurrentDynamicVBSize", &GetCurrentDynamicVBSize);
    c.Bind("GetMaxToRender", &GetMaxToRender);
    c.Bind("GetMaxVerticesToRender", &GetMaxVerticesToRender);
    c.Bind("GetMaxIndicesToRender", &GetMaxIndicesToRender);
    c.Bind("ComputeVertexDescription", &ComputeVertexDescription);
    c.Bind("GetVertexModifyBuilder", &GetVertexModifyBuilder);
}

}
