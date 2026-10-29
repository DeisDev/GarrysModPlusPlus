#pragma once

#include <windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gmodpp::vcs {

// ShaderHeader_t from the SDK's shader_vcs_version.h (version 6).
struct Header {
    std::int32_t version;
    std::int32_t totalCombos;
    std::int32_t dynamicCombos;
    std::uint32_t flags;
    std::uint32_t centroidMask;
    std::uint32_t staticCombos;
    std::uint32_t sourceCrc32;
};

struct Combo {
    std::uint32_t staticCombo;
    std::uint32_t dynamicCombo;
    std::vector<DWORD> bytecode;
};

// The static combo directory of a .vcs file: StaticComboRecord_t (sorted, ending with a sentinel whose offset ends the
// last block) and StaticComboAliasRecord_t (version 6: static combos that share another's code).
struct Directory {
    struct Record {
        std::uint32_t staticCombo;
        std::uint32_t fileOffset;
    };
    struct Alias {
        std::uint32_t staticCombo;
        std::uint32_t sourceStaticCombo;
    };
    Header header = {};
    std::vector<Record> records;
    std::vector<Alias> aliases;
};

bool ReadDirectory(std::span<const std::uint8_t> file, Directory& directory, std::string& error);

// Reads the dynamic combos of one static combo, following its alias. Returns false with the reason in `error`; a
// static combo the file does not contain (skipped at compile time) is an error too.
bool ReadStaticCombo(std::span<const std::uint8_t> file, const Directory& directory, std::uint32_t staticCombo, std::vector<Combo>& combos,
    std::string& error);

// Reads every compiled combo of a .vcs file. Returns false with the reason in `error`.
bool ReadCombos(std::span<const std::uint8_t> file, Header& header, std::vector<Combo>& combos, std::string& error);

}
