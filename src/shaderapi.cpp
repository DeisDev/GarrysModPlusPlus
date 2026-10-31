#include "shaderapi.h"

#include "log.h"
#include "native_lighting.h"
#include "shaderapi_capture.h"
#include "shaderapi_commands.h"
#include "shaderapi_device.h"
#include "shaderapi_pipelines.h"
#include "shaderapi_shadow.h"
#include "shaderapi_textures.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gmodpp::native {

namespace {

constexpr size_t kShaderApiSlots = 289;
// IShaderUtil (materialsystem.dll), in the SDK's order (shaderapi_slots.cpp).
constexpr unsigned kShaderUtilGetConfig = 5;
constexpr unsigned kShaderUtilBindStandardTexture = 9;
constexpr unsigned kShaderUtilInFlashlightMode = 14;
constexpr unsigned kShaderUtilNoteAnisotropicLevel = 15;
constexpr unsigned kShaderUtilInEditorMode = 16;
constexpr unsigned kShaderUtilDrawClearBufferQuad = 19;
constexpr unsigned kShaderUtilOnFlushBufferedPrimitives = 25;
// MaterialSystem_Config_t (public/materialsystem/materialsystem_config.h): bReverseDepth and m_bSuppressRendering,
// the offsets shaderapidx9.dll reads (0x18002a170, 0x1800152c0).
constexpr size_t kConfigReverseDepth = 0x3d;
constexpr size_t kConfigSuppressRendering = 0x4b;
constexpr size_t kConfigNormalSlopeScaleDepthBias = 0x54;
constexpr size_t kConfigNormalDepthBias = 0x58;
constexpr size_t kConfigDecalSlopeScaleDepthBias = 0x5c;
constexpr size_t kConfigDecalDepthBias = 0x60;
// IMaterialInternal slots shaderapidx9.dll's DrawMesh (0x1800152c0) and CommitStateChanges (0x180012c70) call:
// IMaterial::IsVertexLit and GetVertexFormat, and IMaterialInternal::DrawMesh(VertexCompressionType_t).
constexpr unsigned kMaterialIsVertexLit = 19;
constexpr unsigned kMaterialGetVertexFormat = 20;
constexpr unsigned kMaterialDrawMesh = 72;
// MaterialMatrixMode_t.
constexpr int kMatrixView = 0;
constexpr int kMatrixProjection = 1;
constexpr int kMatrixModel = 10;
constexpr int kMatrixModes = 11;
// MaterialFogMode_t.
constexpr int kFogLinearBelowFogZ = 2;
// Vertex shader constants shaderapidx9.dll sets (materialsystem/stdshaders/common_vs_fxc.h names them).
constexpr int kVertexConstants1 = 1;
constexpr int kVertexEyePosWaterZ = 2;
constexpr int kVertexFlexScale = 3;
constexpr int kVertexModelViewProj = 4;
constexpr int kVertexViewProj = 8;
constexpr int kVertexModelViewProjZ = 12;
constexpr int kVertexViewProjZ = 13;
constexpr int kVertexFogParams = 16;
constexpr int kVertexAmbientCube = 21;
constexpr int kVertexModel = 58;
constexpr int kPixelLinearFogColor = 29;
constexpr int kPixelLightScale = 30;
// The depth range materials write to destination alpha: 192 without float HDR (ResetRenderState, 0x18001bf40); c29.w is
// its reciprocal and depth feathering divides it by the blend scale (0x180022230, 0x18001dfd0).
constexpr float kDestAlphaDepthRange = 192.0f;
constexpr float kOverbright = 2.0f;
// ShaderFogMode_t.
constexpr int kShaderFogOverbright = 1;
constexpr int kShaderFogBlack = 2;
constexpr int kShaderFogGrey = 3;
constexpr int kShaderFogColor = 4;
constexpr int kShaderFogWhite = 5;
constexpr int kStandardTextures = 128;
constexpr int kRenderParameters = 64;

struct Matrix {
    float m[4][4];
};

constexpr Matrix kIdentity = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } } };

Matrix Multiply(const Matrix& a, const Matrix& b)
{
    Matrix r = {};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    }
    return r;
}

Matrix Transpose(const Matrix& a)
{
    Matrix r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[j][i];
    }
    return r;
}

bool Invert(const Matrix& in, Matrix& out)
{
    const float* a = &in.m[0][0];
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (det == 0.0f)
        return false;
    for (int i = 0; i < 16; ++i)
        (&out.m[0][0])[i] = inv[i] / det;
    return true;
}

struct ShaderApiObject : NativeObject {
    std::recursive_mutex mutex;
    bool mutexEnabled = false;
};

// The parts of CShaderAPIDx8's dynamic state that only this file reads.
struct ApiState {
    std::array<std::vector<Matrix>, kMatrixModes> stacks;
    int matrixMode = kMatrixView;
    // Bit 0 per mode: the vertex shader constants derived from it are stale (shaderapidx9.dll's dirty bytes at
    // 0x12f3 + mode).
    std::array<bool, kMatrixModes> dirty = { true, true, true, true, true, true, true, true, true, true, true };
    float eye[4] = { 0, 0, 0, 1 };
    void* material = nullptr;
    const MeshDraw* renderMesh = nullptr;
    int snapshot = -1;
    int boneWeights = 0;
    int fogMode = 0;
    float fogStart = 0;
    float fogEnd = 0;
    float fogZ = 0;
    float fogMaxDensity = 1;
    // The register the last SetPixelShaderFogParams wrote, rewritten when a pass's fog mode is applied.
    int pixelFogRegister = -1;
    unsigned char fogColor[3] = {};
    float lightScale[4] = { 1, 1, 1, 1 };
    // D3DRS_TEXTUREFACTOR, which only fixed-function texture stages read.
    unsigned textureFactor = 0xffffffff;
    Float4 ambientCube[6] = {};
    unsigned clearColor = 0xff000000;
    std::array<std::intptr_t, kStandardTextures> standardTextures = {};
    std::array<int, kRenderParameters> intParameters = {};
    std::array<float, kRenderParameters> floatParameters = {};
    std::array<std::array<float, 3>, kRenderParameters> vectorParameters = {};
    bool linearFramebuffer = false;
    std::intptr_t fullScreenTexture = 0;
    D3D12_VIEWPORT viewport = {};
    int viewportRaw[4] = {};
};

// LightDesc_t (public/materialsystem/lightdesc.h), as SetLight receives it and GetLight returns it.
struct LightDesc {
    int type;
    float color[3];
    float position[3];
    float direction[3];
    float range;
    float falloff;
    float attenuation[3];
    float theta;
    float phi;
    float thetaDot;
    float phiDot;
    unsigned flags;
    float oneOverThetaDotMinusPhiDot;
    float rangeSquared;
};
static_assert(sizeof(LightDesc) == 0x58);

// The Direct3D 9 light SetLight (0x18001ea70) makes of a LightDesc_t: D3DLIGHT_POINT 1, SPOT 2, DIRECTIONAL 3.
struct DeviceLight {
    int type = 0;
    float color[3] = {};
    float position[3] = {};
    float direction[3] = {};
    float falloff = 0;
    float attenuation[3] = {};
    float theta = 0;
    float phi = 0;
};

constexpr int kMaxLights = 4;
constexpr int kMaxBones = 53;
constexpr int kVertexLights = 27;
constexpr int kD3DLightPoint = 1;
constexpr int kD3DLightSpot = 2;
constexpr int kD3DLightDirectional = 3;

constexpr int kClipPlanes = 6;
// D3DQUERYTYPE_OCCLUSION queries in flight at once.
constexpr UINT kOcclusionQueries = 4096;

struct ClipState {
    // SetClipPlane (0x18001dc20) stores (a, b, c, -d).
    float planes[kClipPlanes][4] = {};
    unsigned enabled = 0;
    // EnableUserClipTransformOverride and UserClipTransform (0x1800157c0, 0x1800224a0): a world-to-view matrix, stored
    // transposed like the transforms, used for the clip planes instead of the view matrix.
    bool transformOverride = false;
    Matrix userTransform = {};
};

struct OcclusionQueries {
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> results;
    const std::uint64_t* mapped = nullptr;
    std::vector<UINT> free;
    UINT next = 0;
};

// An occlusion query object: its slot and the fence value its result is resolved by.
struct OcclusionQuery {
    UINT slot = 0;
    std::uint64_t fence = 0;
    bool ended = false;
};

struct LightingState {
    std::array<LightDesc, kMaxLights> descs = {};
    std::array<DeviceLight, kMaxLights> lights = {};
    std::array<bool, kMaxLights> enabled = {};
    std::array<std::array<float, 12>, kMaxBones> bones = {};
    int maxBone = 0;
};

ApiState g_api;
DynamicState g_dynamic;
LightingState g_lighting;
ClipState g_clip;

// SetFlashlightStateEx (0x18001e200) copies FlashlightState_t field by field: 0x98 bytes in GMod.
constexpr size_t kFlashlightStateSize = 0x98;

struct FlashlightState {
    std::array<std::uint8_t, kFlashlightStateSize> state = {};
    Matrix worldToTexture = {};
    void* depthTexture = nullptr;
};

FlashlightState g_flashlight;
OcclusionQueries g_queries;

// The enabled lights in shaderapidx9.dll's order (0x180020ea0): spot, point, then directional, stable.
std::vector<int> SortedLights()
{
    std::vector<int> order;
    for (int i = 0; i < kMaxLights; ++i) {
        if (g_lighting.enabled[static_cast<size_t>(i)])
            order.push_back(i);
    }
    auto rank = [](int i) {
        const int type = g_lighting.lights[static_cast<size_t>(i)].type;
        return type == kD3DLightSpot ? 0 : type == kD3DLightPoint ? 1 : 2;
    };
    std::ranges::stable_sort(order, {}, rank);
    return order;
}

template <class Fn>
Fn UtilSlot(unsigned slot)
{
    return reinterpret_cast<Fn>((*static_cast<void***>(GetBackend().shaderUtil))[slot]);
}

template <class Fn>
Fn MaterialSlot(void* material, unsigned slot)
{
    return reinterpret_cast<Fn>((*static_cast<void***>(material))[slot]);
}

const std::uint8_t* Config()
{
    return GetBackend().shaderUtil != nullptr ? UtilSlot<const std::uint8_t* (*)(void*)>(kShaderUtilGetConfig)(GetBackend().shaderUtil) : nullptr;
}

bool ReverseDepth()
{
    const std::uint8_t* config = Config();
    return config != nullptr && config[kConfigReverseDepth] != 0;
}

std::vector<Matrix>& Stack(int mode)
{
    std::vector<Matrix>& stack = g_api.stacks[static_cast<size_t>(mode)];
    if (stack.empty())
        stack.push_back(kIdentity);
    return stack;
}

Matrix& Top(int mode) { return Stack(mode).back(); }

void MatrixChanged()
{
    const int mode = g_api.matrixMode;
    if (mode < 0 || mode >= kMatrixModes)
        return;
    g_api.dirty[static_cast<size_t>(mode)] = true;
    if (mode == kMatrixView) {
        Matrix inverse;
        if (Invert(Top(kMatrixView), inverse)) {
            g_api.eye[0] = inverse.m[3][0];
            g_api.eye[1] = inverse.m[3][1];
            g_api.eye[2] = inverse.m[3][2];
        }
    }
}

void SetVertexFloats(int first, const float* values, int count)
{
    if (first < 0 || first + count > kVertexFloatConstants)
        return;
    std::memcpy(&g_dynamic.vertexFloats[static_cast<size_t>(first)], values, static_cast<size_t>(count) * sizeof(Float4));
    ++g_dynamic.vertexFloatsVersion;
}

void SetPixelFloats(int first, const float* values, int count)
{
    if (first < 0 || first + count > kPixelFloatConstants)
        return;
    std::memcpy(&g_dynamic.pixelFloats[static_cast<size_t>(first)], values, static_cast<size_t>(count) * sizeof(Float4));
    ++g_dynamic.pixelFloatsVersion;
}

void SetVertexMatrixRows(int first, const Matrix& matrix, int rows)
{
    const Matrix transposed = Transpose(matrix);
    SetVertexFloats(first, &transposed.m[0][0], rows);
}

// CShaderAPIDx8's vertex shader transforms (0x180013540 with 0x180020820 and 0x180020650): c8-c11 view x projection
// and c13 its z row, c4-c7 model x view x projection and c12 its z row, c58-c60 the model matrix when not skinning,
// and on a view change c16 (fog) and c2 (eye position, water height). Only stale ones are uploaded, as there.
void CommitVertexShaderTransforms()
{
    const bool view = g_api.dirty[kMatrixView];
    const bool projection = g_api.dirty[kMatrixProjection];
    const bool model = g_api.dirty[kMatrixModel] && g_api.boneWeights <= 0;
    if (!view && !projection && !model)
        return;
    const Matrix& viewMatrix = Top(kMatrixView);
    const Matrix& projectionMatrix = Top(kMatrixProjection);
    if (view) {
        const float range = g_api.fogEnd != g_api.fogStart ? 1.0f / (g_api.fogEnd - g_api.fogStart) : 1.0f;
        const float fog[4] = { g_api.fogEnd * range, 1.0f, 1.0f - std::clamp(g_api.fogMaxDensity, 0.0f, 1.0f), range };
        SetVertexFloats(kVertexFogParams, fog, 1);
        const float eye[4] = { g_api.eye[0], g_api.eye[1], g_api.eye[2], g_api.fogZ };
        SetVertexFloats(kVertexEyePosWaterZ, eye, 1);
    }
    const Matrix viewProjection = Multiply(viewMatrix, projectionMatrix);
    if (view || projection) {
        SetVertexMatrixRows(kVertexViewProj, viewProjection, 4);
        SetVertexFloats(kVertexViewProjZ, &Transpose(viewProjection).m[2][0], 1);
    }
    const Matrix modelViewProjection = Multiply(Top(kMatrixModel), viewProjection);
    SetVertexMatrixRows(kVertexModelViewProj, modelViewProjection, 4);
    SetVertexFloats(kVertexModelViewProjZ, &Transpose(modelViewProjection).m[2][0], 1);
    if (model)
        SetVertexMatrixRows(kVertexModel, Top(kMatrixModel), 3);
    g_api.dirty[kMatrixView] = false;
    g_api.dirty[kMatrixProjection] = false;
    g_api.dirty[kMatrixModel] = false;
}

// CommitVertexShaderLighting (0x180013190) at DX level 95: five registers per light from c27 in sorted order, then
// i0 = (light count, 0, 1, 0) and b0-b3 = which of them exist.
void CommitVertexShaderLighting()
{
    const std::vector<int> order = SortedLights();
    int reg = kVertexLights;
    for (int index : order) {
        const DeviceLight& light = g_lighting.lights[static_cast<size_t>(index)];
        float registers[5][4] = {
            { light.color[0], light.color[1], light.color[2], light.type == kD3DLightDirectional ? 1.0f : 0.0f },
            { light.direction[0], light.direction[1], light.direction[2], light.type == kD3DLightSpot ? 1.0f : 0.0f },
            { light.position[0], light.position[1], light.position[2], 1.0f },
            { 0.0f, 1.0f, 1.0f, 1.0f },
            { light.attenuation[0], light.attenuation[1], light.attenuation[2], 0.0f },
        };
        if (light.type == kD3DLightSpot) {
            const float cosTheta = std::cos(light.theta * 0.5f);
            const float cosPhi = std::cos(light.phi * 0.5f);
            registers[3][0] = light.falloff;
            registers[3][1] = cosTheta;
            registers[3][2] = cosPhi;
            registers[3][3] = cosTheta <= cosPhi ? 0.0f : 1.0f / (cosTheta - cosPhi);
        }
        SetVertexFloats(reg, &registers[0][0], 5);
        reg += 5;
    }
    g_dynamic.vertexInts[0] = { static_cast<int>(order.size()), 0, 1, 0 };
    for (int i = 0; i < kMaxLights; ++i)
        g_dynamic.vertexBools[static_cast<size_t>(i)] = i < static_cast<int>(order.size()) ? 1 : 0;
}

// The parts of CommitStateChanges (0x180012c70) that vertex shaders need; fixed-function draws are not supported.
void CommitStateChanges()
{
    void* material = g_api.material;
    const bool vertexShader = material == nullptr || (MaterialSlot<std::uint64_t (*)(void*)>(material, kMaterialGetVertexFormat)(material) & kVertexFormatVertexShader) != 0;
    if (!vertexShader) {
        static std::set<std::string> logged;
        if (logged.size() < 50 && logged.insert(BoundMaterialName()).second)
            log::Info("Native: fixed-function material '{}' was drawn; fixed-function rendering is not implemented", BoundMaterialName());
        return;
    }
    CommitVertexShaderTransforms();
    if (material != nullptr && MaterialSlot<bool (*)(void*)>(material, kMaterialIsVertexLit)(material))
        CommitVertexShaderLighting();
}

// SetViewports (0x1800209d0): the size is clamped to render target 0's.
void SetViewports(NativeObject*, int count, const int* viewports)
{
    if (count != 1 || viewports == nullptr)
        return;
    UINT width = 0;
    UINT height = 0;
    TargetSize(width, height);
    int w = viewports[3];
    int h = viewports[4];
    if (static_cast<UINT>(w) > width && width > 0)
        w = static_cast<int>(width);
    if (static_cast<UINT>(h) > height && height > 0)
        h = static_cast<int>(height);
    float depth[2];
    std::memcpy(depth, viewports + 5, sizeof(depth));
    g_api.viewportRaw[0] = viewports[1];
    g_api.viewportRaw[1] = viewports[2];
    g_api.viewportRaw[2] = w;
    g_api.viewportRaw[3] = h;
    g_dynamic.viewport = { static_cast<float>(viewports[1]), static_cast<float>(viewports[2]), static_cast<float>(w), static_cast<float>(h), depth[0], depth[1] };
    realtime::OnViewport(depth[0], depth[1]);
}

int GetViewports(NativeObject*, int* viewports, int max)
{
    if (viewports != nullptr && max >= 1) {
        viewports[0] = 1;
        std::memcpy(viewports + 1, g_api.viewportRaw, sizeof(g_api.viewportRaw));
        std::memcpy(viewports + 5, &g_dynamic.viewport.MinDepth, sizeof(float));
        std::memcpy(viewports + 6, &g_dynamic.viewport.MaxDepth, sizeof(float));
    }
    return 1;
}

// CurrentTime (0x180014ce0) returns a float copy of tier0's Plat_FloatTime.
double CurrentTime(NativeObject*)
{
    using FloatTimeFn = double (*)();
    static const FloatTimeFn floatTime = reinterpret_cast<FloatTimeFn>(GetProcAddress(GetModuleHandleW(L"tier0.dll"), "Plat_FloatTime"));
    return floatTime != nullptr ? static_cast<float>(floatTime()) : 0.0;
}
int GetSceneFogMode(NativeObject*) { return g_api.fogMode; }

void GetSceneFogColor(NativeObject*, unsigned char* rgb)
{
    std::memcpy(rgb, g_api.fogColor, 3);
}

void MatrixMode(NativeObject*, int mode) { g_api.matrixMode = mode; }

void PushMatrix(NativeObject*)
{
    if (g_api.matrixMode >= 0 && g_api.matrixMode < kMatrixModes) {
        std::vector<Matrix>& stack = Stack(g_api.matrixMode);
        stack.push_back(stack.back());
    }
}

void PopMatrix(NativeObject*)
{
    if (g_api.matrixMode < 0 || g_api.matrixMode >= kMatrixModes)
        return;
    std::vector<Matrix>& stack = Stack(g_api.matrixMode);
    if (stack.size() > 1)
        stack.pop_back();
    MatrixChanged();
}

void LoadMatrix(NativeObject*, const float* m)
{
    if (g_api.matrixMode < 0 || g_api.matrixMode >= kMatrixModes)
        return;
    std::memcpy(&Top(g_api.matrixMode), m, sizeof(Matrix));
    MatrixChanged();
}

void MultMatrix(NativeObject*, const float* m)
{
    if (g_api.matrixMode < 0 || g_api.matrixMode >= kMatrixModes)
        return;
    Matrix matrix;
    std::memcpy(&matrix, m, sizeof(matrix));
    Matrix& top = Top(g_api.matrixMode);
    top = Multiply(top, matrix);
    MatrixChanged();
}

void MultMatrixLocal(NativeObject*, const float* m)
{
    if (g_api.matrixMode < 0 || g_api.matrixMode >= kMatrixModes)
        return;
    Matrix matrix;
    std::memcpy(&matrix, m, sizeof(matrix));
    Matrix& top = Top(g_api.matrixMode);
    top = Multiply(matrix, top);
    MatrixChanged();
}

// The helpers below build D3DX matrices (formulas from the D3DXMatrix* reference) and left-multiply them onto the
// current stack like ID3DXMatrixStack's *Local methods, as shaderapidx9.dll does.
void MultLocal(NativeObject* self, const Matrix& matrix) { MultMatrixLocal(self, &matrix.m[0][0]); }

// 0x180019a20. The arguments reach D3DXMatrixOrthoOffCenterRH as (l, r, b, t) = (1st, 3rd, 2nd, 4th).
void Ortho(NativeObject* self, double left, double top, double right, double bottom, double zNear, double zFar)
{
    const float l = static_cast<float>(left), r = static_cast<float>(right), b = static_cast<float>(top), t = static_cast<float>(bottom);
    const float zn = static_cast<float>(zNear), zf = static_cast<float>(zFar);
    MultLocal(self, { { { 2 / (r - l), 0, 0, 0 }, { 0, 2 / (t - b), 0, 0 }, { 0, 0, 1 / (zn - zf), 0 }, { (l + r) / (l - r), (t + b) / (b - t), zn / (zn - zf), 1 } } });
}

Matrix PerspectiveOffCenter(float l, float r, float b, float t, float zn, float zf)
{
    return { { { 2 * zn / (r - l), 0, 0, 0 }, { 0, 2 * zn / (t - b), 0, 0 }, { (l + r) / (r - l), (t + b) / (t - b), zf / (zn - zf), -1 },
        { 0, 0, zn * zf / (zn - zf), 0 } } };
}

// 0x18001a120: D3DXMatrixPerspectiveRH with the near plane's width from the horizontal field of view.
void PerspectiveX(NativeObject* self, double fovx, double aspect, double zNear, double zFar)
{
    const float width = static_cast<float>(std::tan(fovx * 0.0087266462599716) * (zNear + zNear));
    const float height = static_cast<float>(width / aspect);
    MultLocal(self, PerspectiveOffCenter(-width / 2, width / 2, -height / 2, height / 2, static_cast<float>(zNear), static_cast<float>(zFar)));
}

// 0x180019f00: the window's edges are fractions of the full frustum, 0 the near plane's minimum and 1 its maximum.
void PerspectiveOffCenterX(NativeObject* self, double fovx, double aspect, double zNear, double zFar, double bottom, double top, double left,
    double right)
{
    const float width = static_cast<float>(std::tan(fovx * 0.0087266462599716) * (zNear + zNear));
    const double halfWidth = width * 0.5f;
    const double halfHeight = static_cast<float>(width / aspect) * 0.5f;
    auto edge = [](double half, double fraction) { return static_cast<float>(half * fraction - (1.0 - fraction) * half); };
    MultLocal(self, PerspectiveOffCenter(edge(halfWidth, left), edge(halfWidth, right), edge(halfHeight, bottom), edge(halfHeight, top),
        static_cast<float>(zNear), static_cast<float>(zFar)));
}

// 0x18001a250. Both offsets subtract the viewport's x, as shaderapidx9.dll does.
void PickMatrix(NativeObject* self, int x, int y, int width, int height)
{
    const D3D12_VIEWPORT& viewport = g_dynamic.viewport;
    const float viewportX = static_cast<float>(static_cast<int>(viewport.TopLeftX));
    const float inverseWidth = 1.0f / static_cast<float>(static_cast<int>(viewport.Width));
    const float inverseHeight = 1.0f / static_cast<float>(static_cast<int>(viewport.Height));
    const float sx = 1.0f / (static_cast<float>(width + width) * inverseWidth);
    const float sy = 1.0f / (static_cast<float>(height + height) * inverseHeight);
    const float tx = ((static_cast<float>(x) - viewportX) * -4.0f * inverseWidth + 2.0f) * sx;
    const float ty = ((static_cast<float>(y) - viewportX) * -4.0f * inverseHeight + 2.0f) * sy;
    MultLocal(self, { { { sx + sx, 0, 0, 0 }, { 0, sy + sy, 0, 0 }, { 0, 0, 1, 0 }, { tx, ty, 0, 1 } } });
}

// 0x18001d280: RotateAxisLocal, the angle in degrees.
void Rotate(NativeObject* self, float angle, float x, float y, float z)
{
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length == 0)
        return;
    x /= length;
    y /= length;
    z /= length;
    const float radians = angle * 0.017453292f;
    const float c = std::cos(radians), s = std::sin(radians), t = 1 - c;
    MultLocal(self, { { { t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0 }, { t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0 },
                        { t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0 }, { 0, 0, 0, 1 } } });
}

// 0x180021c30, 0x18001d380 and 0x18001d450: TranslateLocal and ScaleLocal.
void Translate(NativeObject* self, float x, float y, float z) { MultLocal(self, { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { x, y, z, 1 } } }); }
void Scale(NativeObject* self, float x, float y, float z) { MultLocal(self, { { { x, 0, 0, 0 }, { 0, y, 0, 0 }, { 0, 0, z, 0 }, { 0, 0, 0, 1 } } }); }
void ScaleXY(NativeObject* self, float x, float y) { Scale(self, x, y, 1); }

void GetMatrix(NativeObject*, int mode, float* destination)
{
    if (mode >= 0 && mode < kMatrixModes)
        std::memcpy(destination, &Top(mode), sizeof(Matrix));
}

void LoadIdentity(NativeObject*)
{
    if (g_api.matrixMode < 0 || g_api.matrixMode >= kMatrixModes)
        return;
    Top(g_api.matrixMode) = kIdentity;
    MatrixChanged();
}

// LoadCameraToWorld (0x180018f90): the inverse of the view matrix without its translation.
void LoadCameraToWorld(NativeObject*)
{
    if (g_api.matrixMode < 0 || g_api.matrixMode >= kMatrixModes)
        return;
    Matrix inverse;
    if (!Invert(Top(kMatrixView), inverse))
        return;
    inverse.m[3][0] = inverse.m[3][1] = inverse.m[3][2] = 0;
    Top(g_api.matrixMode) = inverse;
    MatrixChanged();
}

void SetVertexShaderConstant(NativeObject*, int first, const float* values, int count, bool) { SetVertexFloats(first, values, count); }
void SetPixelShaderConstant(NativeObject*, int first, const float* values, int count, bool) { SetPixelFloats(first, values, count); }

void SetBooleanVertexShaderConstant(NativeObject*, int first, const int* values, int count, bool)
{
    for (int i = 0; i < count && first + i < kBoolConstants; ++i)
        g_dynamic.vertexBools[static_cast<size_t>(first + i)] = values[i];
}

void SetIntegerVertexShaderConstant(NativeObject*, int first, const int* values, int count, bool)
{
    for (int i = 0; i < count && first + i < kIntConstants; ++i)
        std::memcpy(g_dynamic.vertexInts[static_cast<size_t>(first + i)].data(), values + i * 4, 4 * sizeof(int));
}

void SetBooleanPixelShaderConstant(NativeObject*, int first, const int* values, int count, bool)
{
    for (int i = 0; i < count && first + i < kBoolConstants; ++i)
        g_dynamic.pixelBools[static_cast<size_t>(first + i)] = values[i];
}

void SetIntegerPixelShaderConstant(NativeObject*, int first, const int* values, int count, bool)
{
    for (int i = 0; i < count && first + i < kIntConstants; ++i)
        std::memcpy(g_dynamic.pixelInts[static_cast<size_t>(first + i)].data(), values + i * 4, 4 * sizeof(int));
}

// SetDefaultState (0x18001dd80): the shader indices go back to -1 (vertex, read as 0) and 0 (pixel).
void SetDefaultState(NativeObject*)
{
    g_dynamic.vertexShaderIndex = -1;
    g_dynamic.pixelShaderIndex = 0;
}

void GetWorldSpaceCameraPosition(NativeObject*, float* position) { std::memcpy(position, g_api.eye, 3 * sizeof(float)); }
int GetCurrentNumBones(NativeObject*) { return g_api.boneWeights; }
void SetNumBoneWeights(NativeObject*, int count) { g_api.boneWeights = count; }
int GetCurrentFogType(NativeObject*) { return g_api.fogMode; }
void SetVertexShaderIndex(NativeObject*, int index) { g_dynamic.vertexShaderIndex = index; }
void SetPixelShaderIndex(NativeObject*, int index) { g_dynamic.pixelShaderIndex = index; }

// 0x18001f160: no fog when the scene has none or the pass's ShaderFogMode_t is disabled ($nofog).
void WritePixelFogParams(int reg, int shaderFogMode)
{
    g_api.pixelFogRegister = reg;
    float fog[4] = { 0, g_api.fogZ, 1, 0 };
    if (g_api.fogMode != 0 && shaderFogMode != 0) {
        const float range = g_api.fogEnd != g_api.fogStart ? 1.0f / (g_api.fogEnd - g_api.fogStart) : 1.0f;
        fog[0] = g_api.fogStart * range;
        fog[2] = std::clamp(g_api.fogMaxDensity, 0.0f, 1.0f);
        fog[3] = range;
        if (g_api.fogMode == kFogLinearBelowFogZ) {
            fog[0] = 0;
            fog[2] = 1;
        }
    }
    SetPixelFloats(reg, fog, 1);
}

// SetPixelShaderFogParams (0x18001f290): with the fog mode of the pass being drawn.
void SetPixelShaderFogParams(NativeObject*, int reg)
{
    const ShadowState* shadow = Snapshot(g_api.snapshot);
    WritePixelFogParams(reg, shadow != nullptr ? shadow->fogMode : 0);
}

void SetAmbientLightCube(NativeObject*, const Float4* cube) { std::memcpy(g_api.ambientCube, cube, sizeof(g_api.ambientCube)); }

// GetAmbientLightCubeLuminance (0x180017000): the mean luminance (0.3, 0.59, 0.11) of the six cube faces.
float GetAmbientLightCubeLuminance(NativeObject*)
{
    float sum = 0;
    for (const Float4& face : g_api.ambientCube)
        sum += face.v[0] * 0.3f + face.v[1] * 0.59f + face.v[2] * 0.11f;
    return sum / 6.0f;
}

void SetVertexShaderStateAmbientLightCube(NativeObject*) { SetVertexFloats(kVertexAmbientCube, &g_api.ambientCube[0].v[0], 6); }

void SetPixelShaderStateAmbientLightCube(NativeObject*, int reg, bool forceBlack)
{
    static const Float4 kBlack[6] = {};
    SetPixelFloats(reg, forceBlack ? &kBlack[0].v[0] : &g_api.ambientCube[0].v[0], 6);
}

// CommitPixelShaderLighting (0x180012760): colour and position of the first three sorted lights, the fourth's packed
// into the six w components; directional lights sit 10000 units from the eye against their direction.
void CommitPixelShaderLighting(NativeObject*, int reg)
{
    const std::vector<int> order = SortedLights();
    float registers[6][4] = {};
    auto position = [](const DeviceLight& light, float* out) {
        for (int c = 0; c < 3; ++c)
            out[c] = light.type == kD3DLightDirectional ? g_api.eye[c] - light.direction[c] * 10000.0f : light.position[c];
    };
    for (size_t i = 0; i < order.size() && i < 3; ++i) {
        const DeviceLight& light = g_lighting.lights[static_cast<size_t>(order[i])];
        std::copy(std::begin(light.color), std::end(light.color), registers[i * 2]);
        position(light, registers[i * 2 + 1]);
    }
    if (order.size() > 3) {
        const DeviceLight& light = g_lighting.lights[static_cast<size_t>(order[3])];
        float fourth[3];
        position(light, fourth);
        registers[0][3] = light.color[0];
        registers[1][3] = light.color[1];
        registers[2][3] = light.color[2];
        registers[3][3] = fourth[0];
        registers[4][3] = fourth[1];
        registers[5][3] = fourth[2];
    }
    SetPixelFloats(reg, &registers[0][0], 6);
}

// SetLight (0x18001ea70): theta is kept just below phi, which is at most pi; directional lights have no range.
void SetLight(NativeObject*, int index, const LightDesc& desc)
{
    if (index < 0 || index >= kMaxLights)
        return;
    g_lighting.descs[static_cast<size_t>(index)] = desc;
    static constexpr int kTypes[] = { 0, kD3DLightPoint, kD3DLightDirectional, kD3DLightSpot };
    if (desc.type < 1 || desc.type > 3) {
        g_lighting.enabled[static_cast<size_t>(index)] = false;
        return;
    }
    DeviceLight& light = g_lighting.lights[static_cast<size_t>(index)];
    light.type = kTypes[desc.type];
    std::copy(std::begin(desc.color), std::end(desc.color), light.color);
    std::copy(std::begin(desc.position), std::end(desc.position), light.position);
    std::copy(std::begin(desc.direction), std::end(desc.direction), light.direction);
    light.falloff = desc.falloff;
    std::copy(std::begin(desc.attenuation), std::end(desc.attenuation), light.attenuation);
    light.phi = std::min(desc.phi, 3.1415927f);
    light.theta = desc.theta - light.phi > -0.001f ? light.phi - 0.001f : desc.theta;
    g_lighting.enabled[static_cast<size_t>(index)] = true;
}

const LightDesc& GetLight(NativeObject*, int index) { return g_lighting.descs[static_cast<size_t>(std::clamp(index, 0, kMaxLights - 1))]; }

void DisableAllLocalLights(NativeObject*)
{
    for (int i = 0; i < kMaxLights; ++i) {
        g_lighting.descs[static_cast<size_t>(i)].type = 0;
        g_lighting.enabled[static_cast<size_t>(i)] = false;
    }
}

// Fixed-function ambient light and the lighting origin feed nothing the translated shaders read.
void SetAmbientLight(NativeObject*, float, float, float) {}
void SetLightingOrigin(NativeObject*, const float*) {}

// LightState_t (public/materialsystem/imaterialsystem.h) as GetDX9LightState (0x180017bb0) fills it.
struct LightState {
    int lights;
    bool ambient;
    bool staticVertex;
    bool staticTexel;
};

void GetDX9LightState(NativeObject*, LightState* state)
{
    state->lights = static_cast<int>(SortedLights().size());
    state->ambient = std::ranges::any_of(g_api.ambientCube, [](const Float4& c) { return c.v[0] != 0 || c.v[1] != 0 || c.v[2] != 0; });
    state->staticVertex = g_api.renderMesh != nullptr && g_api.renderMesh->streams[1].SizeInBytes != 0;
    state->staticTexel = false;
}

// LoadBoneMatrix (0x180018ec0): bone 0 also becomes the model matrix.
void LoadBoneMatrix(NativeObject* self, int bone, const float* matrix)
{
    if (bone < 0 || bone >= kMaxBones)
        return;
    std::memcpy(g_lighting.bones[static_cast<size_t>(bone)].data(), matrix, 12 * sizeof(float));
    g_lighting.maxBone = std::max(g_lighting.maxBone, bone);
    if (bone == 0) {
        Matrix model = { { { matrix[0], matrix[1], matrix[2], matrix[3] }, { matrix[4], matrix[5], matrix[6], matrix[7] },
            { matrix[8], matrix[9], matrix[10], matrix[11] }, { 0, 0, 0, 1 } } };
        model = Transpose(model);
        MatrixMode(self, kMatrixModel);
        LoadMatrix(self, &model.m[0][0]);
    }
}

// SetSkinningMatrices (0x18001f830): with bones, c58 onwards take bone 0 (the model matrix) to the highest bone loaded.
void SetSkinningMatrices(NativeObject*)
{
    void* material = g_api.material;
    if (g_api.boneWeights == 0 || material == nullptr
        || !(MaterialSlot<std::uint64_t (*)(void*)>(material, kMaterialGetVertexFormat)(material) & kVertexFormatVertexShader))
        return;
    const Matrix model = Transpose(Top(kMatrixModel));
    std::memcpy(g_lighting.bones[0].data(), &model.m[0][0], 12 * sizeof(float));
    g_api.dirty[kMatrixModel] = false;
    const int count = std::max(g_lighting.maxBone + 1, 1);
    g_lighting.maxBone = 0;
    SetVertexFloats(kVertexModel, g_lighting.bones[0].data(), count * 3);
}

int GetPixelFogCombo(NativeObject*) { return g_api.fogMode != 0 ? g_api.fogMode - 1 : 0; }

bool InFlashlightMode(NativeObject*) { return UtilSlot<bool (*)(void*)>(kShaderUtilInFlashlightMode)(GetBackend().shaderUtil); }
bool InEditorMode(NativeObject*) { return UtilSlot<bool (*)(void*)>(kShaderUtilInEditorMode)(GetBackend().shaderUtil); }

// GetLightMapScaleFactor (0x180017ed0): 16 for integer HDR, 1 for float HDR, 2^2.2 without HDR.
float GetLightMapScaleFactor(NativeObject*)
{
    const int hdr = HdrType();
    return hdr == kHdrInteger ? 16.0f : hdr == kHdrFloat ? 1.0f : std::pow(2.0f, 2.2f);
}

// SetToneMappingScaleLinear (0x180020280): pixel c30 is (scale, lightmap scale, 16 for integer HDR, scale^(1/2.2)),
// with the scale and z 1 without HDR. GetToneMappingScaleLinear (0x180018350) returns that vector.
void SetToneMappingScaleLinear(NativeObject* self, const float* scale)
{
    const int hdr = HdrType();
    g_api.lightScale[0] = hdr == kHdrNone ? 1.0f : scale[0];
    g_api.lightScale[1] = GetLightMapScaleFactor(self);
    g_api.lightScale[2] = hdr == kHdrInteger ? 16.0f : 1.0f;
    g_api.lightScale[3] = std::pow(g_api.lightScale[0], 1.0f / 2.2f);
    SetPixelFloats(kPixelLightScale, g_api.lightScale, 1);
}

const float* GetToneMappingScaleLinear(NativeObject*) { return g_api.lightScale; }

void SetIntRenderingParameter(NativeObject*, int parameter, int value)
{
    if (parameter >= 0 && parameter < kRenderParameters)
        g_api.intParameters[static_cast<size_t>(parameter)] = value;
}

int GetIntRenderingParameter(NativeObject*, int parameter)
{
    // The capture selects the stock shaders' LIGHTING_PREVIEW combos through INT_RENDERPARM_ENABLE_FIXED_LIGHTING.
    constexpr int kEnableFixedLighting = 0;
    if (parameter == kEnableFixedLighting) {
        if (const int fixedLighting = capture::FixedLighting(); fixedLighting >= 0)
            return fixedLighting;
    }
    return parameter >= 0 && parameter < kRenderParameters ? g_api.intParameters[static_cast<size_t>(parameter)] : 0;
}

void SetFloatRenderingParameter(NativeObject*, int parameter, float value)
{
    if (parameter >= 0 && parameter < kRenderParameters)
        g_api.floatParameters[static_cast<size_t>(parameter)] = value;
}

float GetFloatRenderingParameter(NativeObject*, int parameter)
{
    return parameter >= 0 && parameter < kRenderParameters ? g_api.floatParameters[static_cast<size_t>(parameter)] : 0.0f;
}

void SetVectorRenderingParameter(NativeObject*, int parameter, const float* value)
{
    if (parameter >= 0 && parameter < kRenderParameters)
        std::memcpy(g_api.vectorParameters[static_cast<size_t>(parameter)].data(), value, 3 * sizeof(float));
}

// Returns a Vector by value: MSVC passes the result's address after `this`.
float* GetVectorRenderingParameter(NativeObject*, float* result, int parameter)
{
    if (parameter >= 0 && parameter < kRenderParameters)
        std::memcpy(result, g_api.vectorParameters[static_cast<size_t>(parameter)].data(), 3 * sizeof(float));
    else
        std::memset(result, 0, 3 * sizeof(float));
    return result;
}

// ShouldWriteDepthToDestAlpha (0x180020e50): ps_2_b, not under water fog, and INT_RENDERPARM_WRITE_DEPTH_TO_DESTALPHA.
bool ShouldWriteDepthToDestAlpha(NativeObject*) { return g_api.fogMode != kFogLinearBelowFogZ && g_api.intParameters[10] != 0; }

void SceneFogMode(NativeObject*, int mode) { g_api.fogMode = mode; }

void SceneFogColor3ub(NativeObject*, unsigned char r, unsigned char g, unsigned char b)
{
    g_api.fogColor[0] = r;
    g_api.fogColor[1] = g;
    g_api.fogColor[2] = b;
}

void FogStart(NativeObject*, float start)
{
    g_api.fogStart = start;
    g_api.dirty[kMatrixView] = true;
}

void FogEnd(NativeObject*, float end)
{
    g_api.fogEnd = end;
    g_api.dirty[kMatrixView] = true;
}

void SetFogZ(NativeObject*, float z)
{
    g_api.fogZ = z;
    g_api.dirty[kMatrixView] = true;
}

void FogMaxDensity(NativeObject*, float density)
{
    g_api.fogMaxDensity = density;
    g_api.dirty[kMatrixView] = true;
}

void GetFogDistances(NativeObject*, float* start, float* end, float* z)
{
    *start = g_api.fogStart;
    *end = g_api.fogEnd;
    *z = g_api.fogZ;
}

void Bind(NativeObject*, void* material) { g_api.material = material; }

void BindTexture(NativeObject*, int sampler, std::intptr_t handle)
{
    if (sampler < 0 || sampler >= kSamplers)
        return;
    if (g_dynamic.textures[static_cast<size_t>(sampler)] != handle) {
        g_dynamic.pointFilter[static_cast<size_t>(sampler)] = 0;
        g_dynamic.borderAddress[static_cast<size_t>(sampler)] = false;
    }
    g_dynamic.textures[static_cast<size_t>(sampler)] = handle;
}

void BindVertexTexture(NativeObject*, int sampler, std::intptr_t handle)
{
    if (sampler >= 0 && sampler < kVertexSamplers)
        g_dynamic.vertexTextures[static_cast<size_t>(sampler)] = handle;
}

void SetStandardTextureHandle(NativeObject*, int id, std::intptr_t handle)
{
    if (id >= 0 && id < kStandardTextures)
        g_api.standardTextures[static_cast<size_t>(id)] = handle;
}

// BindStandardTexture (0x180011650) and command 512: the handle SetStandardTextureHandle gave, else materialsystem's.
void BindStandardTexture(NativeObject* self, int sampler, int id)
{
    if (id >= 0 && id < kStandardTextures && g_api.standardTextures[static_cast<size_t>(id)] != 0)
        BindTexture(self, sampler, g_api.standardTextures[static_cast<size_t>(id)]);
    else
        UtilSlot<void (*)(void*, int, int)>(kShaderUtilBindStandardTexture)(GetBackend().shaderUtil, sampler, id);
}

void SetSamplerBorderAddress(NativeObject*, int sampler)
{
    if (sampler >= 0 && sampler < kSamplers)
        g_dynamic.borderAddress[static_cast<size_t>(sampler)] = true;
}

// SetAnisotropicLevel (0x18001d820): levels outside 2 to the maximum become a quarter of the maximum, from 2 to 8.
void SetAnisotropicLevel(NativeObject*, int level)
{
    UtilSlot<void (*)(void*, int)>(kShaderUtilNoteAnisotropicLevel)(GetBackend().shaderUtil, level);
    if (level < 2 || level > kMaxAnisotropy)
        level = std::clamp(kMaxAnisotropy / 4, 2, 8);
    g_dynamic.anisotropy = level;
}

void SetRenderTargetEx(NativeObject*, int id, std::intptr_t color, std::intptr_t depth)
{
    if (id < 0 || id >= kRenderTargets)
        return;
    g_dynamic.colorTargets[static_cast<size_t>(id)] = color;
    if (id == 0)
        g_dynamic.depthTarget = depth;
}

void SetRenderTarget(NativeObject* self, std::intptr_t color, std::intptr_t depth) { SetRenderTargetEx(self, 0, color, depth); }
void EnableLinearColorSpaceFrameBuffer(NativeObject*, bool enable) { g_api.linearFramebuffer = enable; }
void SetFullScreenTextureHandle(NativeObject*, std::intptr_t handle) { g_api.fullScreenTexture = handle; }

// Color3f..Color4ubv (0x180011ec0..0x180012300) set D3DRS_TEXTUREFACTOR as A8R8G8B8, alpha 255 for the 3-component
// forms.
unsigned PackColor(float r, float g, float b, float a)
{
    auto channel = [](float v) { return static_cast<unsigned>(static_cast<int>(v * 255.0f)) & 0xff; };
    return channel(a) << 24 | channel(r) << 16 | channel(g) << 8 | channel(b);
}

void Color3f(NativeObject*, float r, float g, float b) { g_api.textureFactor = PackColor(r, g, b, 1.0f); }
void Color3fv(NativeObject*, const float* c) { g_api.textureFactor = PackColor(c[0], c[1], c[2], 1.0f); }
void Color4f(NativeObject*, float r, float g, float b, float a) { g_api.textureFactor = PackColor(r, g, b, a); }
void Color4fv(NativeObject*, const float* c) { g_api.textureFactor = PackColor(c[0], c[1], c[2], c[3]); }
void Color3ub(NativeObject*, unsigned char r, unsigned char g, unsigned char b) { g_api.textureFactor = 0xff000000u | r << 16 | g << 8 | b; }
void Color3ubv(NativeObject*, const unsigned char* c) { g_api.textureFactor = 0xff000000u | c[0] << 16 | c[1] << 8 | c[2]; }
void Color4ub(NativeObject*, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    g_api.textureFactor = static_cast<unsigned>(a) << 24 | r << 16 | g << 8 | b;
}
void Color4ubv(NativeObject*, const unsigned char* c) { g_api.textureFactor = static_cast<unsigned>(c[3]) << 24 | c[0] << 16 | c[1] << 8 | c[2]; }

void ClearColor3ub(NativeObject*, unsigned char r, unsigned char g, unsigned char b) { g_api.clearColor = 0xff000000u | (r << 16) | (g << 8) | b; }

void ClearColor4ub(NativeObject*, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    g_api.clearColor = (static_cast<unsigned>(a) << 24) | (r << 16) | (g << 8) | b;
}

// The clear colour as shaderapidx9.dll applies it (0x180016f00): with a linear colour space frame buffer, RGB goes
// through its pow(x, 2.2) table (filled at 0x18003ad70), read as 1 from 0.95 up (0x18003b180).
unsigned ClearColor()
{
    const unsigned c = g_api.clearColor;
    if (!g_api.linearFramebuffer)
        return c;
    auto linear = [](unsigned channel) {
        const float x = static_cast<float>(channel) * 0.003921569f;
        const float y = x >= 0.95f ? 1.0f : static_cast<float>(std::pow(std::round(x * 255.0f) * 0.003921569, 2.2));
        return static_cast<unsigned>(static_cast<long long>(y * 255.0f)) & 0xff;
    };
    return (c & 0xff000000u) | linear((c >> 16) & 0xff) << 16 | linear((c >> 8) & 0xff) << 8 | linear(c & 0xff);
}

// ClearBuffers (0x1800118d0): Direct3D 9 clears the viewport of every bound target; colour is written without sRGB
// conversion, depth to 1 (0 with reverse depth), stencil to 0.
void ClearBuffers(NativeObject*, bool color, bool depth, bool stencil, int, int)
{
    const std::uint8_t* config = Config();
    if (config != nullptr && config[kConfigSuppressRendering] != 0)
        return;
    Targets targets = PrepareTargets(false);
    ID3D12GraphicsCommandList* commands = GetBackend().Commands();
    const D3D12_VIEWPORT& viewport = g_dynamic.viewport;
    D3D12_RECT rect = { static_cast<LONG>(viewport.TopLeftX), static_cast<LONG>(viewport.TopLeftY), static_cast<LONG>(viewport.TopLeftX + viewport.Width),
        static_cast<LONG>(viewport.TopLeftY + viewport.Height) };
    rect.right = std::min(rect.right, static_cast<LONG>(targets.width));
    rect.bottom = std::min(rect.bottom, static_cast<LONG>(targets.height));
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return;
    if (color) {
        const unsigned c = ClearColor();
        const float value[4] = { ((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f, (c >> 24) / 255.0f };
        for (UINT i = 0; i < targets.count; ++i) {
            if (targets.colors[i].ptr != 0)
                commands->ClearRenderTargetView(targets.colors[i], value, 1, &rect);
        }
    }
    D3D12_CLEAR_FLAGS flags = {};
    if (depth)
        flags |= D3D12_CLEAR_FLAG_DEPTH;
    if (stencil && targets.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT)
        flags |= D3D12_CLEAR_FLAG_STENCIL;
    if (flags != 0 && targets.depth.ptr != 0)
        commands->ClearDepthStencilView(targets.depth, flags, ReverseDepth() ? 0.0f : 1.0f, 0, 1, &rect);
}

void CullMode(NativeObject*, int mode) { g_dynamic.cullMode = mode; }
void ForceDepthFuncEquals(NativeObject*, bool enable) { g_dynamic.forceDepthFuncEquals = enable; }

void OverrideDepthEnable(NativeObject*, bool enable, bool depthEnable)
{
    g_dynamic.overrideDepthEnable = enable;
    g_dynamic.depthEnable = depthEnable;
}

void OverrideAlphaWriteEnable(NativeObject*, bool enable, bool alphaWrite)
{
    g_dynamic.overrideAlphaWrite = enable;
    g_dynamic.alphaWriteEnable = alphaWrite;
}

void OverrideColorWriteEnable(NativeObject*, bool enable, bool colorWrite)
{
    g_dynamic.overrideColorWrite = enable;
    g_dynamic.colorWriteEnable = colorWrite;
}

void SetStencilEnable(NativeObject*, bool enable) { g_dynamic.stencilEnable = enable; }
void SetStencilFailOperation(NativeObject*, int op) { g_dynamic.stencilFail = op; }
void SetStencilZFailOperation(NativeObject*, int op) { g_dynamic.stencilDepthFail = op; }
void SetStencilPassOperation(NativeObject*, int op) { g_dynamic.stencilPass = op; }
void SetStencilCompareFunction(NativeObject*, int func) { g_dynamic.stencilFunc = func; }
void SetStencilReferenceValue(NativeObject*, int ref) { g_dynamic.stencilRef = ref; }
void SetStencilTestMask(NativeObject*, unsigned mask) { g_dynamic.stencilTestMask = mask; }
void SetStencilWriteMask(NativeObject*, unsigned mask) { g_dynamic.stencilWriteMask = mask; }

void OverrideTextureFilter(NativeObject*, bool minification, int filter)
{
    (minification ? g_dynamic.minFilterOverride : g_dynamic.magFilterOverride) = filter;
}

void OverrideBlend(NativeObject*, bool override, bool enable, int source, int destination, int op)
{
    g_dynamic.blendOverride = { override, enable, source, destination, op };
}

void OverrideBlendSeparateAlpha(NativeObject*, bool override, bool enable, int source, int destination, int op)
{
    g_dynamic.alphaBlendOverride = { override, enable, source, destination, op };
}

void SetClipPlane(NativeObject*, int index, const float* plane)
{
    if (index < 0 || index >= kClipPlanes)
        return;
    g_clip.planes[index][0] = plane[0];
    g_clip.planes[index][1] = plane[1];
    g_clip.planes[index][2] = plane[2];
    g_clip.planes[index][3] = -plane[3];
}

void EnableUserClipTransformOverride(NativeObject*, bool enable) { g_clip.transformOverride = enable; }

void UserClipTransform(NativeObject*, const float* worldToView)
{
    Matrix matrix;
    std::memcpy(&matrix, worldToView, sizeof(matrix));
    g_clip.userTransform = Transpose(matrix);
}

void EnableClipPlane(NativeObject*, int index, bool enable)
{
    if (index < 0 || index >= kClipPlanes)
        return;
    g_clip.enabled = enable ? (g_clip.enabled | (1u << index)) : (g_clip.enabled & ~(1u << index));
}

bool InitializeQueries()
{
    if (g_queries.heap != nullptr)
        return true;
    ID3D12Device* device = GetBackend().Device();
    D3D12_QUERY_HEAP_DESC desc = { D3D12_QUERY_HEAP_TYPE_OCCLUSION, kOcclusionQueries, 0 };
    HRESULT hr = device->CreateQueryHeap(&desc, IID_PPV_ARGS(&g_queries.heap));
    const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = kOcclusionQueries * sizeof(std::uint64_t);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_queries.results));
    if (SUCCEEDED(hr))
        hr = g_queries.results->Map(0, nullptr, reinterpret_cast<void**>(const_cast<std::uint64_t**>(&g_queries.mapped)));
    if (FAILED(hr)) {
        log::Info("Native: creating the occlusion queries failed: {:#010x}", static_cast<unsigned>(hr));
        g_queries.heap = nullptr;
        return false;
    }
    return true;
}

// Occlusion queries (0x180014550 and its neighbours): GetNumPixelsRendered returns -1 while the result is pending.
OcclusionQuery* CreateOcclusionQueryObject(NativeObject*)
{
    if (!InitializeQueries())
        return nullptr;
    UINT slot;
    if (!g_queries.free.empty()) {
        slot = g_queries.free.back();
        g_queries.free.pop_back();
    } else if (g_queries.next < kOcclusionQueries) {
        slot = g_queries.next++;
    } else {
        log::Info("Native: more than {} occlusion queries", kOcclusionQueries);
        return nullptr;
    }
    return new OcclusionQuery{ slot };
}

void DestroyOcclusionQueryObject(NativeObject*, OcclusionQuery* query)
{
    if (query == nullptr)
        return;
    g_queries.free.push_back(query->slot);
    delete query;
}

void BeginOcclusionQueryDrawing(NativeObject*, OcclusionQuery* query)
{
    if (query == nullptr)
        return;
    query->ended = false;
    GetBackend().Commands()->BeginQuery(g_queries.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, query->slot);
}

void EndOcclusionQueryDrawing(NativeObject*, OcclusionQuery* query)
{
    if (query == nullptr)
        return;
    Backend& backend = GetBackend();
    backend.Commands()->EndQuery(g_queries.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, query->slot);
    backend.Commands()->ResolveQueryData(g_queries.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, query->slot, 1, g_queries.results.Get(),
        query->slot * sizeof(std::uint64_t));
    query->fence = backend.PendingFence();
    query->ended = true;
}

int OcclusionQueryGetNumPixelsRendered(NativeObject*, OcclusionQuery* query, bool flush)
{
    if (query == nullptr || !query->ended)
        return -1;
    Backend& backend = GetBackend();
    if (backend.CompletedFence() < query->fence) {
        if (!flush)
            return -1;
        backend.Flush();
    }
    return static_cast<int>(std::min<std::uint64_t>(g_queries.mapped[query->slot], INT_MAX));
}

// CompareSnapshots (0x180013970): by vertex shader, pixel shader and their static combos, then by id.
int CompareSnapshots(NativeObject*, short first, short second)
{
    const ShadowState* a = Snapshot(first);
    const ShadowState* b = Snapshot(second);
    if (a != nullptr && b != nullptr) {
        if (a->vertexShader != b->vertexShader)
            return a->vertexShader < b->vertexShader ? -1 : 1;
        if (a->pixelShader != b->pixelShader)
            return a->pixelShader < b->pixelShader ? -1 : 1;
        if (a->vertexStaticIndex != b->vertexStaticIndex)
            return a->vertexStaticIndex - b->vertexStaticIndex;
        if (a->pixelStaticIndex != b->pixelStaticIndex)
            return a->pixelStaticIndex - b->pixelStaticIndex;
    }
    return first - second;
}

void IgnoreFlag(NativeObject*, bool) {}

void SetScissorRect(NativeObject*, int left, int top, int right, int bottom, bool enable)
{
    g_dynamic.scissorEnabled = enable;
    g_dynamic.scissor = { left, top, right, bottom };
}

// SetStandardVertexShaderConstants (0x18001f9a0) at DX level 95.
void SetStandardVertexShaderConstants(NativeObject*, float overbright)
{
    const float c0[4] = { 0.0f, 1.0f, 2.0f, 0.5f };
    const float c1[4] = { 1.0f / 2.2f, overbright, 1.0f / 3.0f, 1.0f / overbright };
    const float c3[4] = { 27.0f, 32.0f, 765.01f, static_cast<float>(kVertexModel) };
    SetVertexFloats(0, c0, 1);
    SetVertexFloats(kVertexConstants1, c1, 1);
    SetVertexFloats(kVertexFlexScale, c3, 1);
}

void SetDepthFeatheringPixelShaderConstant(NativeObject*, int reg, float scale)
{
    const float value[4] = { kDestAlphaDepthRange / scale, 0, 0, 0 };
    SetPixelFloats(reg, value, 1);
}

// ResetRenderState (0x18001bf40), which shaderapidx9.dll also runs after creating its device: a partial reset clears the
// constant registers and sets the standard vertex shader constants with an overbright of 2; every derived matrix
// constant is stale again and the viewport covers the window.
// ResetRenderState (0x18001bf40). Both resets zero the constants; a full one, which a new device gets
// (ReacquireResourcesInternal, 0x1800276b0), also sets the anisotropy and identity view and projection matrices.
void ResetRenderState(NativeObject* self, bool fullReset)
{
    g_dynamic.vertexFloats = {};
    g_dynamic.pixelFloats = {};
    ++g_dynamic.vertexFloatsVersion;
    ++g_dynamic.pixelFloatsVersion;
    g_dynamic.vertexInts = {};
    g_dynamic.pixelInts = {};
    g_dynamic.vertexBools = {};
    g_dynamic.pixelBools = {};
    SetStandardVertexShaderConstants(self, kOverbright);
    if (fullReset) {
        SetAnisotropicLevel(self, 1);
        for (int mode : { kMatrixView, kMatrixProjection }) {
            MatrixMode(self, mode);
            LoadIdentity(self);
        }
    }
    g_api.dirty.fill(true);
    RECT client = {};
    GetClientRect(GetBackend().Window(), &client);
    const int viewport[7] = { 1, 0, 0, client.right - client.left, client.bottom - client.top, 0, 0 };
    float depth[2] = { 0.0f, 1.0f };
    int raw[7];
    std::memcpy(raw, viewport, sizeof(raw));
    std::memcpy(raw + 5, depth, sizeof(depth));
    SetViewports(self, 1, raw);
}

void ExecuteCommandBuffer(NativeObject* self, const std::uint8_t* buffer);

void ExecuteCommands(NativeObject* self, const std::uint8_t* at)
{
    auto read = [&](size_t offset) {
        int value;
        std::memcpy(&value, at + offset, sizeof(value));
        return value;
    };
    for (;;) {
        const int opcode = read(0);
        switch (opcode) {
        case kCommandEnd:
            return;
        case kCommandJump: {
            const std::uint8_t* target;
            std::memcpy(&target, at + 4, sizeof(target));
            at = target;
            continue;
        }
        case kCommandJsr: {
            const std::uint8_t* target;
            std::memcpy(&target, at + 4, sizeof(target));
            ExecuteCommands(self, target);
            at += 12;
            continue;
        }
        case kCommandSetPixelShaderFloatConst:
        case kCommandSetVertexShaderFloatConst: {
            const int first = read(4);
            const int count = read(8);
            // The floats are unaligned in the buffer.
            std::array<float, kVertexFloatConstants * 4> values;
            const int copied = std::clamp(count, 0, kVertexFloatConstants);
            std::memcpy(values.data(), at + 12, static_cast<size_t>(copied) * 4 * sizeof(float));
            if (opcode == kCommandSetPixelShaderFloatConst)
                SetPixelFloats(first, values.data(), copied);
            else
                SetVertexFloats(first, values.data(), copied);
            at += 12 + static_cast<size_t>(std::max(count, 0)) * 4 * sizeof(float);
            continue;
        }
        case kCommandSetPixelShaderFogParams:
            SetPixelShaderFogParams(self, read(4));
            at += 8;
            continue;
        case kCommandStoreEyePosInPixelShaderConst:
            SetPixelFloats(read(4), g_api.eye, 1);
            at += 8;
            continue;
        case kCommandCommitPixelShaderLighting:
            CommitPixelShaderLighting(self, read(4));
            at += 8;
            continue;
        case kCommandSetPixelShaderAmbientLightCube:
            SetPixelShaderStateAmbientLightCube(self, read(4), false);
            at += 8;
            continue;
        case kCommandSetVertexShaderAmbientLightCube:
            SetVertexShaderStateAmbientLightCube(self);
            at += 4;
            continue;
        case kCommandSetDepthFeatheringConst: {
            float scale;
            std::memcpy(&scale, at + 8, sizeof(scale));
            SetDepthFeatheringPixelShaderConstant(self, read(4), scale);
            at += 12;
            continue;
        }
        case kCommandBindStandardTexture:
            BindStandardTexture(self, read(4), read(8));
            at += 12;
            continue;
        case kCommandBindShaderApiTextureHandle: {
            std::intptr_t handle;
            std::memcpy(&handle, at + 8, sizeof(handle));
            BindTexture(self, read(4), handle);
            at += 16;
            continue;
        }
        case kCommandSetPixelShaderIndex:
            g_dynamic.pixelShaderIndex = read(4);
            at += 8;
            continue;
        case kCommandSetVertexShaderIndex:
            g_dynamic.vertexShaderIndex = read(4);
            at += 8;
            continue;
        case kCommandSetSamplerPointFilter: {
            const int sampler = read(4);
            const int mode = read(8);
            if (sampler >= 0 && sampler < kSamplers && (mode == 1 || mode == 2))
                g_dynamic.pointFilter[static_cast<size_t>(sampler)] = mode;
            at += 12;
            continue;
        }
        default:
            log::Info("Native: command buffer opcode {} is unknown; the rest of the buffer is skipped", opcode);
            return;
        }
    }
}

void ExecuteCommandBuffer(NativeObject* self, const std::uint8_t* buffer) { ExecuteCommands(self, buffer); }

void BeginPass(NativeObject*, short snapshot) { g_api.snapshot = snapshot; }

// The fog colour a pass's shader fog mode selects (shaderapidx9.dll 0x180010ed0) as pixel c29, linear and scaled by
// the tone mapping scale like 0x180022230, with w the reciprocal of the destination alpha depth range; the pixel fog
// params are rewritten for that mode too. Pixel fog is used (r_pixelfog), so Direct3D 9's fixed-function fog stays
// off at DX level 95 (0x1800167e0).
void CommitLinearFogColor(const ShadowState& shadow)
{
    float color[4] = { 0, 0, 0, 1.0f / kDestAlphaDepthRange };
    if (g_api.fogMode != 0) {
        unsigned char rgb[3] = {};
        bool fog = true;
        switch (shadow.fogMode) {
        case kShaderFogOverbright:
        case kShaderFogGrey: rgb[0] = rgb[1] = rgb[2] = 0x80; break;
        case kShaderFogBlack: fog = false; break;
        case kShaderFogColor: std::memcpy(rgb, g_api.fogColor, 3); break;
        case kShaderFogWhite: rgb[0] = rgb[1] = rgb[2] = 0xff; break;
        default: fog = false; break;
        }
        for (int i = 0; i < 3; ++i) {
            const float gamma = rgb[i] / 255.0f;
            color[i] = gamma <= 0.04045f ? gamma * 0.07739938f : std::pow(gamma * 0.94786733f + 0.052132703f, 2.4f);
            if (fog && !shadow.disableFogGammaCorrection)
                color[i] *= g_api.lightScale[0];
        }
    }
    SetPixelFloats(kPixelLinearFogColor, color, 1);
    if (g_api.pixelFogRegister != -1)
        WritePixelFogParams(g_api.pixelFogRegister, shadow.fogMode);
}

// RenderPass (0x18001ab60): draws the render mesh with the pass's snapshot, then forgets the snapshot.
void RenderPass(NativeObject*, int, int)
{
    const ShadowState* shadow = Snapshot(g_api.snapshot);
    if (shadow != nullptr && g_api.renderMesh != nullptr) {
        CommitLinearFogColor(*shadow);
        DrawPass(*shadow, *g_api.renderMesh);
    }
    g_api.snapshot = -1;
}

void InvalidateDelayedShaderConstants(NativeObject*) {}
void BeginFrame(NativeObject*) {}
void EndFrame(NativeObject*) {}
void Nothing(NativeObject*) {}

bool SetMode(ShaderApiObject* self, void* window, int, const ShaderDeviceInfo& info)
{
    if (!GetBackend().SetMode(static_cast<HWND>(window), info))
        return false;
    ResetRenderState(self, true);
    realtime::OnDeviceReady();
    return true;
}

// shaderapidx9.dll 0x180011870 only records the mode; Present applies it.
void ChangeVideoMode(ShaderApiObject*, const ShaderDeviceInfo& info) { RequestVideoMode(info); }

void EnableShaderShaderMutex(ShaderApiObject* self, bool enable) { self->mutexEnabled = enable; }

void ShaderLock(ShaderApiObject* self)
{
    if (self->mutexEnabled)
        self->mutex.lock();
}

void ShaderUnlock(ShaderApiObject* self)
{
    if (self->mutexEnabled)
        self->mutex.unlock();
}

void FlushBufferedPrimitives(ShaderApiObject*)
{
    if (GetBackend().shaderUtil != nullptr)
        UtilSlot<bool (*)(void*)>(kShaderUtilOnFlushBufferedPrimitives)(GetBackend().shaderUtil);
}

// ClearBuffersObeyStencilEx (0x180011b30): materialsystem draws a quad over the viewport with the current stencil state,
// user clip planes off. ClearBuffersObeyStencil (0x180011b10) clears alpha with colour.
void ClearBuffersObeyStencilEx(ShaderApiObject* self, bool color, bool alpha, bool depth)
{
    if (!color && !alpha && !depth)
        return;
    FlushBufferedPrimitives(self);
    const unsigned clipPlanes = std::exchange(g_clip.enabled, 0u);
    const unsigned c = ClearColor();
    using DrawFn = void (*)(void*, unsigned char, unsigned char, unsigned char, unsigned char, bool, bool, bool);
    UtilSlot<DrawFn>(kShaderUtilDrawClearBufferQuad)(GetBackend().shaderUtil, static_cast<unsigned char>(c >> 16), static_cast<unsigned char>(c >> 8),
        static_cast<unsigned char>(c), static_cast<unsigned char>(c >> 24), color, alpha, depth);
    FlushBufferedPrimitives(self);
    g_clip.enabled = clipPlanes;
}

void ClearBuffersObeyStencil(ShaderApiObject* self, bool color, bool depth) { ClearBuffersObeyStencilEx(self, color, color, depth); }

// 0x18001e200; shadow depth textures are supported, so shadows are kept. SetFlashlightState (0x18001e1f0) passes no
// depth texture.
void SetFlashlightStateEx(ShaderApiObject* self, const std::uint8_t* state, const Matrix& worldToTexture, void* depthTexture)
{
    FlushBufferedPrimitives(self);
    std::memcpy(g_flashlight.state.data(), state, kFlashlightStateSize);
    g_flashlight.worldToTexture = worldToTexture;
    g_flashlight.depthTexture = depthTexture;
}

void SetFlashlightState(ShaderApiObject* self, const std::uint8_t* state, const Matrix& worldToTexture)
{
    SetFlashlightStateEx(self, state, worldToTexture, nullptr);
}

// 0x180017da0 and 0x180017dd0.
const std::uint8_t* GetFlashlightState(ShaderApiObject*, Matrix& worldToTexture)
{
    worldToTexture = g_flashlight.worldToTexture;
    return g_flashlight.state.data();
}

const std::uint8_t* GetFlashlightStateEx(ShaderApiObject*, Matrix& worldToTexture, void** depthTexture)
{
    worldToTexture = g_flashlight.worldToTexture;
    *depthTexture = g_flashlight.depthTexture;
    return g_flashlight.state.data();
}

// shaderapidx9.dll 0x180011840: whether the device exists and is not deactivated.
bool CanDownloadTextures(ShaderApiObject*) { return GetBackend().HasDevice(); }
// shaderapidx9.dll 0x180015270: only a multisampled depth buffer needs separate ones, and MSAA is never used.
bool DoRenderTargetsNeedSeparateDepthBuffer(ShaderApiObject*) { return false; }

void GetBackBufferDimensions(ShaderApiObject*, int& width, int& height)
{
    width = static_cast<int>(GetBackend().Width());
    height = static_cast<int>(GetBackend().Height());
}

int GetMaxLights(NativeObject*) { return 4; }
bool IsHWMorphingEnabled(NativeObject*) { return false; }
bool SupportsShadowDepthTextures(NativeObject*) { return true; }
int GetShadowDepthTextureFormat(NativeObject*) { return kFormatNvDst16; }
bool SupportsFetch4(NativeObject*) { return false; }
int GetNullTextureFormat(NativeObject*) { return kFormatNvNull; }
bool SupportsNormalMapCompression(NativeObject*) { return false; }
// Real-time lighting reads single-sampled targets, so MSAA is reported unsupported.
bool SupportsMSAAMode(NativeObject*, int mode) { return mode == 0 || mode == 1; }
bool SupportsCSAAMode(NativeObject*, int, int) { return false; }

// LinearToGamma_HardwareSpecific and GammaToLinear_HardwareSpecific (0x18003b260, 0x18003b200): the sRGB curves.
float LinearToGamma(NativeObject*, float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

float GammaToLinear(NativeObject*, float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f ? value * 0.07739938f : std::pow(value * 0.94786733f + 0.052132703f, 2.4f);
}

void SetLinearToGammaConversionTextures(NativeObject*, std::intptr_t, std::intptr_t) {}
void SetShadowDepthBiasFactors(NativeObject*, float slopeScale, float bias)
{
    g_dynamic.shadowSlopeScaleDepthBias = slopeScale;
    g_dynamic.shadowDepthBias = bias;
}
void IgnorePix(NativeObject*, unsigned long, const char*) {}

}

DynamicState& Dynamic() { return g_dynamic; }

int UserClipPlanes(float planes[6][4])
{
    if (g_clip.enabled == 0)
        return 0;
    Matrix transform;
    // 0x180012ec0 with vertex shaders: world-space planes go to clip space through the view (or the override) and
    // the projection.
    const Matrix& view = g_clip.transformOverride ? g_clip.userTransform : Top(kMatrixView);
    if (!Invert(Multiply(view, Top(kMatrixProjection)), transform))
        return 0;
    transform = Transpose(transform);
    int count = 0;
    for (int i = 0; i < kClipPlanes; ++i) {
        if (!(g_clip.enabled & (1u << i)))
            continue;
        const float* p = g_clip.planes[i];
        for (int c = 0; c < 4; ++c)
            planes[count][c] = p[0] * transform.m[0][c] + p[1] * transform.m[1][c] + p[2] * transform.m[2][c] + p[3] * transform.m[3][c];
        ++count;
    }
    return count;
}
void ResetDynamicState() { ResetRenderState(ShaderApi(), true); }
void* BoundMaterial() { return g_api.material; }

const char* BoundMaterialName()
{
    constexpr unsigned kMaterialGetName = 0;
    return g_api.material != nullptr ? MaterialSlot<const char* (*)(void*)>(g_api.material, kMaterialGetName)(g_api.material) : "none";
}

const char* BoundShaderName()
{
    // IMaterial::GetShaderName (the IMaterial:GetShader Lua binding calls it; engine_interfaces.cpp).
    constexpr unsigned kMaterialGetShaderName = 49;
    const char* name = g_api.material != nullptr ? MaterialSlot<const char* (*)(void*)>(g_api.material, kMaterialGetShaderName)(g_api.material) : nullptr;
    return name != nullptr ? name : "none";
}

// 0x1800112c0: decals use the reciprocals of MaterialSystem_Config_t's decal factors (slope 0x5c, bias 0x60; SDK
// defaults -0.5 and -262144), other passes those of its normal ones (0x54, 0x58), and shadow depth the factors set
// for it. The ATI and no-slope-bias fallbacks there do not apply to this device.
DepthBias PassDepthBias(int polygonOffsetMode)
{
    constexpr int kPolygonOffsetDecal = 1;
    constexpr int kPolygonOffsetShadowBias = 2;
    if (polygonOffsetMode == kPolygonOffsetShadowBias)
        return { g_dynamic.shadowSlopeScaleDepthBias, g_dynamic.shadowDepthBias };
    const std::uint8_t* config = Config();
    if (config == nullptr)
        return {};
    const size_t slope = polygonOffsetMode == kPolygonOffsetDecal ? kConfigDecalSlopeScaleDepthBias : kConfigNormalSlopeScaleDepthBias;
    const size_t bias = polygonOffsetMode == kPolygonOffsetDecal ? kConfigDecalDepthBias : kConfigNormalDepthBias;
    auto reciprocal = [&](size_t offset) {
        float value;
        std::memcpy(&value, config + offset, sizeof(value));
        return value != 0 ? 1.0f / value : 0.0f;
    };
    return { reciprocal(slope), reciprocal(bias) };
}

void DrawMesh(const MeshDraw& mesh)
{
    const std::uint8_t* config = Config();
    if (config != nullptr && config[kConfigSuppressRendering] != 0)
        return;
    void* material = g_api.material;
    if (material == nullptr)
        return;
    g_api.renderMesh = &mesh;
    CommitStateChanges();
    // 0x18000d6c0: c3 scales the flex position delta (x) and wrinkle (y, ps_2_b only), and is zero without flex.
    const float flex = mesh.streams[2].SizeInBytes != 0 ? 1.0f : 0.0f;
    const float flexScale[4] = { flex, flex, 0, 0 };
    SetVertexFloats(kVertexFlexScale, flexScale, 1);
    MaterialSlot<void (*)(void*, int)>(material, kMaterialDrawMesh)(material, (mesh.vertexFormat & kVertexFormatCompressed) ? 1 : 0);
    g_api.renderMesh = nullptr;
}

NativeObject* ShaderApi()
{
    static NativeClass nativeClass = [] {
        NativeClass c("IShaderAPI", { shim::kShaderApiSlots, shim::kShaderApiSlotsCount }, kShaderApiSlots);
        c.Bind("SetMode", &SetMode);
        c.Bind("ChangeVideoMode", &ChangeVideoMode);
        c.Bind("EnableShaderShaderMutex", &EnableShaderShaderMutex);
        c.Bind("ShaderLock", &ShaderLock);
        c.Bind("ShaderUnlock", &ShaderUnlock);
        c.Bind("FlushBufferedPrimitives", &FlushBufferedPrimitives);
        c.Bind("ClearBuffersObeyStencil", &ClearBuffersObeyStencil);
        c.Bind("ClearBuffersObeyStencilEx", &ClearBuffersObeyStencilEx);
        c.Bind("SetFlashlightState", &SetFlashlightState);
        c.Bind("SetFlashlightStateEx", &SetFlashlightStateEx);
        c.Bind("GetFlashlightState", &GetFlashlightState);
        c.Bind("GetFlashlightStateEx", &GetFlashlightStateEx);
        c.Bind("CanDownloadTextures", &CanDownloadTextures);
        c.Bind("DoRenderTargetsNeedSeparateDepthBuffer", &DoRenderTargetsNeedSeparateDepthBuffer);
        c.Bind("GetBackBufferDimensions", &GetBackBufferDimensions);
        c.Bind("SetViewports", &SetViewports);
        c.Bind("GetViewports", &GetViewports);
        c.Bind("CurrentTime", &CurrentTime);
        c.Bind("GetSceneFogMode", &GetSceneFogMode);
        c.Bind("GetSceneFogColor", &GetSceneFogColor);
        c.Bind("MatrixMode", &MatrixMode);
        c.Bind("PushMatrix", &PushMatrix);
        c.Bind("PopMatrix", &PopMatrix);
        c.Bind("LoadMatrix", &LoadMatrix);
        c.Bind("MultMatrix", &MultMatrix);
        c.Bind("MultMatrixLocal", &MultMatrixLocal);
        c.Bind("GetMatrix", &GetMatrix);
        c.Bind("LoadIdentity", &LoadIdentity);
        c.Bind("LoadCameraToWorld", &LoadCameraToWorld);
        c.Bind("SetVertexShaderConstant", &SetVertexShaderConstant);
        c.Bind("SetPixelShaderConstant", &SetPixelShaderConstant);
        c.Bind("SetBooleanVertexShaderConstant", &SetBooleanVertexShaderConstant);
        c.Bind("SetIntegerVertexShaderConstant", &SetIntegerVertexShaderConstant);
        c.Bind("SetBooleanPixelShaderConstant", &SetBooleanPixelShaderConstant);
        c.Bind("SetIntegerPixelShaderConstant", &SetIntegerPixelShaderConstant);
        c.Bind("SetDefaultState", &SetDefaultState);
        c.Bind("GetWorldSpaceCameraPosition", &GetWorldSpaceCameraPosition);
        c.Bind("GetCurrentNumBones", &GetCurrentNumBones);
        c.Bind("SetNumBoneWeights", &SetNumBoneWeights);
        c.Bind("GetCurrentFogType", &GetCurrentFogType);
        c.Bind("SetVertexShaderIndex", &SetVertexShaderIndex);
        c.Bind("SetPixelShaderIndex", &SetPixelShaderIndex);
        c.Bind("GetMaxLights", &GetMaxLights);
        c.Bind("SetPixelShaderFogParams", &SetPixelShaderFogParams);
        c.Bind("SetAmbientLightCube", &SetAmbientLightCube);
        c.Bind("GetAmbientLightCubeLuminance", &GetAmbientLightCubeLuminance);
        c.Bind("SetVertexShaderStateAmbientLightCube", &SetVertexShaderStateAmbientLightCube);
        c.Bind("SetPixelShaderStateAmbientLightCube", &SetPixelShaderStateAmbientLightCube);
        c.Bind("CommitPixelShaderLighting", &CommitPixelShaderLighting);
        c.Bind("SetLight", &SetLight);
        c.Bind("GetLight", &GetLight);
        c.Bind("DisableAllLocalLights", &DisableAllLocalLights);
        c.Bind("SetAmbientLight", &SetAmbientLight);
        c.Bind("SetLightingOrigin", &SetLightingOrigin);
        c.Bind("GetDX9LightState", &GetDX9LightState);
        c.Bind("LoadBoneMatrix", &LoadBoneMatrix);
        c.Bind("SetSkinningMatrices", &SetSkinningMatrices);
        c.Bind("GetPixelFogCombo", &GetPixelFogCombo);
        c.Bind("InFlashlightMode", &InFlashlightMode);
        c.Bind("InEditorMode", &InEditorMode);
        c.Bind("BindStandardTexture", &BindStandardTexture);
        c.Bind("SetToneMappingScaleLinear", &SetToneMappingScaleLinear);
        c.Bind("GetToneMappingScaleLinear", &GetToneMappingScaleLinear);
        c.Bind("GetLightMapScaleFactor", &GetLightMapScaleFactor);
        c.Bind("SetFloatRenderingParameter", &SetFloatRenderingParameter);
        c.Bind("SetIntRenderingParameter", &SetIntRenderingParameter);
        c.Bind("SetVectorRenderingParameter", &SetVectorRenderingParameter);
        c.Bind("GetFloatRenderingParameter", &GetFloatRenderingParameter);
        c.Bind("GetIntRenderingParameter", &GetIntRenderingParameter);
        c.Bind("GetVectorRenderingParameter", &GetVectorRenderingParameter);
        c.Bind("SetStencilEnable", &SetStencilEnable);
        c.Bind("SetStencilFailOperation", &SetStencilFailOperation);
        c.Bind("SetStencilZFailOperation", &SetStencilZFailOperation);
        c.Bind("SetStencilPassOperation", &SetStencilPassOperation);
        c.Bind("SetStencilCompareFunction", &SetStencilCompareFunction);
        c.Bind("SetStencilReferenceValue", &SetStencilReferenceValue);
        c.Bind("SetStencilTestMask", &SetStencilTestMask);
        c.Bind("SetStencilWriteMask", &SetStencilWriteMask);
        c.Bind("IsHWMorphingEnabled", &IsHWMorphingEnabled);
        c.Bind("ShouldWriteDepthToDestAlpha", &ShouldWriteDepthToDestAlpha);
        c.Bind("ExecuteCommandBuffer", &ExecuteCommandBuffer);
        c.Bind("SetStandardTextureHandle", &SetStandardTextureHandle);
        c.Bind("SetDepthFeatheringPixelShaderConstant", &SetDepthFeatheringPixelShaderConstant);
        c.Bind("SetSamplerBorderAddress", &SetSamplerBorderAddress);
        c.Bind("ClearBuffers", &ClearBuffers);
        c.Bind("ClearColor3ub", &ClearColor3ub);
        c.Bind("Ortho", &Ortho);
        c.Bind("PerspectiveX", &PerspectiveX);
        c.Bind("PerspectiveOffCenterX", &PerspectiveOffCenterX);
        c.Bind("PickMatrix", &PickMatrix);
        c.Bind("Rotate", &Rotate);
        c.Bind("Translate", &Translate);
        c.Bind("Scale", &Scale);
        c.Bind("ScaleXY", &ScaleXY);
        c.Bind("Color3f", &Color3f);
        c.Bind("Color3fv", &Color3fv);
        c.Bind("Color4f", &Color4f);
        c.Bind("Color4fv", &Color4fv);
        c.Bind("Color3ub", &Color3ub);
        c.Bind("Color3ubv", &Color3ubv);
        c.Bind("Color4ub", &Color4ub);
        c.Bind("Color4ubv", &Color4ubv);
        c.Bind("ClearColor4ub", &ClearColor4ub);
        c.Bind("Bind", &Bind);
        c.Bind("BeginPass", &BeginPass);
        c.Bind("RenderPass", &RenderPass);
        c.Bind("CullMode", &CullMode);
        c.Bind("ForceDepthFuncEquals", &ForceDepthFuncEquals);
        c.Bind("OverrideDepthEnable", &OverrideDepthEnable);
        c.Bind("BindTexture", &BindTexture);
        c.Bind("BindVertexTexture", &BindVertexTexture);
        c.Bind("SetRenderTarget", &SetRenderTarget);
        c.Bind("SetRenderTargetEx", &SetRenderTargetEx);
        c.Bind("BeginFrame", &BeginFrame);
        c.Bind("EndFrame", &EndFrame);
        c.Bind("SceneFogMode", &SceneFogMode);
        c.Bind("SceneFogColor3ub", &SceneFogColor3ub);
        c.Bind("FogStart", &FogStart);
        c.Bind("FogEnd", &FogEnd);
        c.Bind("SetFogZ", &SetFogZ);
        c.Bind("FogMaxDensity", &FogMaxDensity);
        c.Bind("GetFogDistances", &GetFogDistances);
        c.Bind("SetAnisotropicLevel", &SetAnisotropicLevel);
        c.Bind("SetStandardVertexShaderConstants", &SetStandardVertexShaderConstants);
        c.Bind("EnableLinearColorSpaceFrameBuffer", &EnableLinearColorSpaceFrameBuffer);
        c.Bind("SetFullScreenTextureHandle", &SetFullScreenTextureHandle);
        c.Bind("SupportsMSAAMode", &SupportsMSAAMode);
        c.Bind("SupportsCSAAMode", &SupportsCSAAMode);
        c.Bind("SupportsShadowDepthTextures", &SupportsShadowDepthTextures);
        c.Bind("GetShadowDepthTextureFormat", &GetShadowDepthTextureFormat);
        c.Bind("SupportsFetch4", &SupportsFetch4);
        c.Bind("GetNullTextureFormat", &GetNullTextureFormat);
        c.Bind("SupportsNormalMapCompression", &SupportsNormalMapCompression);
        c.Bind("InvalidateDelayedShaderConstants", &InvalidateDelayedShaderConstants);
        c.Bind("GammaToLinear_HardwareSpecific", &GammaToLinear);
        c.Bind("LinearToGamma_HardwareSpecific", &LinearToGamma);
        c.Bind("SetLinearToGammaConversionTextures", &SetLinearToGammaConversionTextures);
        c.Bind("OverrideAlphaWriteEnable", &OverrideAlphaWriteEnable);
        c.Bind("OverrideColorWriteEnable", &OverrideColorWriteEnable);
        c.Bind("SetScissorRect", &SetScissorRect);
        c.Bind("SetClipPlane", &SetClipPlane);
        c.Bind("EnableUserClipTransformOverride", &EnableUserClipTransformOverride);
        c.Bind("UserClipTransform", &UserClipTransform);
        c.Bind("EnableClipPlane", &EnableClipPlane);
        c.Bind("CreateOcclusionQueryObject", &CreateOcclusionQueryObject);
        c.Bind("DestroyOcclusionQueryObject", &DestroyOcclusionQueryObject);
        c.Bind("BeginOcclusionQueryDrawing", &BeginOcclusionQueryDrawing);
        c.Bind("EndOcclusionQueryDrawing", &EndOcclusionQueryDrawing);
        c.Bind("OcclusionQuery_GetNumPixelsRendered", &OcclusionQueryGetNumPixelsRendered);
        c.Bind("CompareSnapshots", &CompareSnapshots);
        // Buffers and shaders are freed when their owners go; nothing is kept for these to release.
        c.Bind("DestroyVertexBuffers", &IgnoreFlag);
        c.Bind("ClearVertexAndPixelShaderRefCounts", &Nothing);
        c.Bind("PurgeUnusedVertexAndPixelShaders", &Nothing);
        c.Bind("OverrideTextureFilter", &OverrideTextureFilter);
        c.Bind("OverrideBlend", &OverrideBlend);
        c.Bind("OverrideBlendSeparateAlpha", &OverrideBlendSeparateAlpha);
        c.Bind("BeginPIXEvent", &IgnorePix);
        c.Bind("EndPIXEvent", &Nothing);
        c.Bind("SetPIXMarker", &IgnorePix);
        c.Bind("SetShadowDepthBiasFactors", &SetShadowDepthBiasFactors);
        c.Bind("ResetRenderState", &ResetRenderState);
        // Managed resources are default-heap resources here, and the backend bounds the frames in flight itself.
        c.Bind("EvictManagedResources", &Nothing);
        c.Bind("ForceHardwareSync", &Nothing);
        BindTextureMethods(c);
        BindSnapshotMethods(c);
        BindMeshMethods(c);
        return c;
    }();
    static ShaderApiObject object{ { nativeClass.Vtable(), &nativeClass } };
    return &object;
}

}
