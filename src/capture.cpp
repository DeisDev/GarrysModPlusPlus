#include "capture.h"

#include <cstdio>
#include <vector>

namespace gmodpp {

bool WriteBmp(const std::filesystem::path& path, UINT width, UINT height, const void* bgra, size_t pitch)
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
        const auto* src = static_cast<const unsigned char*>(bgra) + y * pitch;
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
