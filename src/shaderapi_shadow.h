#pragma once

#include "shaderapi_native.h"

#include <cstdint>
#include <type_traits>

namespace gmodpp::native {

class ShaderFile;

// IShaderShadow's state for one pass, from IShaderShadow::SetDefaultState to IShaderAPI::TakeSnapshot, in the SDK's
// enums (public/shaderapi/ishadershadow.h). Only 32- and 64-bit members, so equal states compare equal byte for byte.
struct ShadowState {
    const ShaderFile* vertexShader = nullptr;
    const ShaderFile* pixelShader = nullptr;
    std::uint64_t vertexFormat = 0;
    std::int32_t vertexStaticIndex = 0;
    std::int32_t pixelStaticIndex = 0;
    std::int32_t depthFunc = 3;
    std::int32_t depthWrite = 1;
    std::int32_t depthTest = 1;
    std::int32_t polyOffset = 0;
    std::int32_t colorWrite = 1;
    std::int32_t alphaWrite = 0;
    std::int32_t blend = 0;
    std::int32_t srcBlend = 1;
    std::int32_t dstBlend = 0;
    std::int32_t blendOp = 0;
    std::int32_t separateAlpha = 0;
    std::int32_t srcBlendAlpha = 1;
    std::int32_t dstBlendAlpha = 0;
    std::int32_t blendOpAlpha = 0;
    std::int32_t alphaTest = 0;
    std::int32_t alphaFunc = 6;
    // D3DRS_ALPHAREF, 0-255 (shaderapidx9.dll's AlphaFunc at 0x180028fd0 stores ref * 255).
    std::int32_t alphaRef = 178;
    std::int32_t culling = 1;
    std::int32_t polyModeFront = 2;
    std::int32_t polyModeBack = 2;
    std::int32_t srgbWrite = 0;
    std::int32_t srgbRead = 0;
    std::int32_t textures = 0;
    std::int32_t shadowFiltering = 0;
    std::int32_t alphaToCoverage = 0;
    std::int32_t fogMode = 0;
    std::int32_t disableFogGammaCorrection = 0;
    std::int32_t morphFormat = 0;
    std::int32_t drawFlags = 1;
    std::int32_t lighting = 0;
    std::int32_t vertexBlend = 0;
    std::int32_t constantColor = 0;
    std::int32_t specular = 0;
    std::int32_t customPixelPipe = 0;
    std::int32_t alphaPipe = 0;
    std::int32_t constantAlpha = 0;
    std::int32_t vertexAlpha = 0;
    std::int32_t diffuseMaterialSource = 0;
};
static_assert(std::has_unique_object_representations_v<ShadowState>, "snapshots are found by comparing bytes");

// The state of a snapshot from TakeSnapshot, or null for an unknown id.
const ShadowState* Snapshot(int id);

// Binds IShaderAPI's snapshot methods.
void BindSnapshotMethods(NativeClass& shaderApi);

}
