#include "shaderapi_pipelines.h"

#include "busy_window.h"
#include "color.h"
#include "command_line.h"
#include "log.h"
#include "night_sky.h"
#include "settings.h"
#include "shaderapi_capture.h"
#include "shader_cache.h"
#include "shader_compiler.h"
#include "shader_precache.h"
#include "shader_translator.h"
#include "shaderapi.h"
#include "shaderapi_textures.h"
#include "vcs.h"

#include <d3d9types.h>

#include <g_gradePS.h>
#include <g_skyDrawPS.h>
#include <g_sunDiscPS.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
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
    // shader_precache::HashFile of the bytes.
    std::uint64_t hash = 0;
    vcs::Directory directory;
    // Decoded static combos: their dynamic combos' bytecode by dynamic index.
    std::unordered_map<std::uint32_t, std::vector<std::vector<DWORD>>> statics;
};

namespace {

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
// Bumped when the pipeline descriptions change in ways their names don't capture (the root signature).
constexpr std::uint32_t kPipelineLibraryVersion = 2;
constexpr auto kPipelineLibrarySaveInterval = std::chrono::seconds(30);
// Root parameters of the translated shaders: vertex b0/b1, pixel b0/b1, then the forward twins' frame constants and
// scene.
constexpr UINT kRootForwardFrame = 4;
// ShaderFogMode_t's SHADER_FOGMODE_FOGCOLOR: the pass blends the scene's fog colour (CommitLinearFogColor in shaderapi.cpp).
constexpr int kShaderFogColor = 4;
constexpr UINT kRootForwardScene = 5;
constexpr UINT kRootParameters = 6;

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
    // Names the shader in pipeline library names.
    std::uint64_t dxilHash = 0;
    bool failed = false;
};

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

// Keys are zero-filled before use, so their padding compares and hashes equal; semantic names point to static strings.
struct PipelineKeyHash {
    size_t operator()(const PipelineKey& key) const
    {
        // Unused input element slots are zero, so they are left out.
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&key);
        const size_t elementsEnd = offsetof(PipelineKey, elements) + key.elementCount * sizeof(D3D12_INPUT_ELEMENT_DESC);
        std::uint64_t hash = 14695981039346656037ull;
        auto mix = [&](size_t begin, size_t end) {
            for (size_t i = begin; i + 8 <= end; i += 8) {
                std::uint64_t word;
                std::memcpy(&word, bytes + i, sizeof(word));
                hash = (hash ^ word) * 1099511628211ull ^ (hash >> 31);
            }
        };
        mix(0, elementsEnd);
        mix(offsetof(PipelineKey, elementCount), sizeof(PipelineKey));
        return static_cast<size_t>(hash);
    }
};

struct PipelineKeyEqual {
    bool operator()(const PipelineKey& a, const PipelineKey& b) const { return std::memcmp(&a, &b, sizeof(PipelineKey)) == 0; }
};

// A combo's bytecode never moves once loaded, so its address and the variant find its translation without hashing it.
struct VariantSlot {
    const DWORD* code;
    std::uint64_t variant;
    bool operator==(const VariantSlot&) const = default;
};

struct VariantSlotHash {
    size_t operator()(const VariantSlot& slot) const { return std::hash<const void*>()(slot.code) ^ std::hash<std::uint64_t>()(slot.variant * 0x9e3779b97f4a7c15ull); }
};

// The last upload of a register file's constants, valid while the command list and the registers are unchanged.
struct ConstantUpload {
    std::uint64_t generation = ~0ull;
    std::uint64_t version = 0;
    UINT first = 0;
    UINT count = 0;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
};

struct PipelineCache {
    bool initialized = false;
    bool usable = false;
    // Compiles the copy shaders (StretchRect); translated shaders go through shader_precache::Build.
    ShaderCompiler compiler;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12Resource> zeroBuffer;
    std::array<UINT, kTextureTypes> nullViews = {};
    std::unordered_map<std::string, std::unique_ptr<ShaderFile>> files;
    std::unordered_set<std::string> missingFiles;
    std::unordered_map<const DWORD*, std::unique_ptr<CompiledShader>> baseInfo;
    std::unordered_map<std::uint64_t, std::unique_ptr<CompiledShader>> shaders;
    std::unordered_map<VariantSlot, CompiledShader*, VariantSlotHash> variants;
    std::unordered_map<PipelineKey, ComPtr<ID3D12PipelineState>, PipelineKeyHash, PipelineKeyEqual> pipelines;
    // Consecutive passes often share a pipeline state.
    PipelineKey lastKey = {};
    ID3D12PipelineState* lastPipeline = nullptr;
    ConstantUpload vertexConstants;
    ConstantUpload pixelConstants;
    // By format and sample count.
    std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> blitPipelines;
    std::vector<unsigned char> blitVertexShader;
    std::vector<unsigned char> blitPixelShader;
    // Colour grading (GradeView): its pipeline, and the copy of the back buffer it reads, with that copy's view.
    ComPtr<ID3D12PipelineState> gradePipeline;
    bool gradeFailed = false;
    TrackedResource gradeSource;
    UINT gradeSourceView = 0;
    // The pixel shaders the 2D sky's faces are drawn with in place of the game's (shaders/sky_draw.hlsl).
    CompiledShader skyShader;
    CompiledShader discShader;
    std::uint64_t boundGeneration = ~0ull;
    ID3D12PipelineState* boundPipeline = nullptr;
    // Shaders the render thread asked shader_precache::Build for, and the time it waited.
    size_t built = 0;
    double buildSeconds = 0;
    // Pipeline states saved across launches (ID3D12PipelineLibrary); the blob must outlive the library, which saves on
    // other threads (SavePipelineLibrary) also hold.
    ComPtr<ID3D12PipelineLibrary> library;
    std::shared_ptr<const std::vector<std::uint8_t>> libraryBlob;
    std::filesystem::path libraryPath;
    bool libraryDirty = false;
    std::chrono::steady_clock::time_point librarySaved;
};

PipelineCache g_cache;

// pipelines.bin as ReadPipelineLibraryInBackground read it, for OpenPipelineLibrary.
std::mutex g_libraryReadMutex;
std::optional<std::shared_future<std::vector<std::uint8_t>>> g_libraryRead;
// A save writing the library on another thread.
std::atomic<bool> g_librarySaving = false;

std::filesystem::path PipelineLibraryPath() { return ModDirectory() / L"cache" / L"pipelines.bin"; }

std::vector<std::uint8_t> ReadWholeFile(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> data;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = file ? static_cast<std::streamoff>(file.tellg()) : -1;
    if (size <= 0)
        return data;
    data.resize(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file)
        data.clear();
    return data;
}
int g_drawLog = 0;
int g_drawLimit = -1;
int g_frameDraws = 0;

bool CreateRootSignature()
{
    Backend& backend = GetBackend();
    D3D12_ROOT_PARAMETER1 parameters[kRootParameters] = {};
    for (UINT i = 0; i < 4; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[i].Descriptor.ShaderRegister = i % 2;
        parameters[i].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        parameters[i].ShaderVisibility = i < 2 ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Forward twins light themselves with the lighting's constants and scene (shaders/common.hlsli, register space 1).
    parameters[kRootForwardFrame].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[kRootForwardFrame].Descriptor = { 0, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE };
    parameters[kRootForwardFrame].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[kRootForwardScene].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[kRootForwardScene].Descriptor = { 0, 1, D3D12_ROOT_DESCRIPTOR_FLAG_NONE };
    parameters[kRootForwardScene].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    for (UINT i = 0; i < 2; ++i) {
        samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        const D3D12_TEXTURE_ADDRESS_MODE mode = i == 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = mode;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i == 0 ? 0 : 2;
        samplers[i].RegisterSpace = 1;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = kRootParameters;
    desc.Desc_1_1.pParameters = parameters;
    desc.Desc_1_1.NumStaticSamplers = 2;
    desc.Desc_1_1.pStaticSamplers = samplers;
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

// A library from another driver, adapter or runtime is replaced by an empty one.
void OpenPipelineLibrary(const std::filesystem::path& path)
{
    ComPtr<ID3D12Device1> device;
    D3D12_FEATURE_DATA_SHADER_CACHE support = {};
    if (FAILED(GetBackend().Device()->QueryInterface(IID_PPV_ARGS(&device)))
        || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &support, sizeof(support)))
        || !(support.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY)) {
        log::Info("Native: pipeline libraries are not supported; pipeline states are not saved");
        return;
    }
    g_cache.libraryPath = path;
    std::optional<std::shared_future<std::vector<std::uint8_t>>> read;
    {
        std::scoped_lock lock(g_libraryReadMutex);
        read = std::exchange(g_libraryRead, std::nullopt);
    }
    g_cache.libraryBlob = std::make_shared<const std::vector<std::uint8_t>>(read ? read->get() : ReadWholeFile(path));
    HRESULT hr = device->CreatePipelineLibrary(g_cache.libraryBlob->data(), g_cache.libraryBlob->size(), IID_PPV_ARGS(&g_cache.library));
    if (FAILED(hr) && !g_cache.libraryBlob->empty()) {
        log::Info("Native: the saved pipeline library could not be used ({:#010x}); starting a new one", static_cast<unsigned>(hr));
        g_cache.libraryBlob = std::make_shared<const std::vector<std::uint8_t>>();
        hr = device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&g_cache.library));
    }
    if (FAILED(hr)) {
        log::Info("Native: creating a pipeline library failed: {:#010x}", static_cast<unsigned>(hr));
        g_cache.library = nullptr;
        return;
    }
    g_cache.librarySaved = std::chrono::steady_clock::now();
}

bool Initialize()
{
    if (g_cache.initialized)
        return g_cache.usable;
    g_cache.initialized = true;
    std::string error;
    if (!g_cache.compiler.Load(ModDirectory(), error)) {
        log::Info("Native: the shader compiler is unavailable: {}", error);
        return false;
    }
    if (!CreateRootSignature() || !CreateZeroBuffer())
        return false;
    CreateNullViews();
    for (auto [shader, dxil] : { std::pair{ &g_cache.skyShader, std::span<const unsigned char>(g_skyDrawPS) },
             std::pair{ &g_cache.discShader, std::span<const unsigned char>(g_sunDiscPS) } }) {
        shader->dxil.assign(dxil.begin(), dxil.end());
        shader->dxilHash = Fnv(dxil.data(), dxil.size());
        shader->info.stage = translator::Stage::Pixel;
        shader->info.colorOutputs = 1;
    }
    OpenPipelineLibrary(PipelineLibraryPath());
    g_cache.usable = true;
    return true;
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

const CompiledShader* CompiledByHash(std::span<const DWORD> code, const translator::Variant& variant, const char* fileName);

const CompiledShader* Compiled(std::span<const DWORD> code, const translator::Variant& variant, const char* fileName)
{
    static_assert(sizeof(translator::Variant) == sizeof(std::uint64_t));
    VariantSlot slot = { code.data(), 0 };
    std::memcpy(&slot.variant, &variant, sizeof(slot.variant));
    auto found = g_cache.variants.find(slot);
    if (found == g_cache.variants.end())
        found = g_cache.variants.emplace(slot, const_cast<CompiledShader*>(CompiledByHash(code, variant, fileName))).first;
    return found->second;
}

const CompiledShader* CompiledByHash(std::span<const DWORD> code, const translator::Variant& variant, const char* fileName)
{
    const std::uint64_t key = shader_cache::Key(code, variant);
    auto found = g_cache.shaders.find(key);
    if (found != g_cache.shaders.end())
        return found->second->failed ? nullptr : found->second.get();
    auto shader = std::make_unique<CompiledShader>();
    // Usually a read of what precaching compiled; otherwise the draw waits for the compile.
    const auto start = std::chrono::steady_clock::now();
    const busy::Scope busy(busy::Reason::Shaders);
    shader->failed = !shader_precache::Build(code, variant, fileName, shader->info, shader->dxil);
    g_cache.buildSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (++g_cache.built % 1000 == 0)
        log::Info("Native: {} shaders loaded or compiled on the render thread ({:.1f} s)", g_cache.built, g_cache.buildSeconds);
    shader->dxilHash = Fnv(shader->dxil.data(), shader->dxil.size());
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

// A pipeline's name in the library: its key with the shaders identified by content and the semantic names spelled out,
// so the same pipeline has the same name in every launch.
std::wstring PipelineName(const PipelineKey& key)
{
    PipelineKey stable;
    std::memcpy(&stable, &key, sizeof(stable));
    stable.vertexShader = nullptr;
    stable.pixelShader = nullptr;
    std::uint64_t hash = Fnv(&kPipelineLibraryVersion, sizeof(kPipelineLibraryVersion));
    for (UINT i = 0; i < stable.elementCount; ++i) {
        hash = Fnv(stable.elements[i].SemanticName, std::strlen(stable.elements[i].SemanticName), hash);
        stable.elements[i].SemanticName = nullptr;
    }
    hash = Fnv(&stable, sizeof(stable), hash);
    const std::uint64_t shaders[2] = { key.vertexShader->dxilHash, key.pixelShader != nullptr ? key.pixelShader->dxilHash : 0 };
    return std::format(L"{:016x}", Fnv(shaders, sizeof(shaders), hash));
}

// Everything a pipeline state depends on, zero-filled so its bytes can be hashed.
ID3D12PipelineState* Pipeline(PipelineKey& key)
{
    if (g_cache.lastPipeline != nullptr && PipelineKeyEqual()(key, g_cache.lastKey))
        return g_cache.lastPipeline;
    auto found = g_cache.pipelines.find(key);
    if (found != g_cache.pipelines.end()) {
        g_cache.lastKey = key;
        g_cache.lastPipeline = found->second.Get();
        return g_cache.lastPipeline;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = g_cache.rootSignature.Get();
    desc.VS = { key.vertexShader->dxil.data(), key.vertexShader->dxil.size() };
    if (key.pixelShader != nullptr)
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
    const std::wstring name = g_cache.library != nullptr ? PipelineName(key) : std::wstring();
    HRESULT hr = g_cache.library != nullptr ? g_cache.library->LoadGraphicsPipeline(name.c_str(), &desc, IID_PPV_ARGS(&pipeline)) : E_FAIL;
    if (FAILED(hr)) {
        // The driver compiles the shaders for the GPU.
        const busy::Scope busy(busy::Reason::Shaders);
        hr = GetBackend().Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr))
            log::Info("Native: creating a pipeline state failed: {:#010x}", static_cast<unsigned>(hr));
        else if (g_cache.library != nullptr && SUCCEEDED(g_cache.library->StorePipeline(name.c_str(), pipeline.Get())))
            g_cache.libraryDirty = true;
    }
    ID3D12PipelineState* result = pipeline.Get();
    g_cache.pipelines.emplace(key, std::move(pipeline));
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

// A texture's handle is its address (TextureFromHandle), so a live texture is bound as a target exactly when a target
// handle equals it; comparing handles spares a locked lookup per target for every sampler of every draw.
bool IsTarget(const Texture* texture)
{
    if (texture == nullptr)
        return false;
    const auto handle = reinterpret_cast<std::intptr_t>(texture);
    const DynamicState& state = Dynamic();
    return std::ranges::find(state.colorTargets, handle) != state.colorTargets.end() || state.depthTarget == handle;
}

// The slot of a G-buffer twin's light warp texture (capture::LightWarpSlot), or 0 without one. The twin itself does not
// sample it, so its view comes from the bound texture.
float LightWarpSlot(std::intptr_t handle, bool srgbRead)
{
    Texture* texture = TextureFromHandle(handle);
    if (texture == nullptr || texture->copies.empty() || IsTarget(texture))
        return 0.0f;
    Texture::Copy& copy = texture->Current();
    GetBackend().Transition(copy.resource, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    return capture::LightWarpSlot(srgbRead && copy.srgbView != DescriptorHeap::kInvalid ? copy.srgbView : copy.view);
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

bool CompileBlitShaders()
{
    if (!g_cache.blitVertexShader.empty())
        return true;
    std::string error;
    if (!g_cache.compiler.Compile(kBlitVertexShader, L"vs_6_6", g_cache.blitVertexShader, error)
        || !g_cache.compiler.Compile(kBlitPixelShader, L"ps_6_6", g_cache.blitPixelShader, error)) {
        log::Info("Native: compiling the copy shaders failed: {}", error);
        g_cache.blitVertexShader.clear();
        return false;
    }
    return true;
}

// A pipeline that draws one triangle over the viewport with `pixelShader` into one target of `format`.
ComPtr<ID3D12PipelineState> CreateFullscreenPipeline(std::span<const unsigned char> pixelShader, DXGI_FORMAT format)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = g_cache.rootSignature.Get();
    desc.VS = { g_cache.blitVertexShader.data(), g_cache.blitVertexShader.size() };
    desc.PS = { pixelShader.data(), pixelShader.size() };
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
        log::Info("Native: creating a full-screen pipeline for format {} failed: {:#010x}", static_cast<int>(format), static_cast<unsigned>(hr));
    return pipeline;
}

ID3D12PipelineState* BlitPipeline(DXGI_FORMAT format)
{
    const std::uint64_t key = static_cast<std::uint64_t>(format);
    auto found = g_cache.blitPipelines.find(key);
    if (found != g_cache.blitPipelines.end())
        return found->second.Get();
    if (!CompileBlitShaders())
        return nullptr;
    return g_cache.blitPipelines.emplace(key, CreateFullscreenPipeline(g_cache.blitPixelShader, format)).first->second.Get();
}

// Binds the shader API's root signature and heaps when this command list has not had them yet.
void BindRoot(Backend& backend, ID3D12GraphicsCommandList* commands)
{
    if (g_cache.boundGeneration == backend.ListGeneration())
        return;
    g_cache.boundGeneration = backend.ListGeneration();
    ID3D12DescriptorHeap* heaps[] = { backend.Descriptors().Heap(), backend.SamplerHeap() };
    commands->SetDescriptorHeaps(2, heaps);
    commands->SetGraphicsRootSignature(g_cache.rootSignature.Get());
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
    BindRoot(backend, commands);
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

namespace {

// Mirrors GradeConstants in shaders/grade.hlsl.
struct GradeConstants {
    float gain[3];
    float contrast;
    float saturation;
    std::uint32_t source;
    std::uint32_t padding[2];
};

float Luminance(const std::array<float, 3>& color) { return 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2]; }

// The per-channel gain of exposure and white balance: warmth recolours the view as if lit by a black body of
// 6500 K x 2^-warmth rather than 6500 K (the sRGB white), tint shifts it between green and magenta; neither changes
// its luminance.
std::array<float, 3> GradeGain(const Settings& settings)
{
    constexpr float kNeutralKelvin = 6500.0f;
    constexpr float kTintStrength = 0.2f;
    const std::array<float, 3> neutral = BlackbodyColor(kNeutralKelvin);
    const std::array<float, 3> light = BlackbodyColor(kNeutralKelvin * std::exp2(-settings.warmth));
    std::array<float, 3> balance;
    for (size_t i = 0; i < 3; ++i)
        balance[i] = light[i] / std::max(neutral[i], 1e-6f);
    const float tint = settings.tint * kTintStrength;
    const std::array<float, 3> shift = { 1.0f + tint, 1.0f - tint, 1.0f + tint };
    for (size_t i = 0; i < 3; ++i)
        balance[i] *= shift[i];
    const float scale = std::exp2(settings.exposure) / std::max(Luminance(balance), 1e-6f);
    for (float& gain : balance)
        gain *= scale;
    return balance;
}

bool GradingChanges(const Settings& settings)
{
    return settings.grading && (settings.exposure != 0.0f || settings.contrast != 1.0f || settings.saturation != 1.0f || settings.warmth != 0.0f
        || settings.tint != 0.0f);
}

// The copy of the back buffer GradeView reads, made again when the back buffer's size or format changes.
bool PrepareGradeSource(const D3D12_RESOURCE_DESC& backBuffer)
{
    Backend& backend = GetBackend();
    if (g_cache.gradeSource.resource) {
        const D3D12_RESOURCE_DESC desc = g_cache.gradeSource.resource->GetDesc();
        if (desc.Width == backBuffer.Width && desc.Height == backBuffer.Height && desc.Format == backBuffer.Format)
            return true;
        backend.Retire(std::move(g_cache.gradeSource.resource));
        backend.RetireDescriptor(g_cache.gradeSourceView);
        g_cache.gradeSource = {};
    }
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = backBuffer.Width;
    desc.Height = backBuffer.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = backBuffer.Format;
    desc.SampleDesc.Count = 1;
    const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    const HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&g_cache.gradeSource.resource));
    if (FAILED(hr)) {
        log::Info("Native: creating the colour grading copy failed: {:#010x}", static_cast<unsigned>(hr));
        return false;
    }
    g_cache.gradeSource.state = D3D12_RESOURCE_STATE_COMMON;
    g_cache.gradeSourceView = backend.Descriptors().Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = Backend::kBackBufferFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    backend.Device()->CreateShaderResourceView(g_cache.gradeSource.resource.Get(), &view, backend.Descriptors().Cpu(g_cache.gradeSourceView));
    return true;
}

}

void GradeView(D3D12_RECT rect)
{
    const Settings& settings = GetSettings();
    if (!GradingChanges(settings) || g_cache.gradeFailed)
        return;
    if (Dynamic().colorTargets[0] != kDefaultTarget) {
        static bool logged = false;
        if (!std::exchange(logged, true))
            log::Info("Native: colour grading skipped a view drawn outside the back buffer");
        return;
    }
    if (!Initialize() || !CompileBlitShaders())
        return;
    if (!g_cache.gradePipeline) {
        g_cache.gradePipeline = CreateFullscreenPipeline(g_gradePS, Backend::kBackBufferFormat);
        if (!g_cache.gradePipeline) {
            g_cache.gradeFailed = true;
            return;
        }
    }
    Backend& backend = GetBackend();
    TrackedResource& backBuffer = backend.BackBuffer();
    const D3D12_RESOURCE_DESC desc = backBuffer.resource->GetDesc();
    rect.left = std::max(rect.left, LONG(0));
    rect.top = std::max(rect.top, LONG(0));
    rect.right = std::min(rect.right, static_cast<LONG>(desc.Width));
    rect.bottom = std::min(rect.bottom, static_cast<LONG>(desc.Height));
    if (rect.right <= rect.left || rect.bottom <= rect.top || !PrepareGradeSource(desc))
        return;

    ID3D12GraphicsCommandList* commands = backend.Commands();
    backend.Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
    backend.Transition(g_cache.gradeSource, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to = {};
    to.pResource = g_cache.gradeSource.resource.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = backBuffer.resource.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box = { static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0, static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1 };
    commands->CopyTextureRegion(&to, static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0, &from, &box);
    backend.Transition(g_cache.gradeSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    backend.Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);

    BindRoot(backend, commands);
    commands->SetPipelineState(g_cache.gradePipeline.Get());
    g_cache.boundPipeline = g_cache.gradePipeline.Get();
    const std::array<float, 3> gain = GradeGain(settings);
    const GradeConstants constants = { { gain[0], gain[1], gain[2] }, settings.contrast, settings.saturation, g_cache.gradeSourceView, {} };
    commands->SetGraphicsRootConstantBufferView(2, UploadConstants(&constants, sizeof(constants)));
    const D3D12_CPU_DESCRIPTOR_HANDLE target = backend.BackBufferView(false);
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = { static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right - rect.left),
        static_cast<float>(rect.bottom - rect.top), 0, 1 };
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &rect);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
}

namespace {

// Writes the library's contents to disk, beside the file and renamed into place. The library synchronizes itself
// (ID3D12Device1::CreatePipelineLibrary, Thread Safety), so this runs on any thread.
void WritePipelineLibrary(ID3D12PipelineLibrary* library, const std::filesystem::path& path)
{
    // The render thread may store a pipeline between the size and the copy, which then no longer fits: measured again.
    std::vector<std::uint8_t> data;
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 3 && FAILED(hr); ++attempt) {
        data.resize(library->GetSerializedSize());
        hr = library->Serialize(data.data(), data.size());
    }
    if (FAILED(hr)) {
        log::Info("Native: saving the pipeline library failed: {:#010x}", static_cast<unsigned>(hr));
        return;
    }
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file) {
            log::Info("Native: writing {} failed", temporary.string());
            return;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error)
        log::Info("Native: replacing {} failed: {}", path.string(), error.message());
}

}

void ReadPipelineLibraryInBackground()
{
    std::scoped_lock lock(g_libraryReadMutex);
    if (g_libraryRead || g_cache.initialized)
        return;
    // A promise's future does not wait for the thread when destroyed; the thread is detached and the DLL pinned.
    auto promise = std::make_shared<std::promise<std::vector<std::uint8_t>>>();
    g_libraryRead = promise->get_future().share();
    PinThisModule();
    std::thread([promise, path = PipelineLibraryPath()] { promise->set_value(ReadWholeFile(path)); }).detach();
}

void SavePipelineLibrary(bool now)
{
    // At shutdown a save still writing finishes first, whether or not anything is left to save after it.
    while (now && g_librarySaving)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (g_cache.library == nullptr || !g_cache.libraryDirty)
        return;
    const auto time = std::chrono::steady_clock::now();
    if (!now && (time - g_cache.librarySaved < kPipelineLibrarySaveInterval || g_librarySaving))
        return;
    g_cache.librarySaved = time;
    g_cache.libraryDirty = false;
    if (now) {
        WritePipelineLibrary(g_cache.library.Get(), g_cache.libraryPath);
        return;
    }
    // Serializing a large library and writing it took the render thread long enough to hitch the game every 30 s while
    // new pipeline states were being made. The thread holds the library and its blob for as long as it runs.
    g_librarySaving = true;
    PinThisModule();
    std::thread([library = g_cache.library, blob = g_cache.libraryBlob, path = g_cache.libraryPath] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        WritePipelineLibrary(library.Get(), path);
        g_librarySaving = false;
    }).detach();
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
    file->hash = shader_precache::HashFile(file->bytes);
    return g_cache.files.emplace(key, std::move(file)).first->second.get();
}

void PrecacheShaders(const ShadowState& state)
{
    // Twins are drawn only by real-time lighting (shaderapi_capture.cpp's Prepare).
    const bool twins = GetSettings().realtimeLighting && GetStatistics().realtimeBlock != RealtimeBlock::Unsupported;
    for (const ShaderFile* file : { state.vertexShader, state.pixelShader }) {
        if (file == nullptr)
            continue;
        shader_precache::Request request;
        request.name = file->name.c_str();
        request.file = file->bytes;
        request.fileHash = file->hash;
        request.directory = &file->directory;
        request.staticIndex = file == state.vertexShader ? state.vertexStaticIndex : state.pixelStaticIndex;
        request.vertex = file == state.vertexShader;
        if (file == state.pixelShader) {
            // As DrawPass and capture::Prepare choose them: captured passes with a depth test draw G-buffer twins,
            // translucent passes after the capture draw forward twins, and so does every pass of a view drawn into a
            // texture when such views are lit ("Cameras and mirrors"); opaque passes writing depth and colour with the
            // game's own shader clear the coverage.
            request.alphaTest = state.alphaTest ? static_cast<std::uint8_t>(state.alphaFunc + 1) : 0;
            request.gbufferTwin = twins && state.depthTest;
            request.forwardTwin = twins && (state.blend || GetSettings().renderTargetLighting);
            request.clearCoverage = twins && state.depthTest && !state.blend && state.depthWrite && state.colorWrite;
            request.opacityMicromaps = GetStatistics().opacityMicromapsAvailable;
        }
        shader_precache::Enqueue(request);
    }
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
    Texture* depth = state.depthTarget == kDefaultTarget ? nullptr : TextureFromHandle(state.depthTarget);
    if (state.depthTarget == kDefaultTarget) {
        backend.Transition(backend.DepthBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        targets.depth = backend.DepthBufferView();
        targets.depthFormat = Backend::kDepthFormat;
    } else if (depth != nullptr && depth->IsDepth() && !depth->copies.empty()) {
        backend.Transition(depth->Current().resource, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        targets.depth = depth->Current().target;
        targets.depthFormat = depth->TargetFormat(false);
    }
    backend.Commands()->OMSetRenderTargets(targets.count, targets.colors, FALSE, targets.depth.ptr != 0 ? &targets.depth : nullptr);
    return targets;
}

namespace {

// Vertex c12 and c13: the model x view x projection's and view x projection's z rows (CommitVertexShaderTransforms in
// shaderapi.cpp).
constexpr UINT kModelViewProjectionZ = 12;
constexpr UINT kViewProjectionZ = 13;

// The shader's float registers, with the patches applied, or the last upload of them while they are unchanged.
D3D12_GPU_VIRTUAL_ADDRESS FloatConstants(const CompiledShader& shader, const Float4* registers, UINT registerCount, std::uint64_t version, ConstantUpload& last,
    D3D12_GPU_VIRTUAL_ADDRESS fallback, std::span<const capture::RegisterPatch> patches)
{
    Backend& backend = GetBackend();
    const UINT first = std::min(shader.info.firstFloatConstant, registerCount);
    const UINT count = std::min(shader.info.floatConstantCount, registerCount - first);
    if (count == 0)
        return fallback;
    if (!patches.empty()) {
        Backend::Upload upload = backend.AllocateUpload(count * sizeof(Float4), kConstantAlignment);
        if (upload.data == nullptr)
            return fallback;
        auto* copy = reinterpret_cast<Float4*>(upload.data);
        std::memcpy(copy, registers + first, count * sizeof(Float4));
        for (const capture::RegisterPatch& patch : patches) {
            if (patch.reg >= first && patch.reg < first + count)
                std::copy(patch.value.begin(), patch.value.end(), copy[patch.reg - first].v);
        }
        last.generation = ~0ull;
        return upload.buffer->GetGPUVirtualAddress() + upload.offset;
    }
    if (last.generation != backend.ListGeneration() || last.version != version || last.first != first || last.count < count)
        last = { backend.ListGeneration(), version, first, count, UploadConstants(registers + first, count * sizeof(Float4)) };
    return last.address;
}

void IssueDraws(ID3D12GraphicsCommandList* commands, const MeshDraw& mesh)
{
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

// SkyDrawConstants of shaders/sky_draw.hlsl.
struct SkyConstants {
    std::array<Float4, 4> clipToEye;
    float viewport[4];
    float tonemapScale;
    std::uint32_t skyView;
    std::uint32_t target;
    std::uint32_t padding;
    NightSkyConstants night;
};
static_assert(sizeof(SkyConstants) == 496);

// SkyDrawConstants::target: how a colour target stores colour.
std::uint32_t TargetEncoding(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM: return 1;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 2;
    default: return 0;
    }
}

// The sky shaders' constants for the 2D sky face being drawn into `format`, which they place by the view-projection
// (c8-c11) as the draw's vertex patches leave it; 0 when it cannot be inverted.
// The inverse of a draw's view-projection rows that maps clip space to points relative to the eye, in double precision.
// Far-plane points mapped to the world in single precision moved by up to a few hundredths of a degree from frame to
// frame with the jittered matrix, which the stars and Moon showed when zoomed in; relative to the eye, a rounding of w
// only scales a point along its direction.
bool ClipToEye(const std::array<Float4, 4>& rows, std::array<Float4, 4>& clipToEye)
{
    double m[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[r][c] = rows[static_cast<size_t>(r)].v[c];
            m[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int r = column + 1; r < 4; ++r) {
            if (std::fabs(m[r][column]) > std::fabs(m[pivot][column]))
                pivot = r;
        }
        if (std::fabs(m[pivot][column]) < 1e-300)
            return false;
        std::swap(m[pivot], m[column]);
        const double scale = 1.0 / m[column][column];
        for (double& value : m[column])
            value *= scale;
        for (int r = 0; r < 4; ++r) {
            if (r == column)
                continue;
            const double factor = m[r][column];
            for (int c = 0; c < 8; ++c)
                m[r][c] -= factor * m[column][c];
        }
    }
    // The eye is the point the projection sends to w = 0: the inverse maps clip-space (0, 0, 1, 0) to it.
    if (std::fabs(m[3][6]) < 1e-300)
        return false;
    for (int r = 0; r < 4; ++r) {
        const double eye = r < 3 ? m[r][6] / m[3][6] : 0.0;
        for (int c = 0; c < 4; ++c)
            clipToEye[static_cast<size_t>(r)].v[c] = static_cast<float>(m[r][c + 4] - eye * m[3][c + 4]);
    }
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS SkyDrawConstants(const gbuffer::SkyDraw& sky, std::span<const capture::RegisterPatch> vertexPatches, DXGI_FORMAT format)
{
    const DynamicState& dynamic = Dynamic();
    std::array<Float4, 4> viewProjection;
    for (UINT r = 0; r < 4; ++r)
        viewProjection[r] = dynamic.vertexFloats[gbuffer::kViewProjection + r];
    for (const capture::RegisterPatch& patch : vertexPatches) {
        if (patch.reg >= gbuffer::kViewProjection && patch.reg < gbuffer::kViewProjection + 4)
            std::copy(patch.value.begin(), patch.value.end(), viewProjection[patch.reg - gbuffer::kViewProjection].v);
    }
    SkyConstants constants = {};
    if (!ClipToEye(viewProjection, constants.clipToEye))
        return 0;
    const D3D12_VIEWPORT& viewport = dynamic.viewport;
    const float rect[4] = { viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height };
    std::copy(std::begin(rect), std::end(rect), constants.viewport);
    constants.tonemapScale = dynamic.pixelFloats[gbuffer::kLightScaleRegister].v[0];
    constants.skyView = sky.skyView;
    constants.target = TargetEncoding(format);
    constants.night = PackNightSky(sky);
    return UploadConstants(&constants, sizeof(constants));
}

// Adds the sun's disc over a 2D sky face the game drew, through the same geometry and depth test.
void DrawSunDisc(PipelineKey key, D3D12_GPU_VIRTUAL_ADDRESS constants, const MeshDraw& mesh)
{
    key.pixelShader = &g_cache.discShader;
    D3D12_RENDER_TARGET_BLEND_DESC& blend = key.blend.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ONE;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask &= D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    for (size_t i = 1; i < std::size(key.blend.RenderTarget); ++i)
        key.blend.RenderTarget[i].RenderTargetWriteMask = 0;
    // The face may have written its depth, which the disc then meets exactly.
    key.depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    if (key.depthStencil.DepthFunc == D3D12_COMPARISON_FUNC_LESS)
        key.depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    ID3D12PipelineState* pipeline = Pipeline(key);
    if (pipeline == nullptr)
        return;
    ID3D12GraphicsCommandList* commands = GetBackend().Commands();
    if (g_cache.boundPipeline != pipeline) {
        commands->SetPipelineState(pipeline);
        g_cache.boundPipeline = pipeline;
    }
    commands->SetGraphicsRootConstantBufferView(kRootForwardFrame, constants);
    IssueDraws(commands, mesh);
}

// Draws a 2D skybox pass again into each face of the capture's sky cube, with the game's shaders and blending, so
// lighting sees the whole sky as the game draws it. The tonemap scale (pixel c30.x, which Source's shaders multiply
// their output by) is left out, so the cube holds the sky's radiance.
void DrawSkyFaces(PipelineKey key, const CompiledShader& vertexShader, const CompiledShader& pixelShader, StateBuffer vertexState, StateBuffer pixelState,
    const MeshDraw& mesh)
{
    const std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> faces = capture::SkyFaces();
    if (faces.empty())
        return;
    key.targets = 1;
    std::fill(std::begin(key.targetFormats), std::end(key.targetFormats), DXGI_FORMAT_UNKNOWN);
    key.targetFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    key.depthFormat = DXGI_FORMAT_UNKNOWN;
    key.depthStencil.DepthEnable = FALSE;
    key.depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    key.depthStencil.StencilEnable = FALSE;
    key.depthStencil.StencilReadMask = key.depthStencil.StencilWriteMask = 0;
    key.depthStencil.FrontFace = key.depthStencil.BackFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
    key.raster.CullMode = D3D12_CULL_MODE_NONE;
    key.raster.DepthBias = 0;
    key.raster.SlopeScaledDepthBias = 0.0f;
    key.blend.IndependentBlendEnable = FALSE;
    for (size_t i = 1; i < std::size(key.blend.RenderTarget); ++i)
        key.blend.RenderTarget[i] = {};
    key.blend.RenderTarget[0].RenderTargetWriteMask &= D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    ID3D12PipelineState* pipeline = Pipeline(key);
    if (pipeline == nullptr)
        return;

    Backend& backend = GetBackend();
    const DynamicState& dynamic = Dynamic();
    ID3D12GraphicsCommandList* commands = backend.Commands();
    if (g_cache.boundPipeline != pipeline) {
        commands->SetPipelineState(pipeline);
        g_cache.boundPipeline = pipeline;
    }
    constexpr float kSize = static_cast<float>(capture::kSkyCubeSize);
    const float halfPixel[4] = { 1.0f / kSize, -1.0f / kSize, 0, 0 };
    // User clip planes are in the game's clip space; (0, 0, 0, 1) keeps everything in front of the eye.
    for (float (&plane)[4] : vertexState.clipPlanes) {
        plane[0] = plane[1] = plane[2] = 0.0f;
        plane[3] = 1.0f;
    }
    for (StateBuffer* state : { &vertexState, &pixelState })
        std::memcpy(state->halfPixel, halfPixel, sizeof(halfPixel));
    const D3D12_GPU_VIRTUAL_ADDRESS vertexStateAddress = UploadConstants(&vertexState, sizeof(vertexState));
    const D3D12_GPU_VIRTUAL_ADDRESS pixelStateAddress = UploadConstants(&pixelState, sizeof(pixelState));
    const float* lightScale = dynamic.pixelFloats[gbuffer::kLightScaleRegister].v;
    const capture::RegisterPatch unscaled = { gbuffer::kLightScaleRegister, { 1.0f, lightScale[1], lightScale[2], 1.0f } };
    commands->SetGraphicsRootConstantBufferView(1, vertexStateAddress);
    commands->SetGraphicsRootConstantBufferView(2, FloatConstants(pixelShader, dynamic.pixelFloats.data(), kPixelFloatConstants, dynamic.pixelFloatsVersion,
        g_cache.pixelConstants, pixelStateAddress, std::span(&unscaled, 1)));
    commands->SetGraphicsRootConstantBufferView(3, pixelStateAddress);
    const D3D12_VIEWPORT viewport = { 0, 0, kSize, kSize, 0, 1 };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(capture::kSkyCubeSize), static_cast<LONG>(capture::kSkyCubeSize) };
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    for (int face = 0; face < 6; ++face) {
        // Source's sky shaders place the faces with c4-c7 (sky_vs20) or c8-c11 (GMod's g_sky_vs20).
        const CubeFace transform = CubeFaceTransform(face);
        std::array<capture::RegisterPatch, 10> patches;
        for (UINT r = 0; r < 4; ++r) {
            patches[r] = { gbuffer::kModelViewProjection + r, std::to_array(transform.modelViewProjection[r].v) };
            patches[4 + r] = { gbuffer::kViewProjection + r, std::to_array(transform.viewProjection[r].v) };
        }
        patches[8] = { kModelViewProjectionZ, std::to_array(transform.modelViewProjection[2].v) };
        patches[9] = { kViewProjectionZ, std::to_array(transform.viewProjection[2].v) };
        commands->SetGraphicsRootConstantBufferView(0, FloatConstants(vertexShader, dynamic.vertexFloats.data(), kVertexFloatConstants, dynamic.vertexFloatsVersion,
            g_cache.vertexConstants, vertexStateAddress, patches));
        commands->OMSetRenderTargets(1, &faces[static_cast<size_t>(face)], FALSE, nullptr);
        IssueDraws(commands, mesh);
    }
}

}

void DrawPass(const ShadowState& shadow, const MeshDraw& mesh)
{
    if (!Initialize())
        return;
    if (g_drawLimit >= 0 && g_frameDraws++ >= g_drawLimit)
        return;
    // env_sun's sprite gives way to the sun's disc drawn with the 2D sky.
    if (capture::IsSunSprite(mesh))
        return;
    Backend& backend = GetBackend();
    const DynamicState& dynamic = Dynamic();
    const bool colorWrite = dynamic.overrideColorWrite ? dynamic.colorWriteEnable : shadow.colorWrite != 0;
    const bool alphaWrite = dynamic.overrideAlphaWrite ? dynamic.alphaWriteEnable : shadow.alphaWrite != 0;
    // A vertex shader without a pixel shader that writes no colour (engine/occlusionproxy, for occlusion queries) only
    // needs depth, so it runs without a pixel shader; one that writes colour needs Direct3D 9's fixed-function pixel stage.
    const bool depthOnly = shadow.vertexShader != nullptr && shadow.pixelShader == nullptr && !colorWrite && !alphaWrite;
    if (shadow.vertexShader == nullptr || (shadow.pixelShader == nullptr && !depthOnly)) {
        static std::set<std::string> logged;
        if (logged.size() < 50 && logged.insert(BoundMaterialName()).second)
            log::Info("Native: a pass of '{}' without a vertex or pixel shader was skipped; fixed-function rendering is not implemented", BoundMaterialName());
        return;
    }
    const std::span<const DWORD> vertexCode = Combo(*shadow.vertexShader, shadow.vertexStaticIndex, std::max(dynamic.vertexShaderIndex, 0));
    const std::span<const DWORD> pixelCode = depthOnly ? std::span<const DWORD>() : Combo(*shadow.pixelShader, shadow.pixelStaticIndex, dynamic.pixelShaderIndex);
    if (vertexCode.empty() || (pixelCode.empty() && !depthOnly)) {
        static bool logged = false;
        if (!std::exchange(logged, true))
            log::Info("Native: {} static {} or {} static {} has no code for its dynamic combo", shadow.vertexShader->name, shadow.vertexStaticIndex,
                depthOnly ? "none" : shadow.pixelShader->name, shadow.pixelStaticIndex);
        return;
    }
    const DynamicState::BlendOverride& colorOverride = dynamic.blendOverride;
    const DynamicState::BlendOverride& alphaOverride = dynamic.alphaBlendOverride;
    const int source = colorOverride.active ? colorOverride.source : shadow.srcBlend;
    const int destination = colorOverride.active ? colorOverride.destination : shadow.dstBlend;
    const int op = colorOverride.active ? colorOverride.op : shadow.blendOp;
    const bool blendEnabled = colorOverride.active ? colorOverride.enable : shadow.blend != 0;

    // During a G-buffer capture or the forward lighting after it, the pass may draw other code into other targets.
    const capture::Pass pass =
        capture::Prepare(vertexCode, pixelCode, depthOnly ? nullptr : shadow.pixelShader->name.c_str(),
            depthOnly ? capture::ComboId() : capture::ComboId{ &shadow.pixelShader->directory.header, shadow.pixelStaticIndex, dynamic.pixelShaderIndex },
            shadow.depthTest != 0, blendEnabled, source, destination, shadow.srgbWrite != 0, static_cast<std::uint32_t>(shadow.srgbRead));
    if (pass.route == capture::Pass::Route::Skip)
        return;
    if (pass.waterSurface)
        capture::RecordWaterMapping(mesh);
    const std::span<const DWORD> drawnPixelCode = pass.pixelCode.empty() ? pixelCode : pass.pixelCode;
    const char* pixelName = !pass.pixelCode.empty() ? pass.pixelName : depthOnly ? "none" : shadow.pixelShader->name.c_str();

    const CompiledShader* vertexBase = BaseInfo(vertexCode);
    const CompiledShader* pixelBase = depthOnly ? nullptr : BaseInfo(drawnPixelCode);
    if (vertexBase == nullptr || (pixelBase == nullptr && !depthOnly))
        return;

    Targets targets = pass.route == capture::Pass::Route::Game ? PrepareTargets(shadow.srgbWrite != 0) : capture::BindTargets(pass, shadow.srgbWrite != 0);
    // The 2D sky's faces show the procedural sky, or get the sun's disc over the game's sky (shaders/sky_draw.hlsl).
    const gbuffer::SkyDraw* sky = pass.route == capture::Pass::Route::Game && !depthOnly && targets.count == 1 && capture::SkyMaterial() ? capture::Sky() : nullptr;
    const D3D12_GPU_VIRTUAL_ADDRESS skyConstants = sky != nullptr ? SkyDrawConstants(*sky, pass.vertexPatches, targets.colorFormats[0]) : 0;
    const bool proceduralSky = skyConstants != 0 && sky->sky;
    const bool depthWrite = dynamic.overrideDepthEnable ? dynamic.depthEnable : shadow.depthWrite != 0;
    // The sky lies behind everything, so no surface's coverage is left where it draws.
    const bool clearCoverage = pass.clearsCoverage && !proceduralSky && depthWrite && colorWrite && pixelBase != nullptr &&
        pixelBase->info.colorOutputs == 1 && targets.count == 1 && targets.depth.ptr != 0;
    if (clearCoverage)
        capture::AddCoverageTarget(targets);

    StateBuffer pixelState = {};
    translator::Variant pixelVariant;
    if (pixelBase != nullptr)
        BindSamplers(*pixelBase, dynamic.textures.data(), kSamplers, shadow.srgbRead, pixelState, &pixelVariant.shadowSamplers);
    if (pass.probes) {
        D3D12_SAMPLER_DESC probeSampler = {};
        probeSampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        probeSampler.AddressU = probeSampler.AddressV = probeSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        probeSampler.MaxLOD = D3D12_FLOAT32_MAX;
        for (size_t i = 0; i < pass.probeSamplers.size(); ++i) {
            const UINT s = pass.probeSamplers[i];
            if (s >= static_cast<UINT>(kSamplers))
                continue;
            pixelState.textures[s] = capture::ProbeView(i);
            pixelState.samplers[s] = backend.SamplerIndex(probeSampler);
            pixelVariant.shadowSamplers &= static_cast<std::uint16_t>(~(1u << s));
        }
    }
    if (shadow.alphaTest)
        pixelVariant.alphaTest = static_cast<std::uint8_t>(shadow.alphaFunc + 1);
    pixelVariant.forwardLighting = pass.forwardFrame != 0 ? 1 : 0;
    pixelVariant.opacityMicromaps = pass.forwardFrame != 0 && GetStatistics().opacityMicromapsAvailable ? 1 : 0;
    pixelVariant.fogEpilogue = pass.fogEpilogue ? 1 : 0;
    // The main view's translucent pass (shaders/game_fog.hlsli): 1 where the pass blends the scene's fog colour, 2 where
    // it blends another, 3 where it has no fog; forward twins take any of them for the main view.
    if (pass.fogFrame != 0) {
        pixelState.alpha[2] = pass.fogBlend;
        pixelState.alpha[3] = shadow.fogMode == kShaderFogColor ? 1.0f : shadow.fogMode != 0 ? 2.0f : 3.0f;
    }
    pixelVariant.clearCoverage = clearCoverage ? 1 : 0;
    pixelVariant.markMirror = clearCoverage && pass.marksMirror ? 1 : 0;
    if (pass.route == capture::Pass::Route::GBuffer && pass.lightWarpSampler >= 0 && pass.lightWarpSampler < kSamplers) {
        pixelVariant.lightWarpSlot = 1;
        pixelState.alpha[1] = LightWarpSlot(dynamic.textures[static_cast<size_t>(pass.lightWarpSampler)], ((shadow.srgbRead >> pass.lightWarpSampler) & 1) != 0);
    }
    StateBuffer vertexState = {};
    BindSamplers(*vertexBase, dynamic.vertexTextures.data(), kVertexSamplers, 0, vertexState, nullptr);

    PipelineKey key;
    std::memset(&key, 0, sizeof(key));
    translator::Variant vertexVariant;
    const std::vector<VertexElement>& elements = VertexElements(mesh.vertexFormat, mesh.streams[1].SizeInBytes != 0, mesh.flexFormat);
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
    const CompiledShader* pixelShader = depthOnly ? nullptr : Compiled(drawnPixelCode, pixelVariant, pixelName);
    if (vertexShader == nullptr || (pixelShader == nullptr && !depthOnly))
        return;
    key.vertexShader = vertexShader;
    key.pixelShader = proceduralSky ? &g_cache.skyShader : pixelShader;

    D3D12_RENDER_TARGET_BLEND_DESC& blend = key.blend.RenderTarget[0];
    blend.BlendEnable = blendEnabled;
    blend.SrcBlend = Blend(source, false);
    blend.DestBlend = Blend(destination, false);
    blend.BlendOp = BlendOp(op);
    const bool separate = alphaOverride.active ? alphaOverride.enable : shadow.separateAlpha != 0;
    blend.SrcBlendAlpha = Blend(separate ? (alphaOverride.active ? alphaOverride.source : shadow.srcBlendAlpha) : source, true);
    blend.DestBlendAlpha = Blend(separate ? (alphaOverride.active ? alphaOverride.destination : shadow.dstBlendAlpha) : destination, true);
    blend.BlendOpAlpha = BlendOp(separate ? (alphaOverride.active ? alphaOverride.op : shadow.blendOpAlpha) : op);
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = static_cast<UINT8>((colorWrite ? D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE : 0)
        | (alphaWrite ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0));
    if (clearCoverage) {
        key.blend.IndependentBlendEnable = TRUE;
        key.blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    if (pass.route == capture::Pass::Route::GBuffer) {
        // Direct3D 9 blends every target alike; the game leaves COLORWRITEENABLE1-3 at their default, and the
        // self-illumination target takes colour only from the shaders that write it.
        key.blend.IndependentBlendEnable = TRUE;
        for (UINT i = 1; i < targets.count; ++i) {
            key.blend.RenderTarget[i] = blend;
            key.blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        // Twins write self-illumination and, in alpha, the Phong exponent.
        key.blend.RenderTarget[gbuffer::kEmissiveTarget].RenderTargetWriteMask = pass.emissive ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
    }

    D3D12_RASTERIZER_DESC& raster = key.raster;
    raster.FillMode = shadow.polyModeFront == 1 ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    // Source's front faces are clockwise; MaterialCullMode_t 0 is D3DCULL_CCW, 1 D3DCULL_CW, 2 (GMod) none (0x180014c60).
    raster.CullMode = !shadow.culling || dynamic.cullMode == 2 ? D3D12_CULL_MODE_NONE : dynamic.cullMode == 1 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
    raster.DepthClipEnable = TRUE;
    if (targets.depth.ptr != 0) {
        // Direct3D 9's DEPTHBIAS is in depth-buffer z; Direct3D 12's counts the format's smallest step (Depth Bias,
        // Microsoft Learn). The slope scale means the same in both.
        const DepthBias bias = PassDepthBias(shadow.polyOffset);
        const double step = targets.depthFormat == DXGI_FORMAT_D16_UNORM ? 1.0 / 65535.0 : 1.0 / 16777215.0;
        raster.DepthBias = static_cast<INT>(std::lround(bias.constant / step));
        raster.SlopeScaledDepthBias = bias.slopeScale;
    }

    D3D12_DEPTH_STENCIL_DESC& depth = key.depthStencil;
    const bool reverse = false;
    depth.DepthEnable = shadow.depthTest != 0 && targets.depth.ptr != 0;
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
                diagnostics, shadow.vertexShader->name, shadow.vertexStaticIndex, dynamic.vertexShaderIndex, depthOnly ? "none" : shadow.pixelShader->name, shadow.pixelStaticIndex,
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
    // Direct3D 9's render target origin is the centre of the top-left pixel, so its geometry lands half a pixel right
    // of and below Direct3D 12's (Coordinate Systems (Direct3D 10), Microsoft Learn).
    const float halfPixel[4] = { 1.0f / viewport.Width, -1.0f / viewport.Height, 0, 0 };
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
    const D3D12_GPU_VIRTUAL_ADDRESS vertexStateAddress = UploadConstants(&vertexState, sizeof(vertexState));
    const D3D12_GPU_VIRTUAL_ADDRESS pixelStateAddress = UploadConstants(&pixelState, sizeof(pixelState));
    commands->SetGraphicsRootConstantBufferView(0, FloatConstants(*vertexShader, dynamic.vertexFloats.data(), kVertexFloatConstants, dynamic.vertexFloatsVersion, g_cache.vertexConstants, vertexStateAddress, pass.vertexPatches));
    commands->SetGraphicsRootConstantBufferView(1, vertexStateAddress);
    commands->SetGraphicsRootConstantBufferView(2, pixelShader != nullptr ? FloatConstants(*pixelShader, dynamic.pixelFloats.data(), kPixelFloatConstants, dynamic.pixelFloatsVersion, g_cache.pixelConstants, pixelStateAddress, pass.pixelPatches) : pixelStateAddress);
    commands->SetGraphicsRootConstantBufferView(3, pixelStateAddress);
    if (pass.forwardFrame != 0) {
        commands->SetGraphicsRootConstantBufferView(kRootForwardFrame, pass.forwardFrame);
        commands->SetGraphicsRootShaderResourceView(kRootForwardScene, pass.forwardScene);
    } else {
        // Shaders that may read their fog from the main view's frame constants (shaders/game_fog.hlsli) do so only
        // while gmodpp_alpha says so; elsewhere the parameter holds a valid buffer they leave unread.
        commands->SetGraphicsRootConstantBufferView(kRootForwardFrame, pass.fogFrame != 0 ? pass.fogFrame : pixelStateAddress);
    }
    if (proceduralSky)
        commands->SetGraphicsRootConstantBufferView(kRootForwardFrame, skyConstants);

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
    IssueDraws(commands, mesh);

    if (skyConstants != 0 && !sky->sky && sky->sunDisc)
        DrawSunDisc(key, skyConstants, mesh);
    // The capture's sky cube keeps the game's own sky, whose colours the procedural sky takes.
    if (pass.route == capture::Pass::Route::Game && pixelShader != nullptr && capture::DrawsSky()) {
        capture::RecordSkyShader(pixelCode);
        key.pixelShader = pixelShader;
        DrawSkyFaces(key, *vertexShader, *pixelShader, vertexState, pixelState, mesh);
    }
}

}
