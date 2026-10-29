#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gmodpp {

// A Valve PacK (version 2) as described on the Valve Developer Community wiki ("VPK (file format)"): a <name>_dir.vpk
// directory and its numbered archives.
class Vpk {
public:
    struct Entry {
        std::string path;
        std::uint16_t archive;
        std::uint32_t offset;
        std::uint32_t length;
        std::vector<std::uint8_t> preload;
    };

    bool Open(const std::filesystem::path& directoryFile, std::string& error);
    const std::vector<Entry>& Entries() const { return m_entries; }
    std::optional<std::vector<std::uint8_t>> Read(const Entry& entry, std::string& error) const;

private:
    std::filesystem::path m_directoryFile;
    std::uint64_t m_dataOffset = 0;
    std::vector<Entry> m_entries;
};

}
