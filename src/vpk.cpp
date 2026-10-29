#include "vpk.h"

#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

namespace gmodpp {

namespace {

constexpr std::uint32_t kSignature = 0x55aa1234;
constexpr std::uint32_t kVersion = 2;
constexpr std::uint16_t kDirectoryArchive = 0x7fff;
constexpr std::uint16_t kTerminator = 0xffff;

#pragma pack(push, 1)
struct Header {
    std::uint32_t signature;
    std::uint32_t version;
    std::uint32_t treeSize;
    std::uint32_t fileDataSectionSize;
    std::uint32_t archiveMd5SectionSize;
    std::uint32_t otherMd5SectionSize;
    std::uint32_t signatureSectionSize;
};

struct DirectoryEntry {
    std::uint32_t crc;
    std::uint16_t preloadBytes;
    std::uint16_t archiveIndex;
    std::uint32_t entryOffset;
    std::uint32_t entryLength;
    std::uint16_t terminator;
};
#pragma pack(pop)

}

bool Vpk::Open(const std::filesystem::path& directoryFile, std::string& error)
{
    std::ifstream file(directoryFile, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Header header;
    if (data.size() < sizeof(header)) {
        error = std::format("cannot read {}", directoryFile.string());
        return false;
    }
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.signature != kSignature || header.version != kVersion || sizeof(header) + header.treeSize > data.size()) {
        error = std::format("{} is not a version 2 VPK directory", directoryFile.string());
        return false;
    }
    m_directoryFile = directoryFile;
    m_dataOffset = sizeof(header) + header.treeSize;
    m_entries.clear();

    size_t at = sizeof(header);
    const size_t end = sizeof(header) + header.treeSize;
    auto readString = [&](std::string& out) {
        const void* terminator = std::memchr(data.data() + at, 0, end - at);
        if (terminator == nullptr)
            return false;
        out.assign(data.data() + at, static_cast<size_t>(static_cast<const char*>(terminator) - (data.data() + at)));
        at += out.size() + 1;
        return true;
    };
    std::string extension, directory, name;
    while (readString(extension) && !extension.empty()) {
        while (readString(directory) && !directory.empty()) {
            while (readString(name) && !name.empty()) {
                DirectoryEntry entry;
                if (end - at < sizeof(entry)) {
                    error = "truncated VPK tree";
                    return false;
                }
                std::memcpy(&entry, data.data() + at, sizeof(entry));
                at += sizeof(entry);
                if (entry.terminator != kTerminator || end - at < entry.preloadBytes) {
                    error = "malformed VPK entry";
                    return false;
                }
                std::string path = directory == " " ? "" : directory + "/";
                path += name == " " ? "" : name;
                if (extension != " ")
                    path += "." + extension;
                Entry& added = m_entries.emplace_back(Entry{ path, entry.archiveIndex, entry.entryOffset, entry.entryLength, {} });
                added.preload.assign(data.begin() + at, data.begin() + at + entry.preloadBytes);
                at += entry.preloadBytes;
            }
        }
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> Vpk::Read(const Entry& entry, std::string& error) const
{
    std::vector<std::uint8_t> bytes = entry.preload;
    if (entry.length == 0)
        return bytes;
    std::filesystem::path archive = m_directoryFile;
    std::uint64_t offset = entry.offset;
    if (entry.archive == kDirectoryArchive) {
        offset += m_dataOffset;
    } else {
        std::wstring stem = m_directoryFile.stem().wstring();
        stem.resize(stem.size() - 3);
        archive.replace_filename(std::format(L"{}{:03}.vpk", stem, entry.archive));
    }
    std::ifstream file(archive, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(offset));
    const size_t start = bytes.size();
    bytes.resize(start + entry.length);
    if (!file.read(reinterpret_cast<char*>(bytes.data() + start), entry.length)) {
        error = std::format("cannot read {} from {}", entry.path, archive.string());
        return std::nullopt;
    }
    return bytes;
}

}
