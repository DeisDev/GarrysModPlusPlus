#pragma once

#include "shaderapi_device.h"

#include <d3d9types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gmodpp::native {

// ImageFormat values (public/bitmap/imageformat.h, without the _X360 formats).
enum ImageFormatValue : int {
    kFormatUnknown = -1,
    kFormatRgba8888 = 0,
    kFormatBgra8888 = 12,
    kFormatBgrx8888 = 16,
    kFormatNvDst16 = 30,
    kFormatNvDst24 = 31,
    kFormatNvIntz = 32,
    kFormatNvNull = 36,
    kImageFormats = 41,
};

// A ShaderAPITextureHandle_t (64-bit in GMod) is a pointer to a Texture; -1 names the back buffer or its depth buffer.
struct Texture {
    struct Copy {
        TrackedResource resource;
        UINT view = DescriptorHeap::kInvalid;
        UINT srgbView = DescriptorHeap::kInvalid;
        // Render target views (linear, sRGB) or the depth-stencil view.
        D3D12_CPU_DESCRIPTOR_HANDLE target = {};
        D3D12_CPU_DESCRIPTOR_HANDLE srgbTarget = {};
    };

    std::string name;
    // The ImageFormat materialsystem asked for, after GetNearestSupportedFormat, and the one the resource stores
    // (the same unless the device lacks the format and uploads are converted).
    int format = kFormatUnknown;
    int storage = kFormatUnknown;
    int width = 0;
    int height = 0;
    int depth = 1;
    int mips = 1;
    int flags = 0;
    bool cube = false;
    std::vector<Copy> copies;
    UINT current = 0;
    bool advance = false;
    // D3D9's sampler state, which shaderapidx9 keeps per texture (TexWrap, TexMinFilter, TexMagFilter at
    // 0x180021b40, 0x1800216a0, 0x180021580).
    D3DTEXTUREADDRESS addressU = D3DTADDRESS_WRAP;
    D3DTEXTUREADDRESS addressV = D3DTADDRESS_WRAP;
    D3DTEXTUREADDRESS addressW = D3DTADDRESS_WRAP;
    D3DTEXTUREFILTERTYPE magFilter = D3DTEXF_POINT;
    D3DTEXTUREFILTERTYPE minFilter = D3DTEXF_POINT;
    D3DTEXTUREFILTERTYPE mipFilter = D3DTEXF_NONE;
    // A floating-point render target asked for in the frame's format (GetBackBufferFormat), which holds what an 8-bit
    // sRGB target of the frame would: linear light, written and read as Direct3D 9 would its 8-bit bytes
    // (shaderapi_pipelines.cpp: sRGB-off writes decoded, sRGB-off reads encoded).
    bool srgbEmulated = false;

    Copy& Current() { return copies[current]; }
    bool IsRenderTarget() const;
    bool IsDepth() const;
    // The D3D12 format of render target views (sRGB or not), for pipeline states.
    DXGI_FORMAT TargetFormat(bool srgb) const;
};

// The texture behind a handle, or null for 0, -1 and -2.
Texture* TextureFromHandle(std::intptr_t handle);

// The sampler for a texture as D3D9 would sample it. `pointFilter` is command 2004's mode (1 point min, mag and mip,
// 2 point mag), `border` SetSamplerBorderAddress.
// `minOverride` and `magOverride` are GMod's filter overrides (0 for none).
D3D12_SAMPLER_DESC SamplerFor(const Texture& texture, bool border, int pointFilter, int anisotropy, int minOverride, int magOverride);

// Binds IShaderAPI's texture methods.
void BindTextureMethods(NativeClass& shaderApi);

}
