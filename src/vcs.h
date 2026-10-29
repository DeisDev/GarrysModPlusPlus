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

// Reads every compiled combo of a .vcs file. Returns false with the reason in `error`.
bool ReadCombos(std::span<const std::uint8_t> file, Header& header, std::vector<Combo>& combos, std::string& error);

}
