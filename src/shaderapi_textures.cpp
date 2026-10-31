#include "shaderapi_textures.h"

#include "log.h"
#include "native_lighting.h"
#include "shaderapi.h"
#include "shaderapi_pipelines.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace gmodpp::native {

namespace {

// TEXTURE_CREATE_* (public/shaderapi/ishaderapi.h).
constexpr int kCreateCubemap = 0x1;
constexpr int kCreateRenderTarget = 0x2;
constexpr int kCreateDepthBuffer = 0x8;
// SHADER_TEXFILTERMODE_* and SHADER_TEXWRAPMODE_* (public/shaderapi/ishaderapi.h).
constexpr int kFilterNearest = 0;
constexpr int kFilterAnisotropic = 6;
// IShaderUtil (materialsystem.dll), in the SDK's order (shaderapi_slots.cpp).
constexpr unsigned kShaderUtilConvertImageFormat = 6;
// IVTFTexture (public/vtf/vtf.h), confirmed in shaderapidx9.dll's VTF upload (0x18002bfa0).
constexpr unsigned kVtfWidth = 14;
constexpr unsigned kVtfHeight = 15;
constexpr unsigned kVtfDepth = 16;
constexpr unsigned kVtfMipCount = 17;
constexpr unsigned kVtfFormat = 20;
constexpr unsigned kVtfFaceCount = 21;
constexpr unsigned kVtfIsCubeMap = 29;
constexpr unsigned kVtfImageData = 38;
constexpr int kCubeFaces = 6;

// GMod's shaderapidx9.dll answers for each ImageFormat on the reference machine (caps.txt): the nearest
// supported texture format (the same with and without filtering) and the nearest render target format.
constexpr std::array<int, kImageFormats> kNearestFormat = { 12, 12, 16, 16, 17, 5, 6, -1, 8, -1, -1, 12, 12, 13, 14, 15, 16, 17, 18, 19, 13,
    21, 22, 23, 24, 25, 26, 27, -1, 29, 30, 31, 32, -1, -1, -1, 36, 37, 38, -1, -1 };
constexpr std::array<int, kImageFormats> kNearestTargetFormat = { 12, 12, 16, 16, 17, 5, 12, -1, 8, -1, -1, 12, 12, -1, -1, -1, 16, 17, 18,
    12, -1, 12, -1, -1, 24, 25, -1, 27, -1, 29, 30, 31, 32, -1, -1, -1, 36, -1, -1, -1, -1 };

constexpr UINT Mapping(UINT r, UINT g, UINT b, UINT a) { return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(r, g, b, a); }
constexpr UINT kZero = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
constexpr UINT kOne = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;

// How an ImageFormat is stored. Bytes are per pixel, or per 4x4 block when `block` is set. Missing channels read as D3D9
// reads them (1, except A8's colour, 0; D3DFORMAT documentation).
struct FormatInfo {
    DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srgbView = DXGI_FORMAT_UNKNOWN;
    UINT mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    UINT bytes = 0;
    bool block = false;
    DXGI_FORMAT depthView = DXGI_FORMAT_UNKNOWN;
};

FormatInfo Info(int format)
{
    switch (format) {
    case 12: return { DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 4 };
    case 16: return { DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 4 };
    case 17: return { DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_UNKNOWN, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 2 };
    case 18: return { DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_UNKNOWN, Mapping(0, 1, 2, kOne), 2 };
    case 19: return { DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_UNKNOWN, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 2 };
    case 21: return { DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_UNKNOWN, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 2 };
    case 5: return { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN, Mapping(0, 0, 0, kOne), 1 };
    case 6: return { DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN, Mapping(0, 0, 0, 1), 2 };
    case 8: return { DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_UNKNOWN, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 1 };
    case 13:
    case 20: return { DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 8, true };
    case 14: return { DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 16, true };
    case 15: return { DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 16, true };
    case 22: return { DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_UNKNOWN, Mapping(0, 1, kOne, kOne), 2 };
    case 23: return { DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_UNKNOWN, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 4 };
    case 24: return { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 8 };
    case 25: return { DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_UNKNOWN, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 8 };
    case 27: return { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, Mapping(0, kOne, kOne, kOne), 4 };
    case 29: return { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_UNKNOWN, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 16 };
    case kFormatNvDst16:
        return { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN, Mapping(0, 0, 0, 0), 2, false, DXGI_FORMAT_D16_UNORM };
    case kFormatNvDst24:
    case kFormatNvIntz:
        return { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_UNKNOWN, Mapping(0, 0, 0, 0), 4, false,
            DXGI_FORMAT_D24_UNORM_S8_UINT };
    case 37: return { DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_UNKNOWN, Mapping(0, 1, kOne, kOne), 16, true };
    case 38: return { DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_UNKNOWN, Mapping(0, 0, 0, 0), 8, true };
    default: return {};
    }
}

// Bytes of a tightly packed ImageFormat row and the number of rows (block rows for compressed formats), for the
// formats materialsystem hands over.
UINT SourceBytes(int format)
{
    static constexpr std::array<UINT, kImageFormats> kBytes = { 4, 4, 3, 3, 2, 1, 2, 1, 1, 3, 3, 4, 4, 8, 16, 16, 4, 2, 2, 2, 8, 2, 2, 4, 8, 8, 4, 4,
        12, 16, 0, 0, 0, 0, 0, 0, 0, 16, 8, 8, 16 };
    return format >= 0 && format < kImageFormats ? kBytes[format] : 0;
}

bool IsBlockFormat(int format) { return format == 13 || format == 14 || format == 15 || format == 20 || format == 37 || format == 38 || format == 39 || format == 40; }

UINT RowBytes(int format, int width)
{
    return IsBlockFormat(format) ? std::max(1u, (static_cast<UINT>(width) + 3) / 4) * SourceBytes(format) : static_cast<UINT>(width) * SourceBytes(format);
}

UINT Rows(int format, int height) { return IsBlockFormat(format) ? std::max(1u, (static_cast<UINT>(height) + 3) / 4) : static_cast<UINT>(height); }

std::mutex g_texturesMutex;
std::unordered_set<Texture*> g_textures;
Texture* g_modified = nullptr;

template <class Fn>
Fn UtilSlot(unsigned slot)
{
    return reinterpret_cast<Fn>((*static_cast<void***>(GetBackend().shaderUtil))[slot]);
}

template <class Fn>
Fn VtfSlot(void* vtf, unsigned slot)
{
    return reinterpret_cast<Fn>((*static_cast<void***>(vtf))[slot]);
}

bool Supported(DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { format };
    return SUCCEEDED(GetBackend().Device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
        && (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D);
}

// The format to store, falling back to BGRA8888 (with converted uploads) where the device lacks a 16-bit format.
int StorageFormat(int format)
{
    const FormatInfo info = Info(format);
    if (info.view != DXGI_FORMAT_UNKNOWN && !Supported(info.view)) {
        log::Info("Native: the device lacks DXGI format {} for ImageFormat {}; storing BGRA8888", static_cast<int>(info.view), format);
        return kFormatBgra8888;
    }
    return format;
}

bool CreateCopy(Texture& texture, Texture::Copy& copy)
{
    Backend& backend = GetBackend();
    ID3D12Device* device = backend.Device();
    const FormatInfo info = Info(texture.storage);
    const bool target = texture.IsRenderTarget();
    const bool depthTarget = texture.IsDepth();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = texture.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(texture.width);
    desc.Height = static_cast<UINT>(texture.height);
    desc.DepthOrArraySize = static_cast<UINT16>(texture.depth > 1 ? texture.depth : texture.cube ? kCubeFaces : 1);
    desc.MipLevels = static_cast<UINT16>(texture.mips);
    desc.Format = info.resource;
    desc.SampleDesc.Count = 1;
    if (depthTarget)
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    else if (target)
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&copy.resource.resource));
    if (FAILED(hr)) {
        log::Info("Native: creating texture '{}' ({}x{}x{}, {} mips, format {}) failed: {:#010x}", texture.name, texture.width, texture.height,
            texture.depth, texture.mips, texture.storage, static_cast<unsigned>(hr));
        return false;
    }
    copy.resource.state = D3D12_RESOURCE_STATE_COMMON;

    auto createView = [&](DXGI_FORMAT format) {
        const UINT index = backend.Descriptors().Allocate();
        if (index == DescriptorHeap::kInvalid)
            return index;
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = format;
        view.Shader4ComponentMapping = info.mapping;
        if (texture.depth > 1) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            view.Texture3D.MipLevels = desc.MipLevels;
        } else if (texture.cube) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            view.TextureCube.MipLevels = desc.MipLevels;
        } else {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = desc.MipLevels;
        }
        device->CreateShaderResourceView(copy.resource.resource.Get(), &view, backend.Descriptors().Cpu(index));
        return index;
    };
    copy.view = createView(info.view);
    if (info.srgbView != DXGI_FORMAT_UNKNOWN)
        copy.srgbView = createView(info.srgbView);

    if (depthTarget) {
        copy.target = backend.DepthStencilViews().Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
        view.Format = info.depthView;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(copy.resource.resource.Get(), &view, copy.target);
    } else if (target) {
        for (int srgb = 0; srgb < 2; ++srgb) {
            const DXGI_FORMAT format = srgb ? info.srgbView : info.view;
            if (format == DXGI_FORMAT_UNKNOWN)
                continue;
            D3D12_CPU_DESCRIPTOR_HANDLE& handle = srgb ? copy.srgbTarget : copy.target;
            handle = backend.RenderTargetViews().Allocate();
            D3D12_RENDER_TARGET_VIEW_DESC view = {};
            view.Format = format;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(copy.resource.resource.Get(), &view, handle);
        }
    }
    return true;
}

void DestroyCopy(Texture::Copy& copy)
{
    Backend& backend = GetBackend();
    backend.Retire(std::move(copy.resource.resource));
    backend.RetireDescriptor(copy.view);
    backend.RetireDescriptor(copy.srgbView);
}

std::intptr_t Create(int width, int height, int depth, int format, int mips, int copies, int flags, const char* name)
{
    Backend& backend = GetBackend();
    if (!backend.HasDevice())
        return 0;
    auto texture = std::make_unique<Texture>();
    texture->name = name != nullptr ? name : "";
    texture->format = format;
    texture->width = std::max(width, 1);
    texture->height = std::max(height, 1);
    texture->depth = std::max(depth, 1);
    texture->mips = std::max(mips, 1);
    texture->flags = flags;
    texture->cube = (flags & kCreateCubemap) != 0;
    // Like Direct3D 9's shader API, a texture holds the nearest supported format of the one asked for, and uploads in
    // the asked format are converted (RGBA8888 is stored as BGRA8888).
    const int nearest = format >= 0 && format < kImageFormats ? (texture->IsRenderTarget() ? kNearestTargetFormat : kNearestFormat)[format] : -1;
    if (nearest == kFormatNvNull) {
        texture->storage = nearest;
    } else {
        if (nearest < 0 || Info(nearest).resource == DXGI_FORMAT_UNKNOWN) {
            log::Info("Native: texture '{}' asks for ImageFormat {}, which has no Direct3D 12 format here", texture->name, format);
            return 0;
        }
        texture->storage = StorageFormat(nearest);
        texture->copies.resize(static_cast<size_t>(std::max(copies, 1)));
        for (Texture::Copy& copy : texture->copies) {
            if (!CreateCopy(*texture, copy)) {
                for (Texture::Copy& created : texture->copies)
                    DestroyCopy(created);
                return 0;
            }
        }
    }
    std::scoped_lock lock(g_texturesMutex);
    Texture* raw = texture.release();
    g_textures.insert(raw);
    return reinterpret_cast<std::intptr_t>(raw);
}

// Copies a rectangle of `format` texels into one subresource of the texture's current copy, converting to the stored
// format through materialsystem's ConvertImageFormat as shaderapidx9 does.
void Upload(Texture& texture, int level, int face, int x, int y, int z, int width, int height, int depth, int format, int stride, const void* data)
{
    if (data == nullptr || texture.copies.empty() || level < 0 || level >= texture.mips)
        return;
    if (!GetBackend().OnRenderThread()) {
        static std::atomic<bool> logged = false;
        if (!logged.exchange(true))
            log::Info("Native: texture '{}' was uploaded off the render thread; texture uploads are not synchronised yet", texture.name);
    }
    std::vector<std::uint8_t> converted;
    const std::uint8_t* source = static_cast<const std::uint8_t*>(data);
    UINT sourceRow = stride > 0 ? static_cast<UINT>(stride) : RowBytes(format, width);
    if (format != texture.storage) {
        const UINT row = RowBytes(texture.storage, width);
        converted.resize(static_cast<size_t>(row) * Rows(texture.storage, height) * depth);
        const UINT slice = sourceRow * Rows(format, height);
        for (int s = 0; s < depth; ++s) {
            using ConvertFn = bool (*)(void*, unsigned char*, int, unsigned char*, int, int, int, int, int);
            const bool ok = UtilSlot<ConvertFn>(kShaderUtilConvertImageFormat)(GetBackend().shaderUtil, const_cast<std::uint8_t*>(source) + s * slice, format,
                converted.data() + static_cast<size_t>(s) * row * Rows(texture.storage, height), texture.storage, width, height,
                static_cast<int>(sourceRow), static_cast<int>(row));
            if (!ok) {
                log::Info("Native: converting texture '{}' from ImageFormat {} to {} failed", texture.name, format, texture.storage);
                return;
            }
        }
        source = converted.data();
        sourceRow = row;
        format = texture.storage;
    }

    Backend& backend = GetBackend();
    const UINT rows = Rows(format, height);
    const UINT pitch = (RowBytes(format, width) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    Backend::Upload staging = backend.AllocateStaging(static_cast<UINT64>(pitch) * rows * depth, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (staging.data == nullptr)
        return;
    const UINT copyBytes = std::min(sourceRow, RowBytes(format, width));
    for (int s = 0; s < depth; ++s) {
        for (UINT r = 0; r < rows; ++r)
            std::memcpy(staging.data + (static_cast<size_t>(s) * rows + r) * pitch, source + (static_cast<size_t>(s) * rows + r) * sourceRow, copyBytes);
    }

    Texture::Copy& copy = texture.Current();
    backend.Transition(copy.resource, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = copy.resource.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = static_cast<UINT>(level + (texture.cube ? face * texture.mips : 0));
    D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
    sourceLocation.pResource = staging.buffer;
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sourceLocation.PlacedFootprint.Offset = staging.offset;
    sourceLocation.PlacedFootprint.Footprint.Format = Info(format).resource;
    // Copies of block-compressed data are sized in whole blocks.
    const UINT blockWidth = IsBlockFormat(format) ? (static_cast<UINT>(width) + 3) & ~3u : static_cast<UINT>(width);
    const UINT blockHeight = IsBlockFormat(format) ? (static_cast<UINT>(height) + 3) & ~3u : static_cast<UINT>(height);
    sourceLocation.PlacedFootprint.Footprint.Width = blockWidth;
    sourceLocation.PlacedFootprint.Footprint.Height = blockHeight;
    sourceLocation.PlacedFootprint.Footprint.Depth = static_cast<UINT>(depth);
    sourceLocation.PlacedFootprint.Footprint.RowPitch = pitch;
    const D3D12_BOX box = { 0, 0, 0, blockWidth, blockHeight, static_cast<UINT>(depth) };
    backend.Commands()->CopyTextureRegion(&destination, static_cast<UINT>(x), static_cast<UINT>(y), static_cast<UINT>(z), &sourceLocation, &box);
}

Texture* Modified()
{
    Texture* texture = g_modified;
    if (texture != nullptr && texture->advance) {
        texture->advance = false;
        if (texture->copies.size() > 1)
            texture->current = (texture->current + 1) % static_cast<UINT>(texture->copies.size());
    }
    return texture;
}

std::intptr_t CreateTexture(NativeObject*, int width, int height, int depth, int format, int mips, int copies, int flags, const char* name, const char*)
{
    return Create(width, height, depth, format, mips, copies, flags, name);
}

void CreateTextures(NativeObject*, std::intptr_t* handles, int count, int width, int height, int depth, int format, int mips, int copies, int flags,
    const char* name, const char*)
{
    for (int i = 0; i < count; ++i)
        handles[i] = Create(width, height, depth, format, mips, copies, flags, name);
}

// The depth buffer of a render target that cannot share the back buffer's (materialsystem's CreateDepthTexture).
std::intptr_t CreateDepthTexture(NativeObject*, int, int width, int height, const char* name, bool)
{
    return Create(width, height, 1, kFormatNvDst24, 1, 1, kCreateRenderTarget | kCreateDepthBuffer, name);
}

void DeleteTexture(NativeObject*, std::intptr_t handle)
{
    Texture* texture = TextureFromHandle(handle);
    if (texture == nullptr)
        return;
    {
        std::scoped_lock lock(g_texturesMutex);
        g_textures.erase(texture);
    }
    if (g_modified == texture)
        g_modified = nullptr;
    Backend& backend = GetBackend();
    for (Texture::Copy& copy : texture->copies) {
        DestroyCopy(copy);
        if (texture->IsDepth())
            backend.DepthStencilViews().Free(copy.target);
        else
            backend.RenderTargetViews().Free(copy.target);
        backend.RenderTargetViews().Free(copy.srgbTarget);
    }
    delete texture;
}

bool IsTexture(NativeObject*, std::intptr_t handle) { return TextureFromHandle(handle) != nullptr; }
bool IsTextureResident(NativeObject*, std::intptr_t handle) { return TextureFromHandle(handle) != nullptr; }

void ModifyTexture(NativeObject*, std::intptr_t handle)
{
    g_modified = TextureFromHandle(handle);
    if (g_modified != nullptr)
        g_modified->advance = true;
}

void TexImage2D(NativeObject*, int level, int face, int, int z, int width, int height, int format, bool, void* data)
{
    if (Texture* texture = Modified())
        Upload(*texture, level, face, 0, 0, z, width, height, 1, format, 0, data);
}

void TexSubImage2D(NativeObject*, int level, int face, int x, int y, int z, int width, int height, int format, int stride, bool, void* data)
{
    if (Texture* texture = Modified())
        Upload(*texture, level, face, x, y, z, width, height, 1, format, stride, data);
}

// shaderapidx9.dll 0x1800212b0: the texture's levels come from the same levels of the VTF, which materialsystem has
// already converted and trimmed to match.
void TexImageFromVTF(NativeObject*, void* vtf, int frame)
{
    Texture* texture = Modified();
    if (texture == nullptr || vtf == nullptr)
        return;
    const int format = VtfSlot<int (*)(void*)>(vtf, kVtfFormat)(vtf);
    const int width = VtfSlot<int (*)(void*)>(vtf, kVtfWidth)(vtf);
    const int height = VtfSlot<int (*)(void*)>(vtf, kVtfHeight)(vtf);
    const int depth = VtfSlot<int (*)(void*)>(vtf, kVtfDepth)(vtf);
    const int mips = VtfSlot<int (*)(void*)>(vtf, kVtfMipCount)(vtf);
    const int faces = VtfSlot<int (*)(void*)>(vtf, kVtfFaceCount)(vtf);
    const bool cube = VtfSlot<bool (*)(void*)>(vtf, kVtfIsCubeMap)(vtf);
    if (width != texture->width || height != texture->height || mips < texture->mips) {
        log::Info("Native: VTF {}x{} with {} mips does not match texture '{}' ({}x{}, {} mips)", width, height, mips, texture->name, texture->width,
            texture->height, texture->mips);
        return;
    }
    using ImageDataFn = unsigned char* (*)(void*, int, int, int);
    const int uploadFaces = cube && texture->cube ? std::min(faces, kCubeFaces) : 1;
    for (int face = 0; face < uploadFaces; ++face) {
        for (int level = 0; level < texture->mips; ++level) {
            unsigned char* data = VtfSlot<ImageDataFn>(vtf, kVtfImageData)(vtf, frame, face, level);
            const int levelWidth = std::max(1, width >> level);
            const int levelHeight = std::max(1, height >> level);
            const int levelDepth = std::max(1, depth >> level);
            Upload(*texture, level, face, 0, 0, 0, levelWidth, levelHeight, levelDepth, format, 0, data);
        }
    }
}

void TexSetPriority(NativeObject*, int) {}

// shaderapidx9.dll 0x1800216a0: the mip filter is NONE for single-level textures.
void TexMinFilter(NativeObject*, int mode)
{
    Texture* texture = g_modified;
    if (texture == nullptr)
        return;
    static constexpr D3DTEXTUREFILTERTYPE kMin[] = { D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_ANISOTROPIC };
    static constexpr D3DTEXTUREFILTERTYPE kMip[] = { D3DTEXF_NONE, D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR };
    if (mode < kFilterNearest || mode > kFilterAnisotropic) {
        log::Info("Native: TexMinFilter mode {} is unknown", mode);
        return;
    }
    texture->minFilter = kMin[mode];
    texture->mipFilter = texture->mips == 1 ? D3DTEXF_NONE : kMip[mode];
}

// shaderapidx9.dll 0x180021580: anisotropic magnification only where the D3D9 caps allow it, which NVIDIA's do not;
// Direct3D 12's anisotropic filter covers magnification whenever minification is anisotropic.
void TexMagFilter(NativeObject*, int mode)
{
    Texture* texture = g_modified;
    if (texture == nullptr)
        return;
    if (mode == 0)
        texture->magFilter = D3DTEXF_POINT;
    else if (mode == 1 || mode == kFilterAnisotropic)
        texture->magFilter = D3DTEXF_LINEAR;
    else
        log::Info("Native: TexMagFilter mode {} is invalid", mode);
}

void TexWrap(NativeObject*, int coordinate, int mode)
{
    Texture* texture = g_modified;
    if (texture == nullptr)
        return;
    const D3DTEXTUREADDRESS address = mode == 1 ? D3DTADDRESS_WRAP : mode == 2 ? D3DTADDRESS_BORDER : D3DTADDRESS_CLAMP;
    if (coordinate == 0)
        texture->addressU = address;
    else if (coordinate == 1)
        texture->addressV = address;
    else if (coordinate == 2)
        texture->addressW = address;
}

// Copies a rectangle of render target 0 (the back buffer for the default target) to memory in `format`, waiting for
// the GPU like Direct3D 9's GetRenderTargetData.
void ReadTarget(int x, int y, int width, int height, unsigned char* data, int format, int stride)
{
    Backend& backend = GetBackend();
    const std::intptr_t handle = Dynamic().colorTargets[0];
    Texture* texture = TextureFromHandle(handle);
    TrackedResource* source = nullptr;
    int sourceFormat = kFormatBgra8888;
    UINT targetWidth = backend.Width();
    UINT targetHeight = backend.Height();
    if (texture != nullptr && !texture->copies.empty() && !texture->IsDepth()) {
        source = &texture->Current().resource;
        sourceFormat = texture->storage;
        targetWidth = static_cast<UINT>(texture->width);
        targetHeight = static_cast<UINT>(texture->height);
    } else {
        realtime::OnBackBufferRead();
        source = &backend.BackBuffer();
    }
    const UINT left = static_cast<UINT>(std::clamp(x, 0, static_cast<int>(targetWidth)));
    const UINT top = static_cast<UINT>(std::clamp(y, 0, static_cast<int>(targetHeight)));
    const UINT right = std::min(targetWidth, left + static_cast<UINT>(std::max(width, 0)));
    const UINT bottom = std::min(targetHeight, top + static_cast<UINT>(std::max(height, 0)));
    if (right <= left || bottom <= top || data == nullptr)
        return;
    const UINT copyWidth = right - left;
    const UINT copyHeight = bottom - top;
    const UINT row = RowBytes(sourceFormat, static_cast<int>(copyWidth));
    const UINT pitch = (row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(pitch) * copyHeight;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) {
        log::Info("Native: creating a {}-byte readback buffer failed: {:#010x}", desc.Width, static_cast<unsigned>(hr));
        return;
    }
    backend.Transition(*source, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint = { source->resource->GetDesc().Format, copyWidth, copyHeight, 1, pitch };
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = source->resource.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box = { left, top, 0, right, bottom, 1 };
    backend.Commands()->CopyTextureRegion(&destination, 0, 0, 0, &from, &box);
    backend.Flush();

    std::uint8_t* mapped = nullptr;
    const D3D12_RANGE range = { 0, static_cast<SIZE_T>(desc.Width) };
    if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped))))
        return;
    const UINT destinationRow = stride > 0 ? static_cast<UINT>(stride) : RowBytes(format, static_cast<int>(copyWidth));
    if (format == sourceFormat) {
        for (UINT r = 0; r < copyHeight; ++r)
            std::memcpy(data + static_cast<size_t>(r) * destinationRow, mapped + static_cast<size_t>(r) * pitch, std::min(row, destinationRow));
    } else {
        using ConvertFn = bool (*)(void*, unsigned char*, int, unsigned char*, int, int, int, int, int);
        UtilSlot<ConvertFn>(kShaderUtilConvertImageFormat)(GetBackend().shaderUtil, mapped, sourceFormat, data, format, static_cast<int>(copyWidth),
            static_cast<int>(copyHeight), static_cast<int>(pitch), static_cast<int>(destinationRow));
    }
    readback->Unmap(0, nullptr);
}

void ReadPixels(NativeObject*, int x, int y, int width, int height, unsigned char* data, int format) { ReadTarget(x, y, width, height, data, format, 0); }

// The Rect_t overload reads the source rectangle; the destination rectangle's position offsets the data.
void ReadPixelsRect(NativeObject*, const int* sourceRect, const int* destinationRect, unsigned char* data, int format, int stride)
{
    if (sourceRect == nullptr)
        return;
    unsigned char* start = data;
    if (destinationRect != nullptr)
        start += static_cast<size_t>(destinationRect[1]) * stride + static_cast<size_t>(destinationRect[0]) * SourceBytes(format);
    ReadTarget(sourceRect[0], sourceRect[1], sourceRect[2], sourceRect[3], start, format, stride);
}

// CPixelWriter (public/pixelwriter.h), which materialsystem writes lightmaps through; its SetPixelMemory is inline, so
// TexLock fills it in as the header would.
struct PixelWriter {
    unsigned char* base;
    unsigned char* bits;
    unsigned short bytesPerRow;
    unsigned char size;
    unsigned char flags;
    short shift[4];
    unsigned mask[4];
};
static_assert(sizeof(PixelWriter) == 48);

void SetPixelMemory(PixelWriter& writer, int format, unsigned char* memory, int stride)
{
    constexpr unsigned char kFloat = 0x01;
    constexpr unsigned char kHalf = 0x02;
    struct Layout {
        int format;
        unsigned char size;
        unsigned char flags;
        short shift[4];
        unsigned mask[4];
    };
    static constexpr Layout kLayouts[] = {
        { 27, 4, kFloat, { 0, 0, 0, 0 }, { 0xFFFFFFFF, 0, 0, 0 } },
        { 29, 16, kFloat, { 0, 32, 64, 96 }, { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF } },
        { 24, 8, kFloat | kHalf, { 0, 16, 32, 48 }, { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF } },
        { 0, 4, 0, { 0, 8, 16, 24 }, { 0xFF, 0xFF, 0xFF, 0xFF } },
        { 12, 4, 0, { 16, 8, 0, 24 }, { 0xFF, 0xFF, 0xFF, 0xFF } },
        { 16, 4, 0, { 16, 8, 0, 24 }, { 0xFF, 0xFF, 0xFF, 0 } },
        { 19, 2, 0, { 4, 0, -4, 8 }, { 0xF0, 0xF0, 0xF0, 0xF0 } },
        { 3, 3, 0, { 16, 8, 0, 0 }, { 0xFF, 0xFF, 0xFF, 0 } },
        { 17, 2, 0, { 8, 3, -3, 0 }, { 0xF8, 0xFC, 0xF8, 0 } },
        { 21, 2, 0, { 7, 2, -3, 8 }, { 0xF8, 0xF8, 0xF8, 0x80 } },
        { 18, 2, 0, { 7, 2, -3, 8 }, { 0xF8, 0xF8, 0xF8, 0x80 } },
        { 8, 1, 0, { 0, 0, 0, 0 }, { 0, 0, 0, 0xFF } },
        { 23, 4, 0, { 0, 8, 16, 24 }, { 0xFF, 0xFF, 0xFF, 0xFF } },
        { 25, 8, 0, { 0, 16, 32, 48 }, { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF } },
        { 5, 1, 0, { 0, 0, 0, 0 }, { 0xFF, 0, 0, 0 } },
    };
    writer = {};
    writer.base = writer.bits = memory;
    writer.bytesPerRow = static_cast<unsigned short>(stride);
    for (const Layout& layout : kLayouts) {
        if (layout.format == format) {
            writer.size = layout.size;
            writer.flags = layout.flags;
            std::copy(std::begin(layout.shift), std::end(layout.shift), writer.shift);
            std::copy(std::begin(layout.mask), std::end(layout.mask), writer.mask);
            return;
        }
    }
    log::Info("Native: TexLock cannot write ImageFormat {}", format);
}

struct LockedRect {
    Texture* texture = nullptr;
    int level = 0;
    int face = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> data;
};
LockedRect g_locked;

// TexLock: materialsystem writes the rectangle through the pixel writer, and TexUnlock uploads it.
bool TexLock(NativeObject*, int level, int face, int x, int y, int width, int height, PixelWriter& writer)
{
    Texture* texture = Modified();
    if (texture == nullptr || texture->copies.empty() || width <= 0 || height <= 0 || IsBlockFormat(texture->storage))
        return false;
    g_locked = { texture, level, face, x, y, width, height, {} };
    const UINT row = RowBytes(texture->storage, width);
    g_locked.data.assign(static_cast<size_t>(row) * height, 0);
    SetPixelMemory(writer, texture->storage, g_locked.data.data(), static_cast<int>(row));
    return writer.size != 0;
}

void TexUnlock(NativeObject*)
{
    LockedRect locked = std::move(g_locked);
    g_locked = {};
    if (locked.texture == nullptr || TextureFromHandle(reinterpret_cast<std::intptr_t>(locked.texture)) == nullptr)
        return;
    Upload(*locked.texture, locked.level, locked.face, locked.x, locked.y, 0, locked.width, locked.height, 1, locked.texture->storage, 0, locked.data.data());
}

CopySurface TextureSurface(Texture& texture)
{
    Texture::Copy& copy = texture.Current();
    return { &copy.resource, copy.view, copy.target, texture.TargetFormat(false), static_cast<UINT>(texture.width), static_cast<UINT>(texture.height) };
}

// The colour of render target `id` as Direct3D 9's GetRenderTarget gives it: the back buffer for the default target 0,
// resolved when it is read.
bool TargetSurface(int id, bool read, CopySurface& surface)
{
    if (id < 0 || id >= kRenderTargets)
        return false;
    const std::intptr_t handle = Dynamic().colorTargets[static_cast<size_t>(id)];
    if (id == 0 && handle == kDefaultTarget) {
        if (read)
            realtime::OnBackBufferRead();
        Backend& backend = GetBackend();
        surface = { &backend.BackBuffer(), backend.BackBufferTexture(), backend.BackBufferView(false), Backend::kBackBufferFormat, backend.Width(),
            backend.Height() };
        return true;
    }
    Texture* texture = TextureFromHandle(handle);
    if (texture == nullptr || texture->copies.empty() || texture->IsDepth())
        return false;
    surface = TextureSurface(*texture);
    return true;
}

// Rect_t is x, y, width, height.
D3D12_RECT Rect(const int* rect, UINT width, UINT height)
{
    if (rect == nullptr)
        return { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    return { rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3] };
}

// CopyRenderTargetToTextureEx (0x180013ff0): StretchRect from render target `id` into the texture's top level.
void CopyRenderTargetToTextureEx(NativeObject*, std::intptr_t handle, int id, const int* sourceRect, const int* destinationRect)
{
    Texture* texture = TextureFromHandle(handle);
    CopySurface source;
    if (texture == nullptr || texture->copies.empty() || texture->copies.size() > 1 || !TargetSurface(id, true, source))
        return;
    const CopySurface destination = TextureSurface(*texture);
    StretchRect(source, Rect(sourceRect, source.width, source.height), destination, Rect(destinationRect, destination.width, destination.height));
}

void CopyRenderTargetToTexture(NativeObject* self, std::intptr_t handle) { CopyRenderTargetToTextureEx(self, handle, 0, nullptr, nullptr); }

// CopyTextureToRenderTargetEx (0x1800141d0): StretchRect from the texture into render target `id`.
void CopyTextureToRenderTargetEx(NativeObject*, int id, std::intptr_t handle, const int* sourceRect, const int* destinationRect)
{
    Texture* texture = TextureFromHandle(handle);
    CopySurface destination;
    if (texture == nullptr || texture->copies.empty() || texture->copies.size() > 1 || !TargetSurface(id, false, destination))
        return;
    const CopySurface source = TextureSurface(*texture);
    StretchRect(source, Rect(sourceRect, source.width, source.height), destination, Rect(destinationRect, destination.width, destination.height));
}

int GetNearestSupportedFormat(NativeObject*, int format, bool) { return format >= 0 && format < kImageFormats ? kNearestFormat[format] : -1; }
int GetNearestRenderTargetFormat(NativeObject*, int format) { return format >= 0 && format < kImageFormats ? kNearestTargetFormat[format] : -1; }

}

bool Texture::IsRenderTarget() const { return (flags & (kCreateRenderTarget | kCreateDepthBuffer)) != 0; }
bool Texture::IsDepth() const { return Info(storage).depthView != DXGI_FORMAT_UNKNOWN; }

DXGI_FORMAT Texture::TargetFormat(bool srgb) const
{
    const FormatInfo info = Info(storage);
    if (IsDepth())
        return info.depthView;
    return srgb && info.srgbView != DXGI_FORMAT_UNKNOWN ? info.srgbView : info.view;
}

Texture* TextureFromHandle(std::intptr_t handle)
{
    if (handle == 0 || handle == -1 || handle == -2)
        return nullptr;
    Texture* texture = reinterpret_cast<Texture*>(handle);
    std::scoped_lock lock(g_texturesMutex);
    return g_textures.contains(texture) ? texture : nullptr;
}

D3D12_SAMPLER_DESC SamplerFor(const Texture& texture, bool border, int pointFilter, int anisotropy, int minOverride, int magOverride)
{
    D3DTEXTUREFILTERTYPE minFilter = texture.minFilter;
    D3DTEXTUREFILTERTYPE magFilter = texture.magFilter;
    D3DTEXTUREFILTERTYPE mipFilter = texture.mipFilter;
    // BindTexture (0x18001fe60): the minification override replaces the minification filter, the magnification
    // override the magnification filter and, for textures with mips, the mip filter.
    if (minOverride != 0)
        minFilter = static_cast<D3DTEXTUREFILTERTYPE>(minOverride);
    if (magOverride != 0) {
        magFilter = static_cast<D3DTEXTUREFILTERTYPE>(magOverride);
        if (texture.mips > 1)
            mipFilter = static_cast<D3DTEXTUREFILTERTYPE>(magOverride);
    }
    if (pointFilter == 1) {
        minFilter = D3DTEXF_POINT;
        magFilter = D3DTEXF_POINT;
        mipFilter = D3DTEXF_POINT;
    } else if (pointFilter == 2) {
        magFilter = D3DTEXF_POINT;
    }
    const bool compare = texture.storage == kFormatNvDst16 || texture.storage == kFormatNvDst24;
    D3D12_SAMPLER_DESC desc = {};
    if (minFilter == D3DTEXF_ANISOTROPIC || magFilter == D3DTEXF_ANISOTROPIC) {
        desc.Filter = compare ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = static_cast<UINT>(std::clamp(anisotropy, 1, 16));
    } else {
        const D3D12_FILTER_TYPE min = minFilter == D3DTEXF_POINT ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
        const D3D12_FILTER_TYPE mag = magFilter == D3DTEXF_POINT ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
        const D3D12_FILTER_TYPE mip = mipFilter == D3DTEXF_LINEAR ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
        desc.Filter = D3D12_ENCODE_BASIC_FILTER(min, mag, mip, compare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD);
        desc.MaxAnisotropy = 1;
    }
    auto address = [&](D3DTEXTUREADDRESS value) {
        if (border)
            return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        switch (value) {
        case D3DTADDRESS_CLAMP: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case D3DTADDRESS_BORDER: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        case D3DTADDRESS_MIRROR: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        }
    };
    desc.AddressU = address(texture.addressU);
    desc.AddressV = address(texture.addressV);
    desc.AddressW = address(texture.addressW);
    // D3D9's comparison for hardware shadow maps passes when the reference is nearer than the stored depth.
    desc.ComparisonFunc = compare ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_NEVER;
    // D3D9 samples only the top level without a mip filter.
    desc.MaxLOD = mipFilter == D3DTEXF_NONE ? 0.0f : D3D12_FLOAT32_MAX;
    return desc;
}

void BindTextureMethods(NativeClass& c)
{
    c.Bind("CreateTexture", &CreateTexture);
    c.Bind("CreateTextures", &CreateTextures);
    c.Bind("CreateDepthTexture", &CreateDepthTexture);
    c.Bind("DeleteTexture", &DeleteTexture);
    c.Bind("IsTexture", &IsTexture);
    c.Bind("IsTextureResident", &IsTextureResident);
    c.Bind("ModifyTexture", &ModifyTexture);
    c.Bind("TexImage2D", &TexImage2D);
    c.Bind("TexSubImage2D", &TexSubImage2D);
    c.Bind("TexImageFromVTF", &TexImageFromVTF);
    c.Bind("TexSetPriority", &TexSetPriority);
    c.Bind("TexMinFilter", &TexMinFilter);
    c.Bind("TexMagFilter", &TexMagFilter);
    c.Bind("TexWrap", &TexWrap);
    c.Bind("TexLock", &TexLock);
    c.Bind("TexUnlock", &TexUnlock);
    c.Bind("CopyRenderTargetToTexture", &CopyRenderTargetToTexture);
    c.Bind("CopyRenderTargetToTextureEx", &CopyRenderTargetToTextureEx);
    c.Bind("CopyTextureToRenderTargetEx", &CopyTextureToRenderTargetEx);
    c.Bind("ReadPixels", &ReadPixelsRect, 0);
    c.Bind("ReadPixels", &ReadPixels, 1);
    c.Bind("GetNearestSupportedFormat", &GetNearestSupportedFormat);
    c.Bind("GetNearestRenderTargetFormat", &GetNearestRenderTargetFormat);
}

}
