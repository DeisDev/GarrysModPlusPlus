#pragma once

#include "shaderapi_meshes.h"

#include <array>
#include <cstdint>

namespace gmodpp::native {

constexpr int kVertexFloatConstants = 256;
constexpr int kPixelFloatConstants = 224;
constexpr int kIntConstants = 16;
constexpr int kBoolConstants = 16;
constexpr int kSamplers = 16;
constexpr int kVertexSamplers = 4;
constexpr int kRenderTargets = 4;
// SHADER_RENDERTARGET_BACKBUFFER / SHADER_RENDERTARGET_DEPTHBUFFER and SHADER_RENDERTARGET_NONE (ishaderapi.h).
constexpr std::intptr_t kDefaultTarget = -1;
constexpr std::intptr_t kNoTarget = -2;

struct Float4 {
    float v[4];
};

// IShaderDynamicAPI's state, as the draws read it.
struct DynamicState {
    std::array<Float4, kVertexFloatConstants> vertexFloats = {};
    std::array<Float4, kPixelFloatConstants> pixelFloats = {};
    std::array<std::array<int, 4>, kIntConstants> vertexInts = {};
    std::array<std::array<int, 4>, kIntConstants> pixelInts = {};
    std::array<int, kBoolConstants> vertexBools = {};
    std::array<int, kBoolConstants> pixelBools = {};
    int vertexShaderIndex = -1;
    int pixelShaderIndex = 0;
    std::array<std::intptr_t, kSamplers> textures = {};
    std::array<std::intptr_t, kVertexSamplers> vertexTextures = {};
    std::array<bool, kSamplers> borderAddress = {};
    // Command 2004's mode per sampler (1 point min, mag and mip; 2 point mag).
    std::array<int, kSamplers> pointFilter = {};
    int anisotropy = 1;
    // SetRenderTargetEx: -1 is the back buffer for target 0 and no target for 1-3; the depth target applies to 0.
    std::array<std::intptr_t, kRenderTargets> colorTargets = { kDefaultTarget, kDefaultTarget, kDefaultTarget, kDefaultTarget };
    std::intptr_t depthTarget = kDefaultTarget;
    D3D12_VIEWPORT viewport = {};
    D3D12_RECT scissor = {};
    bool scissorEnabled = false;
    bool stencilEnable = false;
    int stencilFail = D3D12_STENCIL_OP_KEEP;
    int stencilDepthFail = D3D12_STENCIL_OP_KEEP;
    int stencilPass = D3D12_STENCIL_OP_KEEP;
    int stencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    int stencilRef = 0;
    unsigned stencilTestMask = 0xffffffff;
    unsigned stencilWriteMask = 0xffffffff;
    // MaterialCullMode_t: 0 culls counter-clockwise triangles (D3DCULL_CCW), 1 clockwise ones.
    int cullMode = 0;
    bool forceDepthFuncEquals = false;
    bool overrideDepthEnable = false;
    bool depthEnable = true;
    bool overrideAlphaWrite = false;
    bool alphaWriteEnable = true;
    bool overrideColorWrite = false;
    bool colorWriteEnable = true;
    // GMod's render.PushFilterMin/Mag (IShaderAPI slot 254): a D3DTEXTUREFILTERTYPE every texture is sampled with, 0 for
    // the texture's own.
    int minFilterOverride = 0;
    int magFilterOverride = 0;
    // GMod's render.OverrideBlend (slots 255 and 256), in ShaderBlendFactor_t and ShaderBlendOp_t values.
    struct BlendOverride {
        bool active = false;
        bool enable = false;
        int source = 1;
        int destination = 0;
        int op = 0;
    };
    BlendOverride blendOverride;
    BlendOverride alphaBlendOverride;
};

DynamicState& Dynamic();
// The enabled user clip planes in clip space, as shaderapidx9.dll commits them for vertex shaders (0x180012ec0);
// returns how many.
int UserClipPlanes(float planes[6][4]);
// The dynamic state a new device starts with (IShaderAPI::ResetRenderState).
void ResetDynamicState();

// CShaderAPIDx8::DrawMesh (GMod's IShaderAPIDX8 slot 266, shaderapidx9.dll 0x1800152c0): commits the transforms and
// hands the mesh to the bound material, whose passes call BeginPass and RenderPass.
void DrawMesh(const MeshDraw& mesh);
// The material IShaderAPI::Bind selected (an IMaterialInternal*).
void* BoundMaterial();

}
