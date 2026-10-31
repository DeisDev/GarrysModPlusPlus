#pragma once

#include "shader_translator.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace gmodpp::shader_cache {

// Bumped whenever the translator's output changes, so the disk cache misses old translations.
inline constexpr std::uint32_t kTranslatorVersion = 4;

// gmodplusplus\cache\shaders.
std::filesystem::path Directory();

// A translation's key: the bytecode, the variant and the translator version.
std::uint64_t Key(std::span<const DWORD> code, const translator::Variant& variant);

// One file per key. Load fills everything Store wrote (the translation without its HLSL, and the DXIL).
bool Load(const std::filesystem::path& directory, std::uint64_t key, translator::Translation& info, std::vector<unsigned char>& dxil);
void Store(const std::filesystem::path& directory, std::uint64_t key, const translator::Translation& info, std::span<const unsigned char> dxil);
bool Contains(const std::filesystem::path& directory, std::uint64_t key);

}
