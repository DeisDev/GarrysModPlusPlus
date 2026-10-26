#include "capture.h"

#include "log.h"

#include <wrl/client.h>

#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

bool WriteBmp(const std::filesystem::path& path, UINT width, UINT height, const D3DLOCKED_RECT& locked)
{
    const UINT rowBytes = width * 3;
    const UINT stride = (rowBytes + 3) & ~3u;
    BITMAPFILEHEADER file = {};
    BITMAPINFOHEADER info = {};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + stride * height;
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 24;

    FILE* out = nullptr;
    if (_wfopen_s(&out, path.c_str(), L"wb") != 0)
        return false;
    std::fwrite(&file, sizeof(file), 1, out);
    std::fwrite(&info, sizeof(info), 1, out);
    std::vector<unsigned char> row(stride, 0);
    for (UINT y = 0; y < height; ++y) {
        const auto* src = static_cast<const unsigned char*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
        for (UINT x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        std::fwrite(row.data(), stride, 1, out);
    }
    return std::fclose(out) == 0;
}

}

bool CaptureBackBuffer(IDirect3DDevice9* device, const std::filesystem::path& path)
{
    ComPtr<IDirect3DSurface9> backBuffer;
    HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    D3DSURFACE_DESC desc = {};
    if (SUCCEEDED(hr))
        hr = backBuffer->GetDesc(&desc);
    if (FAILED(hr)) {
        log::Info("Capture: GetBackBuffer failed: {:#010x}", static_cast<unsigned>(hr));
        return false;
    }
    if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) {
        log::Info("Capture: unsupported back buffer format {}", static_cast<int>(desc.Format));
        return false;
    }

    ComPtr<IDirect3DSurface9> resolved = backBuffer;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        hr = device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr);
        if (SUCCEEDED(hr))
            hr = device->StretchRect(backBuffer.Get(), nullptr, resolved.Get(), nullptr, D3DTEXF_NONE);
    }

    ComPtr<IDirect3DSurface9> staging;
    if (SUCCEEDED(hr))
        hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr);
    if (SUCCEEDED(hr))
        hr = device->GetRenderTargetData(resolved.Get(), staging.Get());
    D3DLOCKED_RECT locked = {};
    if (SUCCEEDED(hr))
        hr = staging->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        log::Info("Capture: readback failed: {:#010x}", static_cast<unsigned>(hr));
        return false;
    }

    bool written = WriteBmp(path, desc.Width, desc.Height, locked);
    staging->UnlockRect();
    log::Info("Capture: {} {}", written ? "wrote" : "failed to write", path.string());
    return written;
}

}
