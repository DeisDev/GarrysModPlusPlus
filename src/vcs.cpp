#include "vcs.h"

#include <LzmaDec.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>

namespace gmodpp::vcs {

namespace {

constexpr std::int32_t kVersion = 6;
constexpr std::uint32_t kSentinel = 0xffffffff;
constexpr std::uint32_t kEndOfBlocks = 0xffffffff;
// Block size words, read from GMod's .vcs files: bit 31 marks an uncompressed block, and the low 30 bits are the
// block's size in the file (an LZMA block keeps the SDK's lzma_header_t, lzmaDecoder.h, in front of its data).
constexpr std::uint32_t kUncompressedBlock = 0x80000000;
constexpr std::uint32_t kBlockSizeMask = 0x3fffffff;
constexpr std::uint32_t kLzmaId = 'L' | ('Z' << 8) | ('M' << 16) | ('A' << 24);

#pragma pack(push, 1)
struct LzmaHeader {
    std::uint32_t id;
    std::uint32_t actualSize;
    std::uint32_t lzmaSize;
    std::uint8_t properties[LZMA_PROPS_SIZE];
};
#pragma pack(pop)

void* LzmaAlloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
void LzmaFree(ISzAllocPtr, void* address) { std::free(address); }
const ISzAlloc kLzmaAllocator = { LzmaAlloc, LzmaFree };

bool Decompress(std::span<const std::uint8_t> block, std::vector<std::uint8_t>& out)
{
    LzmaHeader header;
    if (block.size() < sizeof(header))
        return false;
    std::memcpy(&header, block.data(), sizeof(header));
    if (header.id != kLzmaId || header.lzmaSize > block.size() - sizeof(header))
        return false;
    out.resize(header.actualSize);
    SizeT outSize = header.actualSize;
    SizeT inSize = header.lzmaSize;
    ELzmaStatus status;
    return LzmaDecode(out.data(), &outSize, block.data() + sizeof(header), &inSize, header.properties, LZMA_PROPS_SIZE, LZMA_FINISH_END, &status,
               &kLzmaAllocator) == SZ_OK
        && outSize == header.actualSize;
}

// Decodes the blocks of one static combo, which start at `at` and end with an end-of-blocks word.
bool ReadBlocks(std::span<const std::uint8_t> file, size_t at, std::uint32_t staticCombo, std::vector<Combo>& combos, std::string& error)
{
    std::vector<std::uint8_t> decoded;
    for (;;) {
        std::uint32_t size;
        if (at + sizeof(size) > file.size()) {
            error = std::format("static combo {}: truncated block", staticCombo);
            return false;
        }
        std::memcpy(&size, file.data() + at, sizeof(size));
        at += sizeof(size);
        if (size == kEndOfBlocks)
            return true;
        const size_t length = size & kBlockSizeMask;
        if (at + length > file.size()) {
            error = std::format("static combo {}: block past the end of the file", staticCombo);
            return false;
        }
        std::span<const std::uint8_t> block = file.subspan(at, length);
        at += length;
        if (size & kUncompressedBlock)
            decoded.assign(block.begin(), block.end());
        else if (!Decompress(block, decoded)) {
            error = std::format("static combo {}: cannot decompress a block", staticCombo);
            return false;
        }
        for (size_t q = 0; q + 8 <= decoded.size();) {
            std::uint32_t dynamicCombo, bytes;
            std::memcpy(&dynamicCombo, decoded.data() + q, 4);
            std::memcpy(&bytes, decoded.data() + q + 4, 4);
            q += 8;
            if (bytes > decoded.size() - q || bytes % sizeof(DWORD) != 0) {
                error = std::format("static combo {}: malformed dynamic combo {}", staticCombo, dynamicCombo);
                return false;
            }
            Combo& combo = combos.emplace_back(Combo{ staticCombo, dynamicCombo, std::vector<DWORD>(bytes / sizeof(DWORD)) });
            std::memcpy(combo.bytecode.data(), decoded.data() + q, bytes);
            q += bytes;
        }
    }
}

}

bool ReadDirectory(std::span<const std::uint8_t> file, Directory& directory, std::string& error)
{
    Header& header = directory.header;
    if (file.size() < sizeof(header)) {
        error = "truncated header";
        return false;
    }
    std::memcpy(&header, file.data(), sizeof(header));
    if (header.version != kVersion) {
        error = std::format("version {}", header.version);
        return false;
    }
    size_t at = sizeof(header);
    if (header.staticCombos > (file.size() - at) / sizeof(Directory::Record)) {
        error = "truncated static combo records";
        return false;
    }
    directory.records.resize(header.staticCombos);
    std::memcpy(directory.records.data(), file.data() + at, directory.records.size() * sizeof(Directory::Record));
    at += directory.records.size() * sizeof(Directory::Record);
    std::uint32_t aliases = 0;
    if (at + sizeof(aliases) > file.size()) {
        error = "truncated alias count";
        return false;
    }
    std::memcpy(&aliases, file.data() + at, sizeof(aliases));
    at += sizeof(aliases);
    if (aliases > (file.size() - at) / sizeof(Directory::Alias)) {
        error = "truncated static combo aliases";
        return false;
    }
    directory.aliases.resize(aliases);
    std::memcpy(directory.aliases.data(), file.data() + at, directory.aliases.size() * sizeof(Directory::Alias));
    return true;
}

bool ReadStaticCombo(std::span<const std::uint8_t> file, const Directory& directory, std::uint32_t staticCombo, std::vector<Combo>& combos,
    std::string& error)
{
    std::uint32_t source = staticCombo;
    auto alias = std::ranges::lower_bound(directory.aliases, staticCombo, {}, &Directory::Alias::staticCombo);
    if (alias != directory.aliases.end() && alias->staticCombo == staticCombo)
        source = alias->sourceStaticCombo;
    auto record = std::ranges::lower_bound(directory.records, source, {}, &Directory::Record::staticCombo);
    if (record == directory.records.end() || record->staticCombo != source || source == kSentinel) {
        error = std::format("static combo {} is not in the file", staticCombo);
        return false;
    }
    return ReadBlocks(file, record->fileOffset, staticCombo, combos, error);
}

bool ReadCombos(std::span<const std::uint8_t> file, Header& header, std::vector<Combo>& combos, std::string& error)
{
    Directory directory;
    if (!ReadDirectory(file, directory, error))
        return false;
    header = directory.header;
    for (const Directory::Record& record : directory.records) {
        if (record.staticCombo != kSentinel && !ReadBlocks(file, record.fileOffset, record.staticCombo, combos, error))
            return false;
    }
    return true;
}

}
