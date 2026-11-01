#include "shaderapi_shadow.h"

#include "log.h"
#include "shaderapi_meshes.h"
#include "shaderapi_pipelines.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace gmodpp::native {

namespace {

constexpr size_t kShaderShadowSlots = 57;
constexpr int kSamplers = 16;
// Vertices narrower than this get a texture coordinate of padding (shaderapidx9.dll 0x18002b2c0).
constexpr int kMinimumVertexSize = 17;

ShadowState g_state;
// A deque keeps its elements in place, so the map can key on their bytes.
std::deque<ShadowState> g_snapshots;
std::unordered_map<std::string_view, short> g_snapshotIds;

std::string_view Bytes(const ShadowState& state) { return { reinterpret_cast<const char*>(&state), sizeof(state) }; }

void SetSamplerBit(std::int32_t& mask, int sampler, bool enable)
{
    if (sampler < 0 || sampler >= kSamplers)
        return;
    mask = enable ? (mask | (1 << sampler)) : (mask & ~(1 << sampler));
}

// shaderapidx9.dll's CShaderShadowDX8::SetDefaultState (0x18002add0) calls the setters with these values; the stencil
// setters (slots 5-12) do nothing in GMod.
void SetDefaultState(NativeObject*) { g_state = ShadowState{}; }
void DepthFunc(NativeObject*, int func) { g_state.depthFunc = func; }
void EnableDepthWrites(NativeObject*, bool enable) { g_state.depthWrite = enable; }
void EnableDepthTest(NativeObject*, bool enable) { g_state.depthTest = enable; }
void EnablePolyOffset(NativeObject*, int mode) { g_state.polyOffset = mode; }
void IgnoreInt(NativeObject*, int) {}
void IgnoreBool(NativeObject*, bool) {}
void EnableColorWrites(NativeObject*, bool enable) { g_state.colorWrite = enable; }
void EnableAlphaWrites(NativeObject*, bool enable) { g_state.alphaWrite = enable; }
void EnableBlending(NativeObject*, bool enable) { g_state.blend = enable; }

void BlendFunc(NativeObject*, int source, int destination)
{
    g_state.srcBlend = source;
    g_state.dstBlend = destination;
}

void EnableAlphaTest(NativeObject*, bool enable) { g_state.alphaTest = enable; }

void AlphaFunc(NativeObject*, int func, float reference)
{
    if (func < 0 || func > 7) {
        log::Info("Native: AlphaFunc {} is invalid", func);
        return;
    }
    g_state.alphaFunc = func;
    g_state.alphaRef = static_cast<std::int32_t>(reference * 255.0f);
}

void PolyMode(NativeObject*, int face, int mode)
{
    if (face == 0 || face == 2)
        g_state.polyModeFront = mode;
    if (face == 1 || face == 2)
        g_state.polyModeBack = mode;
}

void EnableCulling(NativeObject*, bool enable) { g_state.culling = enable; }
void EnableConstantColor(NativeObject*, bool enable) { g_state.constantColor = enable; }

// shaderapidx9.dll 0x18002b2c0: the format of a shader's vertices, without bone indices and marked as a vertex
// shader format, padded with a one-component texture coordinate when narrower than 17 bytes.
void VertexShaderVertexFormat(NativeObject*, unsigned flags, int texCoords, int* dimensions, int userDataSize)
{
    const std::uint64_t base = (flags & ~kVertexBoneIndex) | kVertexFormatVertexShader;
    g_state.vertexFormat = ComputeVertexFormat(base, texCoords, dimensions, 0, userDataSize);
    if (VertexSize(g_state.vertexFormat) < kMinimumVertexSize) {
        static bool warned = false;
        if (!std::exchange(warned, true))
            log::Info("Native: a shader asks for a vertex format narrower than {} bytes; padding it as shaderapidx9 does", kMinimumVertexSize);
        const int padding = 1;
        g_state.vertexFormat = ComputeVertexFormat(base | kVertexPosition, 1, &padding, 0, userDataSize);
    }
}

void SetVertexShader(NativeObject*, const char* name, int staticIndex)
{
    g_state.vertexShader = name != nullptr ? FindShaderFile(name, true) : nullptr;
    g_state.vertexStaticIndex = staticIndex;
}

void SetPixelShader(NativeObject*, const char* name, int staticIndex)
{
    g_state.pixelShader = name != nullptr ? FindShaderFile(name, false) : nullptr;
    g_state.pixelStaticIndex = staticIndex;
}

void EnableLighting(NativeObject*, bool enable) { g_state.lighting = enable; }
void EnableSpecular(NativeObject*, bool enable) { g_state.specular = enable; }
void EnableSRGBWrite(NativeObject*, bool enable) { g_state.srgbWrite = enable; }
void EnableSRGBRead(NativeObject*, int sampler, bool enable) { SetSamplerBit(g_state.srgbRead, sampler, enable); }
void EnableVertexBlend(NativeObject*, bool enable) { g_state.vertexBlend = enable; }
void OverbrightValue(NativeObject*, int, float) {}
void EnableTexture(NativeObject*, int sampler, bool enable) { SetSamplerBit(g_state.textures, sampler, enable); }
void EnableTexGen(NativeObject*, int, bool) {}
void TexGen(NativeObject*, int, int) {}
void EnableCustomPixelPipe(NativeObject*, bool enable) { g_state.customPixelPipe = enable; }
void CustomTextureStages(NativeObject*, int) {}
void CustomTextureOperation(NativeObject*, int, int, int, int, int) {}
void DrawFlags(NativeObject*, unsigned flags) { g_state.drawFlags = static_cast<std::int32_t>(flags); }
void EnableAlphaPipe(NativeObject*, bool enable) { g_state.alphaPipe = enable; }
void EnableConstantAlpha(NativeObject*, bool enable) { g_state.constantAlpha = enable; }
void EnableVertexAlpha(NativeObject*, bool enable) { g_state.vertexAlpha = enable; }
void EnableTextureAlpha(NativeObject*, int, bool) {}
void EnableBlendingSeparateAlpha(NativeObject*, bool enable) { g_state.separateAlpha = enable; }

void BlendFuncSeparateAlpha(NativeObject*, int source, int destination)
{
    g_state.srcBlendAlpha = source;
    g_state.dstBlendAlpha = destination;
}

void FogMode(NativeObject*, int mode) { g_state.fogMode = mode; }
void SetDiffuseMaterialSource(NativeObject*, int source) { g_state.diffuseMaterialSource = source; }
void SetMorphFormat(NativeObject*, unsigned format) { g_state.morphFormat = static_cast<std::int32_t>(format); }
void DisableFogGammaCorrection(NativeObject*, bool disable) { g_state.disableFogGammaCorrection = disable; }
void EnableAlphaToCoverage(NativeObject*, bool enable) { g_state.alphaToCoverage = enable; }
void SetShadowDepthFiltering(NativeObject*, int sampler) { SetSamplerBit(g_state.shadowFiltering, sampler, true); }
void BlendOp(NativeObject*, int op) { g_state.blendOp = op; }
void BlendOpSeparateAlpha(NativeObject*, int op) { g_state.blendOpAlpha = op; }

short TakeSnapshot(NativeObject*)
{
    auto found = g_snapshotIds.find(Bytes(g_state));
    if (found != g_snapshotIds.end())
        return found->second;
    if (g_snapshots.size() >= 0x7fff) {
        log::Info("Native: too many state snapshots");
        return -1;
    }
    const short id = static_cast<short>(g_snapshots.size());
    g_snapshots.push_back(g_state);
    g_snapshotIds.emplace(Bytes(g_snapshots.back()), id);
    PrecacheShaders(g_state);
    return id;
}

// shaderapidx9.dll keeps these as flags of each snapshot's aggregate state (0x180018e70, 0x180018c50, 0x180018ce0).
bool IsTranslucent(NativeObject*, short id)
{
    const ShadowState* state = Snapshot(id);
    return state != nullptr && state->blend;
}

bool IsAlphaTested(NativeObject*, short id)
{
    const ShadowState* state = Snapshot(id);
    return state != nullptr && state->alphaTest;
}

bool UsesVertexAndPixelShaders(NativeObject*, short id)
{
    const ShadowState* state = Snapshot(id);
    return state != nullptr && state->vertexShader != nullptr;
}

bool IsDepthWriteEnabled(NativeObject*, short id)
{
    const ShadowState* state = Snapshot(id);
    return state != nullptr && state->depthWrite;
}

// shaderapidx9.dll 0x180013cd0 (ComputeVertexFormat at 0x180013cc0 calls it): the union of the snapshots' formats.
std::uint64_t ComputeVertexUsage(NativeObject*, int count, short* ids)
{
    if (count <= 0)
        return 0;
    if (count == 1) {
        const ShadowState* state = Snapshot(ids[0]);
        return state != nullptr ? state->vertexFormat : 0;
    }
    std::uint64_t flags = 0;
    int bones = 0;
    int userData = 0;
    int dimensions[kMaxTexCoords] = {};
    for (int i = 0; i < count; ++i) {
        const ShadowState* state = Snapshot(ids[i]);
        if (state == nullptr)
            continue;
        flags |= state->vertexFormat & 0x7ff;
        bones = std::max(bones, BoneWeights(state->vertexFormat));
        userData = std::max(userData, UserDataSize(state->vertexFormat));
        for (int t = 0; t < kMaxTexCoords; ++t)
            dimensions[t] = std::max(dimensions[t], TexCoordSize(t, state->vertexFormat));
    }
    return ComputeVertexFormat(flags, kMaxTexCoords, dimensions, bones, userData);
}

std::uint64_t SnapshotVertexFormat(NativeObject* self, int count, short* ids) { return ComputeVertexUsage(self, count, ids); }

unsigned ComputeMorphFormat(NativeObject*, int count, short* ids)
{
    unsigned format = 0;
    for (int i = 0; i < count; ++i) {
        if (const ShadowState* state = Snapshot(ids[i]))
            format |= static_cast<unsigned>(state->morphFormat);
    }
    return format;
}

void ClearSnapshots(NativeObject*)
{
    g_snapshotIds.clear();
    g_snapshots.clear();
}

}

const ShadowState* Snapshot(int id) { return id >= 0 && id < static_cast<int>(g_snapshots.size()) ? &g_snapshots[static_cast<size_t>(id)] : nullptr; }

void BindSnapshotMethods(NativeClass& c)
{
    c.Bind("TakeSnapshot", &TakeSnapshot);
    c.Bind("IsTranslucent", &IsTranslucent);
    c.Bind("IsAlphaTested", &IsAlphaTested);
    c.Bind("UsesVertexAndPixelShaders", &UsesVertexAndPixelShaders);
    c.Bind("IsDepthWriteEnabled", &IsDepthWriteEnabled);
    c.Bind("ComputeVertexFormat", &SnapshotVertexFormat);
    c.Bind("ComputeVertexUsage", &ComputeVertexUsage);
    c.Bind("ComputeMorphFormat", &ComputeMorphFormat);
    c.Bind("ClearSnapshots", &ClearSnapshots);
}

NativeObject* ShaderShadow()
{
    static NativeClass nativeClass = [] {
        NativeClass c("IShaderShadow", { shim::kShaderShadowSlots, shim::kShaderShadowSlotsCount }, kShaderShadowSlots);
        c.Bind("SetDefaultState", &SetDefaultState);
        c.Bind("DepthFunc", &DepthFunc);
        c.Bind("EnableDepthWrites", &EnableDepthWrites);
        c.Bind("EnableDepthTest", &EnableDepthTest);
        c.Bind("EnablePolyOffset", &EnablePolyOffset);
        c.Bind("EnableStencil", &IgnoreBool);
        c.Bind("StencilFunc", &IgnoreInt);
        c.Bind("StencilPassOp", &IgnoreInt);
        c.Bind("StencilFailOp", &IgnoreInt);
        c.Bind("StencilDepthFailOp", &IgnoreInt);
        c.Bind("StencilReference", &IgnoreInt);
        c.Bind("StencilMask", &IgnoreInt);
        c.Bind("StencilWriteMask", &IgnoreInt);
        c.Bind("EnableColorWrites", &EnableColorWrites);
        c.Bind("EnableAlphaWrites", &EnableAlphaWrites);
        c.Bind("EnableBlending", &EnableBlending);
        c.Bind("BlendFunc", &BlendFunc);
        c.Bind("EnableAlphaTest", &EnableAlphaTest);
        c.Bind("AlphaFunc", &AlphaFunc);
        c.Bind("PolyMode", &PolyMode);
        c.Bind("EnableCulling", &EnableCulling);
        c.Bind("EnableConstantColor", &EnableConstantColor);
        c.Bind("VertexShaderVertexFormat", &VertexShaderVertexFormat);
        c.Bind("SetVertexShader", &SetVertexShader);
        c.Bind("SetPixelShader", &SetPixelShader);
        c.Bind("EnableLighting", &EnableLighting);
        c.Bind("EnableSpecular", &EnableSpecular);
        c.Bind("EnableSRGBWrite", &EnableSRGBWrite);
        c.Bind("EnableSRGBRead", &EnableSRGBRead);
        c.Bind("EnableVertexBlend", &EnableVertexBlend);
        c.Bind("OverbrightValue", &OverbrightValue);
        c.Bind("EnableTexture", &EnableTexture);
        c.Bind("EnableTexGen", &EnableTexGen);
        c.Bind("TexGen", &TexGen);
        c.Bind("EnableCustomPixelPipe", &EnableCustomPixelPipe);
        c.Bind("CustomTextureStages", &CustomTextureStages);
        c.Bind("CustomTextureOperation", &CustomTextureOperation);
        c.Bind("DrawFlags", &DrawFlags);
        c.Bind("EnableAlphaPipe", &EnableAlphaPipe);
        c.Bind("EnableConstantAlpha", &EnableConstantAlpha);
        c.Bind("EnableVertexAlpha", &EnableVertexAlpha);
        c.Bind("EnableTextureAlpha", &EnableTextureAlpha);
        c.Bind("EnableBlendingSeparateAlpha", &EnableBlendingSeparateAlpha);
        c.Bind("BlendFuncSeparateAlpha", &BlendFuncSeparateAlpha);
        c.Bind("FogMode", &FogMode);
        c.Bind("SetDiffuseMaterialSource", &SetDiffuseMaterialSource);
        c.Bind("SetMorphFormat", &SetMorphFormat);
        c.Bind("DisableFogGammaCorrection", &DisableFogGammaCorrection);
        c.Bind("EnableAlphaToCoverage", &EnableAlphaToCoverage);
        c.Bind("SetShadowDepthFiltering", &SetShadowDepthFiltering);
        c.Bind("BlendOp", &BlendOp);
        c.Bind("BlendOpSeparateAlpha", &BlendOpSeparateAlpha);
        return c;
    }();
    static NativeObject object{ nativeClass.Vtable(), &nativeClass };
    return &object;
}

}
