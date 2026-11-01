#include "shader_cache.h"

#include "command_line.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

namespace gmodpp::shader_cache {

namespace {

constexpr std::uint32_t kMagic = 0x53504d47;

std::uint64_t Fnv(const void* data, size_t size, std::uint64_t hash = 14695981039346656037ull)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::filesystem::path FilePath(const std::filesystem::path& directory, std::uint64_t key)
{
    return directory / std::format("{:016x}.bin", key);
}

}

std::uint64_t LightingHeadersHash()
{
    static const std::uint64_t hash = [] {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (const auto& item : std::filesystem::directory_iterator(ModDirectory() / L"hlsl", error))
            files.push_back(item.path());
        std::sort(files.begin(), files.end());
        std::uint64_t result = 14695981039346656037ull;
        for (const std::filesystem::path& path : files) {
            std::ifstream file(path, std::ios::binary);
            std::ostringstream contents;
            contents << file.rdbuf();
            const std::string text = contents.str();
            result = Fnv(text.data(), text.size(), result);
        }
        return result;
    }();
    return hash;
}

std::filesystem::path Directory()
{
    return ModDirectory() / L"cache" / L"shaders";
}

std::uint64_t Key(std::span<const DWORD> code, const translator::Variant& variant)
{
    std::uint64_t key = Fnv(code.data(), code.size_bytes());
    key = Fnv(&variant.shadowSamplers, sizeof(variant.shadowSamplers), key);
    key = Fnv(&variant.integerInputs, sizeof(variant.integerInputs), key);
    key = Fnv(&variant.signedInputs, sizeof(variant.signedInputs), key);
    key = Fnv(&variant.alphaTest, sizeof(variant.alphaTest), key);
    const std::uint8_t clipPlanes = variant.clipPlanes;
    key = Fnv(&clipPlanes, sizeof(clipPlanes), key);
    if (variant.forwardLighting != 0) {
        const std::uint64_t headers = LightingHeadersHash();
        key = Fnv(&headers, sizeof(headers), Fnv("forward", 7, key));
    }
    if (variant.clearCoverage != 0)
        key = Fnv("coverage", 8, key);
    if (variant.markMirror != 0)
        key = Fnv("mirror", 6, key);
    if (variant.lightWarpSlot != 0)
        key = Fnv("lightwarp", 9, key);
    return Fnv(&kTranslatorVersion, sizeof(kTranslatorVersion), key);
}

bool Load(const std::filesystem::path& directory, std::uint64_t key, translator::Translation& info, std::vector<unsigned char>& dxil)
{
    std::ifstream file(FilePath(directory, key), std::ios::binary);
    if (!file)
        return false;
    std::uint32_t header[6] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!file || header[0] != kMagic || header[1] != kTranslatorVersion)
        return false;
    info.firstFloatConstant = header[2];
    info.floatConstantCount = header[3];
    info.colorOutputs = header[4];
    info.writesDepth = header[5] != 0;
    file.read(reinterpret_cast<char*>(info.samplers.data()), sizeof(info.samplers));
    std::uint32_t inputs = 0;
    file.read(reinterpret_cast<char*>(&inputs), sizeof(inputs));
    info.inputs.resize(inputs);
    file.read(reinterpret_cast<char*>(info.inputs.data()), static_cast<std::streamsize>(inputs * sizeof(translator::VertexInput)));
    std::uint32_t size = 0;
    file.read(reinterpret_cast<char*>(&size), sizeof(size));
    dxil.resize(size);
    file.read(reinterpret_cast<char*>(dxil.data()), size);
    return static_cast<bool>(file);
}

void Store(const std::filesystem::path& directory, std::uint64_t key, const translator::Translation& info, std::span<const unsigned char> dxil)
{
    std::ofstream file(FilePath(directory, key), std::ios::binary | std::ios::trunc);
    const std::uint32_t header[6] = { kMagic, kTranslatorVersion, info.firstFloatConstant, info.floatConstantCount, info.colorOutputs,
        info.writesDepth ? 1u : 0u };
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(info.samplers.data()), sizeof(info.samplers));
    const std::uint32_t inputs = static_cast<std::uint32_t>(info.inputs.size());
    file.write(reinterpret_cast<const char*>(&inputs), sizeof(inputs));
    file.write(reinterpret_cast<const char*>(info.inputs.data()), static_cast<std::streamsize>(inputs * sizeof(translator::VertexInput)));
    const std::uint32_t size = static_cast<std::uint32_t>(dxil.size());
    file.write(reinterpret_cast<const char*>(&size), sizeof(size));
    file.write(reinterpret_cast<const char*>(dxil.data()), size);
}

bool Contains(const std::filesystem::path& directory, std::uint64_t key)
{
    std::ifstream file(FilePath(directory, key), std::ios::binary);
    std::uint32_t header[2] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    return file && header[0] == kMagic && header[1] == kTranslatorVersion;
}

}
