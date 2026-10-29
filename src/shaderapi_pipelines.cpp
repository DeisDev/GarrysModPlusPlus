#include "shaderapi_pipelines.h"

#include "command_line.h"
#include "log.h"
#include "shader_compiler.h"
#include "shader_translator.h"
#include "shaderapi.h"
#include "shaderapi_textures.h"
#include "vcs.h"

#include <d3d9types.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp::native {

class ShaderFile {
public:
    std::string name;
    bool vertex = false;
    std::vector<std::uint8_t> bytes;
    vcs::Directory directory;
    // Decoded static combos: their dynamic combos' bytecode by dynamic index.
    std::unordered_map<std::uint32_t, std::vector<std::vector<DWORD>>> statics;
};

namespace {

// Bumped whenever the translator's output changes, so the disk cache misses old translations.
constexpr std::uint32_t kTranslatorVersion = 3;
constexpr std::uint32_t kCacheMagic = 0x53504d47;
// IBaseFileSystem (public/filesystem.h), confirmed in filesystem_stdio.dll (engine_interfaces.cpp).
constexpr unsigned kFileSystemRead = 0;
constexpr unsigned kFileSystemOpen = 2;
constexpr unsigned kFileSystemClose = 3;
constexpr size_t kReadChunk = 1 << 20;
constexpr UINT kStateBufferBytes = 576;
constexpr UINT kConstantAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr UINT kZeroStream = 3;
// Unbound samplers read a null descriptor of the shader's texture type.
constexpr size_t kTextureTypes = 4;

std::uint64_t Fnv(const void* data, size_t size, std::uint64_t hash = 14695981039346656037ull)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

template <class Fn>
Fn FileSystemSlot(unsigned slot)
{
    return reinterpret_cast<Fn>((*static_cast<void***>(GetBackend().fileSystem))[slot]);
}

// Reads to the end rather than trusting the size, as engine_interfaces.cpp does for VPK files.
bool ReadGameFile(const std::string& path, std::vector<std::uint8_t>& data)
{
    void* fileSystem = GetBackend().fileSystem;
    if (fileSystem == nullptr)
        return false;
    void* file = FileSystemSlot<void* (*)(void*, const char*, const char*, const char*)>(kFileSystemOpen)(fileSystem, path.c_str(), "rb", "GAME");
    if (file == nullptr)
        return false;
    data.clear();
    for (;;) {
        const size_t offset = data.size();
        data.resize(offset + kReadChunk);
        const int read = FileSystemSlot<int (*)(void*, void*, int, void*)>(kFileSystemRead)(fileSystem, data.data() + offset, static_cast<int>(kReadChunk), file);
        data.resize(offset + static_cast<size_t>(std::max(read, 0)));
        if (read < static_cast<int>(kReadChunk))
            break;
    }
    FileSystemSlot<void (*)(void*, void*)>(kFileSystemClose)(fileSystem, file);
    return !data.empty();
}

std::span<const DWORD> Combo(const ShaderFile& constFile, int staticIndex, int dynamicIndex)
{
    ShaderFile& file = const_cast<ShaderFile&>(constFile);
    const int dynamicCombos = std::max(file.directory.header.dynamicCombos, 1);
    const std::uint32_t staticCombo = static_cast<std::uint32_t>(staticIndex) / static_cast<std::uint32_t>(dynamicCombos);
    auto found = file.statics.find(staticCombo);
    if (found == file.statics.end()) {
        std::vector<vcs::Combo> combos;
        std::string error;
        std::vector<std::vector<DWORD>> dynamics(static_cast<size_t>(dynamicCombos));
        if (!vcs::ReadStaticCombo(file.bytes, file.directory, staticCombo, combos, error))
            log::Info("Native: shader {}: {}", file.name, error);
        for (vcs::Combo& combo : combos) {
            if (combo.dynamicCombo < dynamics.size())
                dynamics[combo.dynamicCombo] = std::move(combo.bytecode);
            else
                log::Info("Native: shader {} static combo {} has dynamic combo {} of {}", file.name, staticCombo, combo.dynamicCombo, dynamicCombos);
        }
        found = file.statics.emplace(staticCombo, std::move(dynamics)).first;
    }
    if (dynamicIndex < 0 || dynamicIndex >= static_cast<int>(found->second.size()))
        return {};
    return found->second[static_cast<size_t>(dynamicIndex)];
}

struct CompiledShader {
    translator::Translation info;
    std::vector<unsigned char> dxil;
    bool failed = false;
};

struct PipelineCache {
    bool initialized = false;
    bool usable = false;
    ShaderCompiler compiler;
    std::filesystem::path cacheDirectory;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12Resource> zeroBuffer;
    std::array<UINT, kTextureTypes> nullViews = {};
    std::unordered_map<std::string, std::unique_ptr<ShaderFile>> files;
    std::unordered_set<std::string> missingFiles;
    std::unordered_map<const DWORD*, std::unique_ptr<CompiledShader>> baseInfo;
    std::unordered_map<std::uint64_t, std::unique_ptr<CompiledShader>> shaders;
    std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> pipelines;
    std::unordered_map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> blitPipelines;
    std::vector<unsigned char> blitVertexShader;
    std::vector<unsigned char> blitPixelShader;
    std::uint64_t boundGeneration = ~0ull;
    ID3D12PipelineState* boundPipeline = nullptr;
    size_t compiled = 0;
    double compileSeconds = 0;
};

PipelineCache g_cache;
int g_drawLog = 0;
int g_drawLimit = -1;
int g_frameDraws = 0;

bool CreateRootSignature()
{
    Backend& backend = GetBackend();
    D3D12_ROOT_PARAMETER1 parameters[4] = {};
    for (UINT i = 0; i < 4; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[i].Descriptor.ShaderRegister = i % 2;
        parameters[i].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        parameters[i].ShaderVisibility = i < 2 ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 4;
    desc.Desc_1_1.pParameters = parameters;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &errors);
    if (SUCCEEDED(hr))
        hr = backend.Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_cache.rootSignature));
    if (FAILED(hr)) {
        log::Info("Native: creating the translated shaders' root signature failed: {:#010x} {}", static_cast<unsigned>(hr),
            errors != nullptr ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "");
        return false;
    }
    return true;
}

bool CreateZeroBuffer()
{
    const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 16;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = GetBackend().Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&g_cache.zeroBuffer));
    if (FAILED(hr)) {
        log::Info("Native: creating the zero vertex stream failed: {:#010x}", static_cast<unsigned>(hr));
        return false;
    }
    // Vertex inputs a mesh lacks read (0, 0, 0, 1), as missing components of short formats do.
    float* data = nullptr;
    g_cache.zeroBuffer->Map(0, nullptr, reinterpret_cast<void**>(&data));
    const float values[4] = { 0, 0, 0, 1 };
    std::memcpy(data, values, sizeof(values));
    g_cache.zeroBuffer->Unmap(0, nullptr);
    return true;
}

void CreateNullViews()
{
    Backend& backend = GetBackend();
    for (size_t type = 0; type < kTextureTypes; ++type) {
        const UINT index = backend.Descriptors().Allocate();
        g_cache.nullViews[type] = index;
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (type == static_cast<size_t>(translator::TextureType::Cube)) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            view.TextureCube.MipLevels = 1;
        } else if (type == static_cast<size_t>(translator::TextureType::Volume)) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            view.Texture3D.MipLevels = 1;
        } else {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
        }
        backend.Device()->CreateShaderResourceView(nullptr, &view, backend.Descriptors().Cpu(index));
    }
}

bool Initialize()
{
    if (g_cache.initialized)
        return g_cache.usable;
    g_cache.initialized = true;
    const std::filesystem::path game = ExecutablePath().parent_path();
    std::string error;
    if (!g_cache.compiler.Load(game / L"bin" / L"win64" / L"gmodpp", error)) {
        log::Info("Native: the shader compiler is unavailable: {}", error);
        return false;
    }
    g_cache.cacheDirectory = game / L"gmodpp_cache" / L"shaders";
    std::error_code ignored;
    std::filesystem::create_directories(g_cache.cacheDirectory, ignored);
    if (!CreateRootSignature() || !CreateZeroBuffer())
        return false;
    CreateNullViews();
    g_cache.usable = true;
    return true;
}

bool LoadCached(std::uint64_t key, CompiledShader& shader)
{
    std::ifstream file(g_cache.cacheDirectory / std::format("{:016x}.bin", key), std::ios::binary);
    if (!file)
        return false;
    std::uint32_t header[6] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!file || header[0] != kCacheMagic || header[1] != kTranslatorVersion)
        return false;
    shader.info.firstFloatConstant = header[2];
    shader.info.floatConstantCount = header[3];
    shader.info.colorOutputs = header[4];
    shader.info.writesDepth = header[5] != 0;
    file.read(reinterpret_cast<char*>(shader.info.samplers.data()), sizeof(shader.info.samplers));
    std::uint32_t inputs = 0;
    file.read(reinterpret_cast<char*>(&inputs), sizeof(inputs));
    shader.info.inputs.resize(inputs);
    file.read(reinterpret_cast<char*>(shader.info.inputs.data()), static_cast<std::streamsize>(inputs * sizeof(translator::VertexInput)));
    std::uint32_t size = 0;
    file.read(reinterpret_cast<char*>(&size), sizeof(size));
    shader.dxil.resize(size);
    file.read(reinterpret_cast<char*>(shader.dxil.data()), size);
    return static_cast<bool>(file);
}

void StoreCached(std::uint64_t key, const CompiledShader& shader)
{
    std::ofstream file(g_cache.cacheDirectory / std::format("{:016x}.bin", key), std::ios::binary | std::ios::trunc);
    const std::uint32_t header[6] = { kCacheMagic, kTranslatorVersion, shader.info.firstFloatConstant, shader.info.floatConstantCount,
        shader.info.colorOutputs, shader.info.writesDepth ? 1u : 0u };
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(shader.info.samplers.data()), sizeof(shader.info.samplers));
    const std::uint32_t inputs = static_cast<std::uint32_t>(shader.info.inputs.size());
    file.write(reinterpret_cast<const char*>(&inputs), sizeof(inputs));
    file.write(reinterpret_cast<const char*>(shader.info.inputs.data()), static_cast<std::streamsize>(inputs * sizeof(translator::VertexInput)));
    const std::uint32_t size = static_cast<std::uint32_t>(shader.dxil.size());
    file.write(reinterpret_cast<const char*>(&size), sizeof(size));
    file.write(reinterpret_cast<const char*>(shader.dxil.data()), size);
}

// The translation with the default variant, for what a draw must know before choosing its variant.
const CompiledShader* BaseInfo(std::span<const DWORD> code)
{
    auto found = g_cache.baseInfo.find(code.data());
    if (found != g_cache.baseInfo.end())
        return found->second->failed ? nullptr : found->second.get();
    auto shader = std::make_unique<CompiledShader>();
    std::string error;
    if (!translator::Translate(code, {}, shader->info, error)) {
        log::Info("Native: translating a shader ({} tokens) failed: {}", code.size(), error);
        shader->failed = true;
    }
    shader->info.hlsl.clear();
    const CompiledShader* result = shader->failed ? nullptr : shader.get();
    g_cache.baseInfo.emplace(code.data(), std::move(shader));
    return result;
}

const CompiledShader* Compiled(std::span<const DWORD> code, const translator::Variant& variant, const char* fileName)
{
    std::uint64_t key = Fnv(code.data(), code.size_bytes());
    key = Fnv(&variant.shadowSamplers, sizeof(variant.shadowSamplers), key);
    key = Fnv(&variant.integerInputs, sizeof(variant.integerInputs), key);
    key = Fnv(&variant.signedInputs, sizeof(variant.signedInputs), key);
    key = Fnv(&variant.alphaTest, sizeof(variant.alphaTest), key);
    key = Fnv(&variant.clipPlanes, sizeof(variant.clipPlanes), key);
    key = Fnv(&kTranslatorVersion, sizeof(kTranslatorVersion), key);
    auto found = g_cache.shaders.find(key);
    if (found != g_cache.shaders.end())
        return found->second->failed ? nullptr : found->second.get();
    auto shader = std::make_unique<CompiledShader>();
    if (!LoadCached(key, *shader)) {
        *shader = {};
        const auto start = std::chrono::steady_clock::now();
        std::string error;
        if (!translator::Translate(code, variant, shader->info, error)) {
            log::Info("Native: translating a combo of {} failed: {}", fileName, error);
            shader->failed = true;
        } else {
            const wchar_t* profile = shader->info.stage == translator::Stage::Vertex ? L"vs_6_6" : L"ps_6_6";
            if (!g_cache.compiler.Compile(shader->info.hlsl, profile, shader->dxil, error)) {
                log::Info("Native: compiling a combo of {} failed: {}", fileName, error);
                std::ofstream(g_cache.cacheDirectory / std::format("{:016x}.failed.hlsl", key)) << shader->info.hlsl << "\n/*\n" << error << "\n*/\n";
                shader->failed = true;
            } else {
                StoreCached(key, *shader);
                static const bool developer = HasArgument(L"-gmodpp_dev");
                if (developer)
                    std::ofstream(g_cache.cacheDirectory / std::format("{:016x}.{}.hlsl", key, fileName)) << shader->info.hlsl;
            }
        }
        shader->info.hlsl.clear();
        g_cache.compileSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (++g_cache.compiled % 100 == 0)
            log::Info("Native: {} shaders translated and compiled ({:.1f} s)", g_cache.compiled, g_cache.compileSeconds);
    }
    const CompiledShader* result = shader->failed ? nullptr : shader.get();
    g_cache.shaders.emplace(key, std::move(shader));
    return result;
}

D3D12_BLEND Blend(int factor, bool alpha)
{
    switch (factor) {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 2: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 3: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 4: return D3D12_BLEND_SRC_ALPHA;
    case 5: return D3D12_BLEND_INV_SRC_ALPHA;
    case 6: return D3D12_BLEND_DEST_ALPHA;
    case 7: return D3D12_BLEND_INV_DEST_ALPHA;
    case 8: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
    case 9: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case 10: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP BlendOp(int op)
{
    static constexpr D3D12_BLEND_OP kOps[] = { D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_REV_SUBTRACT, D3D12_BLEND_OP_MIN, D3D12_BLEND_OP_MAX };
    return op >= 0 && op < 5 ? kOps[op] : D3D12_BLEND_OP_ADD;
}

// IShaderShadow::DepthFunc (0x18002a170), with the nearer/farther pairs swapped under reverse depth.
D3D12_COMPARISON_FUNC DepthFunc(int func, bool reverse)
{
    switch (func) {
    case 0: return D3D12_COMPARISON_FUNC_NEVER;
    case 1: return reverse ? D3D12_COMPARISON_FUNC_GREATER : D3D12_COMPARISON_FUNC_LESS;
    case 2: return D3D12_COMPARISON_FUNC_EQUAL;
    case 3: return reverse ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 4: return reverse ? D3D12_COMPARISON_FUNC_LESS : D3D12_COMPARISON_FUNC_GREATER;
    case 5: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 6: return reverse ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

const char* Semantic(BYTE usage)
{
    static constexpr const char* kNames[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
        "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };
    return usage < std::size(kNames) ? kNames[usage] : "TEXCOORD";
}

// Everything a pipeline state depends on, zero-filled so its bytes can be hashed.
struct PipelineKey {
    const CompiledShader* vertexShader;
    const CompiledShader* pixelShader;
    D3D12_BLEND_DESC blend;
    D3D12_RASTERIZER_DESC raster;
    D3D12_DEPTH_STENCIL_DESC depthStencil;
    D3D12_INPUT_ELEMENT_DESC elements[16];
    UINT elementCount;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
    UINT targets;
    DXGI_FORMAT targetFormats[4];
    DXGI_FORMAT depthFormat;
};

ID3D12PipelineState* Pipeline(PipelineKey& key)
{
    // Semantic names point to static strings, so hashing the pointers is enough.
    const std::uint64_t hash = Fnv(&key, sizeof(key));
    auto found = g_cache.pipelines.find(hash);
    if (found != g_cache.pipelines.end())
        return found->second.Get();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = g_cache.rootSignature.Get();
    desc.VS = { key.vertexShader->dxil.data(), key.vertexShader->dxil.size() };
    desc.PS = { key.pixelShader->dxil.data(), key.pixelShader->dxil.size() };
    desc.BlendState = key.blend;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState = key.raster;
    desc.DepthStencilState = key.depthStencil;
    desc.InputLayout = { key.elements, key.elementCount };
    desc.PrimitiveTopologyType = key.topology;
    desc.NumRenderTargets = key.targets;
    std::copy(std::begin(key.targetFormats), std::end(key.targetFormats), desc.RTVFormats);
    desc.DSVFormat = key.depthFormat;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    HRESULT hr = GetBackend().Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr))
        log::Info("Native: creating a pipeline state failed: {:#010x}", static_cast<unsigned>(hr));
    ID3D12PipelineState* result = pipeline.Get();
    g_cache.pipelines.emplace(hash, std::move(pipeline));
    if (g_cache.pipelines.size() % 200 == 0)
        log::Info("Native: {} pipeline states", g_cache.pipelines.size());
    return result;
}

D3D12_GPU_VIRTUAL_ADDRESS UploadConstants(const void* data, UINT size)
{
    Backend::Upload upload = GetBackend().AllocateUpload(std::max(size, 16u), kConstantAlignment);
    if (upload.data == nullptr)
        return 0;
    std::memcpy(upload.data, data, size);
    return upload.buffer->GetGPUVirtualAddress() + upload.offset;
}

// gmodpp_state (register b1) of shader_translator.cpp.
struct StateBuffer {
    int ints[16][4];
    std::uint32_t bools[16];
    float halfPixel[4];
    std::uint32_t textures[16];
    std::uint32_t samplers[16];
    float alpha[4];
    float clipPlanes[6][4];
};
static_assert(sizeof(StateBuffer) == kStateBufferBytes);

bool IsTarget(const Texture* texture)
{
    if (texture == nullptr)
        return false;
    const DynamicState& state = Dynamic();
    for (std::intptr_t handle : state.colorTargets) {
        if (TextureFromHandle(handle) == texture)
            return true;
    }
    return TextureFromHandle(state.depthTarget) == texture;
}

// Fills one stage's texture and sampler indices and moves its textures into a shader resource state.
void BindSamplers(const CompiledShader& shader, const std::intptr_t* handles, int count, std::int32_t srgbRead, StateBuffer& state,
    std::uint16_t* shadowSamplers)
{
    Backend& backend = GetBackend();
    const DynamicState& dynamic = Dynamic();
    for (int s = 0; s < count; ++s) {
        const translator::TextureType type = shader.info.samplers[static_cast<size_t>(s)];
        if (type == translator::TextureType::None)
            continue;
        Texture* texture = TextureFromHandle(handles[s]);
        if (texture != nullptr && (texture->copies.empty() || IsTarget(texture)))
            texture = nullptr;
        if (texture == nullptr) {
            state.textures[s] = g_cache.nullViews[static_cast<size_t>(type)];
            D3D12_SAMPLER_DESC desc = {};
            desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            desc.MaxLOD = D3D12_FLOAT32_MAX;
            state.samplers[s] = backend.SamplerIndex(desc);
            continue;
        }
        Texture::Copy& copy = texture->Current();
        const bool srgb = ((srgbRead >> s) & 1) != 0 && copy.srgbView != DescriptorHeap::kInvalid;
        state.textures[s] = srgb ? copy.srgbView : copy.view;
        backend.Transition(copy.resource, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        const bool border = s < kSamplers && dynamic.borderAddress[static_cast<size_t>(s)];
        const int point = s < kSamplers ? dynamic.pointFilter[static_cast<size_t>(s)] : 0;
        state.samplers[s] = backend.SamplerIndex(SamplerFor(*texture, border, point, dynamic.anisotropy, dynamic.minFilterOverride, dynamic.magFilterOverride));
        if (shadowSamplers != nullptr && (texture->storage == kFormatNvDst16 || texture->storage == kFormatNvDst24))
            *shadowSamplers |= static_cast<std::uint16_t>(1u << s);
    }
}

}

namespace {

// The draw half of StretchRect: a triangle covering the destination rectangle samples the source rectangle.
constexpr const char* kBlitVertexShader = R"(struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint vertex : SV_VertexID)
{
    Output output;
    float2 t = float2((vertex << 1) & 2, vertex & 2);
    output.position = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    output.uv = t;
    return output;
}
)";
constexpr const char* kBlitPixelShader = R"(cbuffer Blit : register(b0) { float4 rect; uint4 indices; };
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    Texture2D<float4> source = ResourceDescriptorHeap[indices.x];
    SamplerState linearClamp = SamplerDescriptorHeap[indices.y];
    return source.SampleLevel(linearClamp, rect.xy + uv * rect.zw, 0);
}
)";

ID3D12PipelineState* BlitPipeline(DXGI_FORMAT format)
{
    auto found = g_cache.blitPipelines.find(format);
    if (found != g_cache.blitPipelines.end())
        return found->second.Get();
    std::string error;
    if (g_cache.blitVertexShader.empty() && (!g_cache.compiler.Compile(kBlitVertexShader, L"vs_6_6", g_cache.blitVertexShader, error)
            || !g_cache.compiler.Compile(kBlitPixelShader, L"ps_6_6", g_cache.blitPixelShader, error))) {
        log::Info("Native: compiling the copy shaders failed: {}", error);
        g_cache.blitVertexShader.clear();
        return nullptr;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = g_cache.rootSignature.Get();
    desc.VS = { g_cache.blitVertexShader.data(), g_cache.blitVertexShader.size() };
    desc.PS = { g_cache.blitPixelShader.data(), g_cache.blitPixelShader.size() };
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    HRESULT hr = GetBackend().Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr))
        log::Info("Native: creating the copy pipeline for format {} failed: {:#010x}", static_cast<int>(format), static_cast<unsigned>(hr));
    return g_cache.blitPipelines.emplace(format, std::move(pipeline)).first->second.Get();
}

// Formats a copy (CopyTextureRegion) can move between: the same typeless family.
DXGI_FORMAT Family(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    default: return format;
    }
}

}

void StretchRect(const CopySurface& source, const D3D12_RECT& sourceRect, const CopySurface& destination, const D3D12_RECT& destinationRect)
{
    Backend& backend = GetBackend();
    ID3D12GraphicsCommandList* commands = backend.Commands();
    const LONG width = sourceRect.right - sourceRect.left;
    const LONG height = sourceRect.bottom - sourceRect.top;
    if (width <= 0 || height <= 0 || destinationRect.right <= destinationRect.left || destinationRect.bottom <= destinationRect.top)
        return;
    if (width == destinationRect.right - destinationRect.left && height == destinationRect.bottom - destinationRect.top
        && Family(source.format) == Family(destination.format)) {
        backend.Transition(*source.resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
        backend.Transition(*destination.resource, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to = {};
        to.pResource = destination.resource->resource.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION from = {};
        from.pResource = source.resource->resource.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box = { static_cast<UINT>(sourceRect.left), static_cast<UINT>(sourceRect.top), 0, static_cast<UINT>(sourceRect.right),
            static_cast<UINT>(sourceRect.bottom), 1 };
        commands->CopyTextureRegion(&to, static_cast<UINT>(destinationRect.left), static_cast<UINT>(destinationRect.top), 0, &from, &box);
        return;
    }
    if (destination.target.ptr == 0 || !Initialize()) {
        log::Info("Native: a scaled copy into a texture that is not a render target was skipped");
        return;
    }
    ID3D12PipelineState* pipeline = BlitPipeline(destination.format);
    if (pipeline == nullptr)
        return;
    backend.Transition(*source.resource, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    backend.Transition(*destination.resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (g_cache.boundGeneration != backend.ListGeneration()) {
        g_cache.boundGeneration = backend.ListGeneration();
        ID3D12DescriptorHeap* heaps[] = { backend.Descriptors().Heap(), backend.SamplerHeap() };
        commands->SetDescriptorHeaps(2, heaps);
        commands->SetGraphicsRootSignature(g_cache.rootSignature.Get());
    }
    commands->SetPipelineState(pipeline);
    g_cache.boundPipeline = pipeline;
    D3D12_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    struct {
        float rect[4];
        std::uint32_t indices[4];
    } constants = { { static_cast<float>(sourceRect.left) / source.width, static_cast<float>(sourceRect.top) / source.height,
                        static_cast<float>(width) / source.width, static_cast<float>(height) / source.height },
        { source.texture, backend.SamplerIndex(sampler), 0, 0 } };
    commands->SetGraphicsRootConstantBufferView(2, UploadConstants(&constants, sizeof(constants)));
    commands->OMSetRenderTargets(1, &destination.target, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = { static_cast<float>(destinationRect.left), static_cast<float>(destinationRect.top),
        static_cast<float>(destinationRect.right - destinationRect.left), static_cast<float>(destinationRect.bottom - destinationRect.top), 0, 1 };
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &destinationRect);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
}

void LogNextDraws(int count) { g_drawLog = count; }

void LimitDraws(int count)
{
    if (count != g_drawLimit)
        log::Info("Native: drawing {} passes a frame", count);
    g_drawLimit = count;
    g_frameDraws = 0;
}

const ShaderFile* FindShaderFile(const char* name, bool vertex)
{
    std::string key = name;
    std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto found = g_cache.files.find(key);
    if (found != g_cache.files.end())
        return found->second.get();
    if (g_cache.missingFiles.contains(key))
        return nullptr;
    auto file = std::make_unique<ShaderFile>();
    file->name = name;
    file->vertex = vertex;
    // CShaderManager (0x180034cc0) tries shaders\vsh or shaders\psh, then shaders\fxc.
    if (!ReadGameFile(std::format("shaders\\fxc\\{}.vcs", key), file->bytes) && !ReadGameFile(std::format("shaders\\{}\\{}.vcs", vertex ? "vsh" : "psh", key), file->bytes)) {
        log::Info("Native: shader file {} was not found", name);
        g_cache.missingFiles.insert(key);
        return nullptr;
    }
    std::string error;
    if (!vcs::ReadDirectory(file->bytes, file->directory, error)) {
        log::Info("Native: shader file {}: {}", name, error);
        g_cache.missingFiles.insert(key);
        return nullptr;
    }
    return g_cache.files.emplace(key, std::move(file)).first->second.get();
}

void TargetSize(UINT& width, UINT& height)
{
    const DynamicState& state = Dynamic();
    if (const Texture* texture = TextureFromHandle(state.colorTargets[0])) {
        width = static_cast<UINT>(texture->width);
        height = static_cast<UINT>(texture->height);
    } else {
        width = GetBackend().Width();
        height = GetBackend().Height();
    }
}

Targets PrepareTargets(bool srgb)
{
    Backend& backend = GetBackend();
    const DynamicState& state = Dynamic();
    Targets targets;
    TargetSize(targets.width, targets.height);
    for (UINT i = 0; i < kRenderTargets; ++i) {
        const std::intptr_t handle = state.colorTargets[i];
        if (i == 0 && handle == kDefaultTarget) {
            backend.Transition(backend.BackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            targets.colors[0] = backend.BackBufferView(srgb);
            targets.colorFormats[0] = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : Backend::kBackBufferFormat;
            targets.count = 1;
            continue;
        }
        Texture* texture = TextureFromHandle(handle);
        if (texture == nullptr || texture->copies.empty() || texture->IsDepth())
            break;
        Texture::Copy& copy = texture->Current();
        const bool useSrgb = srgb && copy.srgbTarget.ptr != 0;
        backend.Transition(copy.resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
        targets.colors[i] = useSrgb ? copy.srgbTarget : copy.target;
        targets.colorFormats[i] = texture->TargetFormat(useSrgb);
        targets.count = i + 1;
    }
    if (state.depthTarget == kDefaultTarget) {
        backend.Transition(backend.DepthBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        targets.depth = backend.DepthBufferView();
        targets.depthFormat = Backend::kDepthFormat;
    } else if (Texture* depth = TextureFromHandle(state.depthTarget); depth != nullptr && depth->IsDepth() && !depth->copies.empty()) {
        backend.Transition(depth->Current().resource, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        targets.depth = depth->Current().target;
        targets.depthFormat = depth->TargetFormat(false);
    }
    backend.Commands()->OMSetRenderTargets(targets.count, targets.colors, FALSE, targets.depth.ptr != 0 ? &targets.depth : nullptr);
    return targets;
}

void DrawPass(const ShadowState& shadow, const MeshDraw& mesh)
{
    if (!Initialize())
        return;
    if (g_drawLimit >= 0 && g_frameDraws++ >= g_drawLimit)
        return;
    if (shadow.vertexShader == nullptr || shadow.pixelShader == nullptr) {
        static bool logged = false;
        if (!std::exchange(logged, true))
            log::Info("Native: a pass without vertex and pixel shaders was skipped; fixed-function rendering is not implemented");
        return;
    }
    Backend& backend = GetBackend();
    const DynamicState& dynamic = Dynamic();
    const std::span<const DWORD> vertexCode = Combo(*shadow.vertexShader, shadow.vertexStaticIndex, std::max(dynamic.vertexShaderIndex, 0));
    const std::span<const DWORD> pixelCode = Combo(*shadow.pixelShader, shadow.pixelStaticIndex, dynamic.pixelShaderIndex);
    if (vertexCode.empty() || pixelCode.empty()) {
        static bool logged = false;
        if (!std::exchange(logged, true))
            log::Info("Native: {} static {} or {} static {} has no code for its dynamic combo", shadow.vertexShader->name, shadow.vertexStaticIndex,
                shadow.pixelShader->name, shadow.pixelStaticIndex);
        return;
    }
    const CompiledShader* vertexBase = BaseInfo(vertexCode);
    const CompiledShader* pixelBase = BaseInfo(pixelCode);
    if (vertexBase == nullptr || pixelBase == nullptr)
        return;

    const Targets targets = PrepareTargets(shadow.srgbWrite != 0);

    StateBuffer pixelState = {};
    translator::Variant pixelVariant;
    BindSamplers(*pixelBase, dynamic.textures.data(), kSamplers, shadow.srgbRead, pixelState, &pixelVariant.shadowSamplers);
    if (shadow.alphaTest)
        pixelVariant.alphaTest = static_cast<std::uint8_t>(shadow.alphaFunc + 1);
    StateBuffer vertexState = {};
    BindSamplers(*vertexBase, dynamic.vertexTextures.data(), kVertexSamplers, 0, vertexState, nullptr);

    PipelineKey key;
    std::memset(&key, 0, sizeof(key));
    translator::Variant vertexVariant;
    const std::vector<VertexElement> elements = VertexElements(mesh.vertexFormat, mesh.streams[1].SizeInBytes != 0);
    for (const translator::VertexInput& input : vertexBase->info.inputs) {
        if (key.elementCount == std::size(key.elements))
            break;
        D3D12_INPUT_ELEMENT_DESC& element = key.elements[key.elementCount++];
        element.SemanticName = Semantic(input.usage);
        element.SemanticIndex = input.usageIndex;
        auto match = std::ranges::find_if(elements, [&](const VertexElement& e) { return e.usage == input.usage && e.usageIndex == input.usageIndex; });
        if (match == elements.end()) {
            element.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            element.InputSlot = kZeroStream;
            continue;
        }
        element.Format = match->format;
        element.InputSlot = match->stream;
        element.AlignedByteOffset = match->offset;
        if (match->format == DXGI_FORMAT_R8G8B8A8_UINT || match->format == DXGI_FORMAT_R16G16_SINT) {
            vertexVariant.integerInputs |= static_cast<std::uint16_t>(1u << input.reg);
            if (match->format == DXGI_FORMAT_R16G16_SINT)
                vertexVariant.signedInputs |= static_cast<std::uint16_t>(1u << input.reg);
        }
    }
    vertexVariant.clipPlanes = static_cast<std::uint8_t>(UserClipPlanes(vertexState.clipPlanes));
    const CompiledShader* vertexShader = Compiled(vertexCode, vertexVariant, shadow.vertexShader->name.c_str());
    const CompiledShader* pixelShader = Compiled(pixelCode, pixelVariant, shadow.pixelShader->name.c_str());
    if (vertexShader == nullptr || pixelShader == nullptr)
        return;
    key.vertexShader = vertexShader;
    key.pixelShader = pixelShader;

    D3D12_RENDER_TARGET_BLEND_DESC& blend = key.blend.RenderTarget[0];
    const DynamicState::BlendOverride& colorOverride = dynamic.blendOverride;
    const DynamicState::BlendOverride& alphaOverride = dynamic.alphaBlendOverride;
    const int source = colorOverride.active ? colorOverride.source : shadow.srcBlend;
    const int destination = colorOverride.active ? colorOverride.destination : shadow.dstBlend;
    const int op = colorOverride.active ? colorOverride.op : shadow.blendOp;
    blend.BlendEnable = colorOverride.active ? colorOverride.enable : shadow.blend != 0;
    blend.SrcBlend = Blend(source, false);
    blend.DestBlend = Blend(destination, false);
    blend.BlendOp = BlendOp(op);
    const bool separate = alphaOverride.active ? alphaOverride.enable : shadow.separateAlpha != 0;
    blend.SrcBlendAlpha = Blend(separate ? (alphaOverride.active ? alphaOverride.source : shadow.srcBlendAlpha) : source, true);
    blend.DestBlendAlpha = Blend(separate ? (alphaOverride.active ? alphaOverride.destination : shadow.dstBlendAlpha) : destination, true);
    blend.BlendOpAlpha = BlendOp(separate ? (alphaOverride.active ? alphaOverride.op : shadow.blendOpAlpha) : op);
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    const bool colorWrite = dynamic.overrideColorWrite ? dynamic.colorWriteEnable : shadow.colorWrite != 0;
    const bool alphaWrite = dynamic.overrideAlphaWrite ? dynamic.alphaWriteEnable : shadow.alphaWrite != 0;
    blend.RenderTargetWriteMask = static_cast<UINT8>((colorWrite ? D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE : 0)
        | (alphaWrite ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0));
    key.blend.AlphaToCoverageEnable = shadow.alphaToCoverage != 0;

    D3D12_RASTERIZER_DESC& raster = key.raster;
    raster.FillMode = shadow.polyModeFront == 1 ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    // Source's front faces are clockwise; MaterialCullMode_t 0 is D3DCULL_CCW, 1 D3DCULL_CW, 2 (GMod) none (0x180014c60).
    raster.CullMode = !shadow.culling || dynamic.cullMode == 2 ? D3D12_CULL_MODE_NONE : dynamic.cullMode == 1 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
    raster.DepthClipEnable = TRUE;
    if (shadow.polyOffset == 1 && targets.depth.ptr != 0) {
        static bool logged = false;
        if (!std::exchange(logged, true))
            log::Info("Native: decal depth bias is not applied yet");
    }

    D3D12_DEPTH_STENCIL_DESC& depth = key.depthStencil;
    const bool reverse = false;
    depth.DepthEnable = shadow.depthTest != 0 && targets.depth.ptr != 0;
    const bool depthWrite = dynamic.overrideDepthEnable ? dynamic.depthEnable : shadow.depthWrite != 0;
    depth.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = dynamic.forceDepthFuncEquals ? D3D12_COMPARISON_FUNC_EQUAL : DepthFunc(shadow.depthFunc, reverse);
    depth.StencilEnable = dynamic.stencilEnable && targets.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth.StencilReadMask = static_cast<UINT8>(dynamic.stencilTestMask);
    depth.StencilWriteMask = static_cast<UINT8>(dynamic.stencilWriteMask);
    depth.FrontFace = { static_cast<D3D12_STENCIL_OP>(dynamic.stencilFail), static_cast<D3D12_STENCIL_OP>(dynamic.stencilDepthFail),
        static_cast<D3D12_STENCIL_OP>(dynamic.stencilPass), static_cast<D3D12_COMPARISON_FUNC>(dynamic.stencilFunc) };
    depth.BackFace = depth.FrontFace;
    if (!depth.StencilEnable) {
        depth.FrontFace = depth.BackFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
        depth.StencilReadMask = depth.StencilWriteMask = 0;
    }

    switch (mesh.topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: key.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: key.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
    default: key.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
    }
    key.targets = targets.count;
    std::copy(std::begin(targets.colorFormats), std::end(targets.colorFormats), key.targetFormats);
    key.depthFormat = targets.depth.ptr != 0 ? targets.depthFormat : DXGI_FORMAT_UNKNOWN;
    ID3D12PipelineState* pipeline = Pipeline(key);
    if (pipeline == nullptr)
        return;

    const D3D12_VIEWPORT& viewport = dynamic.viewport;
    {
        static int diagnostics = 0;
        if (g_drawLog > 0) {
            --g_drawLog;
            ++diagnostics;
            auto c = [&](int r) { const float* v = dynamic.vertexFloats[static_cast<size_t>(r)].v; return std::format("({:.4g} {:.4g} {:.4g} {:.4g})", v[0], v[1], v[2], v[3]); };
            const Texture* t0 = TextureFromHandle(dynamic.textures[0]);
            log::Info("DIAG draw {} {}#{}+{} {}#{}+{} fmt {:#x} verts {} prims {}x{} vp {} {} {} {} c4 {} {} {} {} c8 {} {} {} {} c58 {} {} {} tex0 {} {}x{} rt0 {} first {} count {}",
                diagnostics, shadow.vertexShader->name, shadow.vertexStaticIndex, dynamic.vertexShaderIndex, shadow.pixelShader->name, shadow.pixelStaticIndex,
                dynamic.pixelShaderIndex, mesh.vertexFormat, mesh.vertexCount, mesh.primitives.size(), mesh.primitives.empty() ? 0 : mesh.primitives[0].second,
                viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height, c(4), c(5), c(6), c(7), c(8), c(9), c(10), c(11), c(58), c(59), c(60),
                t0 != nullptr ? t0->name : "none", t0 != nullptr ? t0->width : 0, t0 != nullptr ? t0->height : 0, dynamic.colorTargets[0],
                vertexShader->info.firstFloatConstant, vertexShader->info.floatConstantCount);
            if (mesh.vertexData != nullptr) {
                const float* p = reinterpret_cast<const float*>(mesh.vertexData);
                const int stride = mesh.streams[0].StrideInBytes / 4;
                log::Info("DIAG   v0 ({} {} {}) v1 ({} {} {}) v2 ({} {} {}) stride {} ps c0 {} c1 {} c2 {} c3 {} srv {} smp {} pso {}", p[0], p[1], p[2], p[stride],
                    p[stride + 1], p[stride + 2], p[2 * stride], p[2 * stride + 1], p[2 * stride + 2], stride * 4,
                    std::format("({} {} {} {})", dynamic.pixelFloats[0].v[0], dynamic.pixelFloats[0].v[1], dynamic.pixelFloats[0].v[2], dynamic.pixelFloats[0].v[3]),
                    std::format("({} {} {} {})", dynamic.pixelFloats[1].v[0], dynamic.pixelFloats[1].v[1], dynamic.pixelFloats[1].v[2], dynamic.pixelFloats[1].v[3]),
                    std::format("({} {} {} {})", dynamic.pixelFloats[2].v[0], dynamic.pixelFloats[2].v[1], dynamic.pixelFloats[2].v[2], dynamic.pixelFloats[2].v[3]),
                    std::format("({} {} {} {})", dynamic.pixelFloats[3].v[0], dynamic.pixelFloats[3].v[1], dynamic.pixelFloats[3].v[2], dynamic.pixelFloats[3].v[3]),
                    pixelState.textures[0], pixelState.samplers[0], static_cast<const void*>(pipeline));
            }
        }
    }
    if (viewport.Width <= 0 || viewport.Height <= 0)
        return;
    const float halfPixel[4] = { -1.0f / viewport.Width, 1.0f / viewport.Height, 0, 0 };
    for (StateBuffer* state : { &vertexState, &pixelState })
        std::memcpy(state->halfPixel, halfPixel, sizeof(halfPixel));
    std::memcpy(vertexState.ints, dynamic.vertexInts.data(), sizeof(vertexState.ints));
    std::memcpy(pixelState.ints, dynamic.pixelInts.data(), sizeof(pixelState.ints));
    for (int i = 0; i < kBoolConstants; ++i) {
        vertexState.bools[i] = dynamic.vertexBools[static_cast<size_t>(i)] != 0;
        pixelState.bools[i] = dynamic.pixelBools[static_cast<size_t>(i)] != 0;
    }
    pixelState.alpha[0] = static_cast<float>(shadow.alphaRef);

    ID3D12GraphicsCommandList* commands = backend.Commands();
    if (g_cache.boundGeneration != backend.ListGeneration()) {
        g_cache.boundGeneration = backend.ListGeneration();
        ID3D12DescriptorHeap* heaps[] = { backend.Descriptors().Heap(), backend.SamplerHeap() };
        commands->SetDescriptorHeaps(2, heaps);
        commands->SetGraphicsRootSignature(g_cache.rootSignature.Get());
        g_cache.boundPipeline = nullptr;
    }
    if (g_cache.boundPipeline != pipeline) {
        commands->SetPipelineState(pipeline);
        g_cache.boundPipeline = pipeline;
    }
    auto floats = [&](const CompiledShader& shader, const Float4* registers, UINT registerCount, D3D12_GPU_VIRTUAL_ADDRESS fallback) {
        const UINT first = std::min(shader.info.firstFloatConstant, registerCount);
        const UINT count = std::min(shader.info.floatConstantCount, registerCount - first);
        return count == 0 ? fallback : UploadConstants(registers + first, count * sizeof(Float4));
    };
    const D3D12_GPU_VIRTUAL_ADDRESS vertexStateAddress = UploadConstants(&vertexState, sizeof(vertexState));
    const D3D12_GPU_VIRTUAL_ADDRESS pixelStateAddress = UploadConstants(&pixelState, sizeof(pixelState));
    commands->SetGraphicsRootConstantBufferView(0, floats(*vertexShader, dynamic.vertexFloats.data(), kVertexFloatConstants, vertexStateAddress));
    commands->SetGraphicsRootConstantBufferView(1, vertexStateAddress);
    commands->SetGraphicsRootConstantBufferView(2, floats(*pixelShader, dynamic.pixelFloats.data(), kPixelFloatConstants, pixelStateAddress));
    commands->SetGraphicsRootConstantBufferView(3, pixelStateAddress);

    commands->RSSetViewports(1, &viewport);
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(targets.width), static_cast<LONG>(targets.height) };
    if (dynamic.scissorEnabled) {
        scissor.left = std::max(scissor.left, dynamic.scissor.left);
        scissor.top = std::max(scissor.top, dynamic.scissor.top);
        scissor.right = std::min(scissor.right, dynamic.scissor.right);
        scissor.bottom = std::min(scissor.bottom, dynamic.scissor.bottom);
    }
    commands->RSSetScissorRects(1, &scissor);
    commands->OMSetStencilRef(static_cast<UINT>(dynamic.stencilRef) & 0xff);

    D3D12_VERTEX_BUFFER_VIEW streams[4] = { mesh.streams[0], mesh.streams[1], mesh.streams[2], { g_cache.zeroBuffer->GetGPUVirtualAddress(), 16, 0 } };
    commands->IASetVertexBuffers(0, 4, streams);
    commands->IASetPrimitiveTopology(mesh.topology);
    if (mesh.indices.SizeInBytes != 0) {
        commands->IASetIndexBuffer(&mesh.indices);
        for (const auto& [first, count] : mesh.primitives) {
            if (count > 0)
                commands->DrawIndexedInstanced(static_cast<UINT>(count), 1, static_cast<UINT>(first), 0, 0);
        }
    } else {
        for (const auto& [first, count] : mesh.primitives) {
            if (count > 0)
                commands->DrawInstanced(static_cast<UINT>(count), 1, static_cast<UINT>(first), 0);
        }
    }
}

}
