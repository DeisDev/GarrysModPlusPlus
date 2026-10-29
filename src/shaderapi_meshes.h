#pragma once

#include "shaderapi_device.h"

#include <cstdint>
#include <vector>

namespace gmodpp::native {

// VertexFormat_t flags and fields (public/materialsystem/imaterial.h).
constexpr std::uint64_t kVertexPosition = 0x0001;
constexpr std::uint64_t kVertexNormal = 0x0002;
constexpr std::uint64_t kVertexColor = 0x0004;
constexpr std::uint64_t kVertexSpecular = 0x0008;
constexpr std::uint64_t kVertexTangentS = 0x0010;
constexpr std::uint64_t kVertexTangentT = 0x0020;
constexpr std::uint64_t kVertexWrinkle = 0x0040;
constexpr std::uint64_t kVertexBoneIndex = 0x0080;
constexpr std::uint64_t kVertexFormatVertexShader = 0x0100;
constexpr std::uint64_t kVertexFormatUseExactFormat = 0x0200;
constexpr std::uint64_t kVertexFormatCompressed = 0x0400;
constexpr int kVertexBoneWeightBit = 11;
constexpr int kUserDataSizeBit = 14;
constexpr int kTexCoordSizeBit = 17;
constexpr int kMaxTexCoords = 8;

constexpr int BoneWeights(std::uint64_t format) { return static_cast<int>((format >> kVertexBoneWeightBit) & 7); }
constexpr int UserDataSize(std::uint64_t format) { return static_cast<int>((format >> kUserDataSizeBit) & 7); }
constexpr int TexCoordSize(int index, std::uint64_t format) { return static_cast<int>((format >> (kTexCoordSizeBit + 3 * index)) & 7); }

// shaderapidx9.dll's CMeshMgr::ComputeVertexFormat (0x1800081f0).
std::uint64_t ComputeVertexFormat(std::uint64_t flags, int texCoords, const int* dimensions, int boneWeights, int userDataSize);
// A vertex's size in bytes (VertexDesc_t::m_ActualVertexSize from 0x180005a60).
int VertexSize(std::uint64_t format);

// One element of a vertex stream, as shaderapidx9.dll's vertex declaration (0x180030360) describes it.
struct VertexElement {
    BYTE usage;
    BYTE usageIndex;
    BYTE stream;
    DXGI_FORMAT format;
    UINT offset;
};
std::vector<VertexElement> VertexElements(std::uint64_t format, bool colorMesh);

// What IShaderAPI::RenderPass draws: the mesh IMesh::Draw handed to the material.
struct MeshDraw {
    std::uint64_t vertexFormat = 0;
    // Streams 0 (vertices), 1 (static prop colour mesh) and 2 (flex deltas); an empty view is absent.
    D3D12_VERTEX_BUFFER_VIEW streams[3] = {};
    D3D12_INDEX_BUFFER_VIEW indices = {};
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    // (first index, index count) per primitive list; for unindexed meshes, vertices.
    std::vector<std::pair<int, int>> primitives;
    int vertexCount = 0;
    ID3D12Resource* buffers[3] = {};
    // The vertices' CPU copy when there is one (developer diagnostics).
    const std::uint8_t* vertexData = nullptr;
};

// IShaderDevice and IShaderAPI's mesh methods.
void BindMeshDeviceMethods(NativeClass& device);
void BindMeshMethods(NativeClass& shaderApi);

}
