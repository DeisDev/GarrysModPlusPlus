#pragma once

#include "shader_translator.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace gmodpp::shader_cache {

// Bumped whenever the translator's output changes, so the disk cache misses old translations.
inline constexpr std::uint32_t kTranslatorVersion = 5;

// The lighting headers forward-lit translations include (gmodplusplus\hlsl).
std::uint64_t LightingHeadersHash();

// A translation's key: the bytecode, the variant and the translator version.
std::uint64_t Key(std::span<const DWORD> code, const translator::Variant& variant);

// Compiled translations live in one file, gmodplusplus\cache\shaders.bin: a header, then records appended as shaders
// are compiled, each with its key and a checksum of its contents. The file's record index is read on first use. A
// torn or corrupt record ends the file there, and a file from another format or translator version starts over, as
// does one grown past a size limit. Thread-safe.
//
// Load fills everything Store wrote (the translation without its HLSL, and the DXIL).
bool Load(std::uint64_t key, translator::Translation& info, std::vector<unsigned char>& dxil);
void Store(std::uint64_t key, const translator::Translation& info, std::span<const unsigned char> dxil);
// Whether the file holds `key`, from its index alone.
bool Contains(std::uint64_t key);
// Whether compiled shaders can be stored (the file could be created or opened for writing).
bool Writable();
// The file's stamp, random for each new file: it changes whenever the cache starts over.
std::uint32_t Stamp();
// Opens the file on another thread, so the first shader the game asks for does not wait for it to be read.
void OpenInBackground();

// Where the HLSL of translations that fail to compile, and with -gmodpp_dev every compiled translation, is written.
std::filesystem::path DumpDirectory();

}
