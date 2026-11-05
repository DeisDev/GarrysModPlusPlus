#pragma once

#include "descriptor_heap.h"

#include <d3d12.h>

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace gmodpp::viewmodel_casters {

// The held weapon and hands as the lighting traces them: their pixels find their own triangles, whose positions last
// capture give their motion (the viewmodel's light history follows it, shaders/gi_temporal.hlsl), and with
// Settings::viewmodelSelfShadows they shadow each other. Their G-buffer draws' vertex shaders write each vertex's world
// position (translator::Variant::writePositions), so the triangles are exactly what the game drew: its skinning, flexes,
// flipped weapons and Lua attachments. The CPU copies their 16-bit indices, offset to where each mesh's vertices went.
// Entries are used in turn, one per viewmodel capture; the lighting builds the entry's BLAS and its own scene with it
// (Lighting::Run).
constexpr UINT kMaxVertices = 65536;
constexpr UINT kMaxTriangles = 65536;

// A viewmodel capture's triangles: positions (three floats per vertex), each vertex's position last frame (kNoPrevious
// where its mesh was not drawn then), indices (three 32-bit indices per triangle, Source's winding) to copy
// into the device-local buffer the shaders and the BLAS read, and a mid-grey albedo per triangle for rays that hit them.
struct Geometry {
    UINT entry = 0;
    UINT positionsView = DescriptorHeap::kInvalid;
    UINT previousView = DescriptorHeap::kInvalid;
    UINT indicesView = DescriptorHeap::kInvalid;
    UINT albedoView = DescriptorHeap::kInvalid;
    UINT vertexCount = 0;
    UINT triangleCount = 0;
};

// Another capture's triangles of the same viewmodel, whose BLAS its own lighting built: some weapons (ARC9) change the
// depth range or copy the screen between their parts, so a frame's viewmodel arrives in several captures.
struct Part {
    UINT entry = 0;
    D3D12_GPU_VIRTUAL_ADDRESS blas = 0;
    UINT positionsView = DescriptorHeap::kInvalid;
    UINT indicesView = DescriptorHeap::kInvalid;
    UINT albedoView = DescriptorHeap::kInvalid;
    UINT triangleCount = 0;
};

// Every coordinate of a vertex without a previous position: far beyond any map, so its motion is never taken
// (shaders/primary.hlsl).
constexpr float kNoPrevious = 1e9f;

// A draw's mesh: its vertex buffer view, its 16-bit indices on the CPU and on the GPU, the (first index, index count)
// lists drawn, and whether it is drawn with clockwise culling, as mirrored models are (Source's front faces are
// clockwise; MaterialCullMode_t 1 is D3DCULL_CW).
struct Mesh {
    D3D12_GPU_VIRTUAL_ADDRESS vertices = 0;
    UINT vertexBytes = 0;
    UINT stride = 0;
    D3D12_GPU_VIRTUAL_ADDRESS indexBuffer = 0;
    const std::uint16_t* indexData = nullptr;
    std::span<const std::pair<int, int>> primitives;
    bool triangleList = false;
    bool mirrored = false;
};

// Where a draw's vertex shader writes its positions: the buffer's raw UAV, its first vertex and the end of its range.
struct Positions {
    UINT uav;
    UINT firstVertex;
    UINT endVertex;
};

// Starts recording when a viewmodel capture begins with real-time lighting on.
void BeginCapture();
bool Recording();
// Records a G-buffer draw of the viewmodel; nothing for draws it cannot take (not triangle lists, no CPU indices, full).
std::optional<Positions> RecordDraw(const Mesh& mesh);
// Ends the capture's recording; its triangles, if it drew any.
std::optional<Geometry> EndCapture();
// Records into `list` the copies of the indices and of the previous positions, and the BLAS build of `geometry`, lit in
// `frame`; its address, or 0.
D3D12_GPU_VIRTUAL_ADDRESS RecordGeometry(ID3D12GraphicsCommandList4* list, const Geometry& geometry, std::uint64_t frame);
// The viewmodel's other parts for `geometry`'s lighting in `frame`: the frame's earlier captures, and last frame's for
// the parts not captured yet, so the parts shadow each other (one frame late where the later part comes later).
std::vector<Part> OtherParts(const Geometry& geometry, std::uint64_t frame);
// The entry's geometry is in use until `fence` reaches `value`.
void MarkUsed(UINT entry, ID3D12Fence* fence, std::uint64_t value);

}
