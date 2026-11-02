#include "shader_cache.h"

#include "command_line.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gmodpp::shader_cache {

namespace {

constexpr std::uint32_t kFileMagic = 0x43504d47;   // "GMPC"
constexpr std::uint32_t kRecordMagic = 0x53504d47; // "GMPS"
constexpr std::uint32_t kIndexMagic = 0x49504d47;  // "GMPI"
constexpr std::uint32_t kIndexFormat = 1;
// Bumped when the file or record layout changes.
constexpr std::uint32_t kFormatVersion = 1;
// Records of shaders that a mod update re-keyed are never read again; past this size the file starts over.
constexpr std::uint64_t kMaxFileBytes = 1ull << 30;

struct FileHeader {
    std::uint32_t magic;
    std::uint32_t format;
    std::uint32_t translator;
    std::uint32_t reserved;
};

struct RecordHeader {
    std::uint32_t magic;
    std::uint32_t bytes;
    std::uint64_t key;
    std::uint64_t checksum;
};

// shaders.idx lists the records of shaders.bin, so a launch reads one small file instead of a header in every record
// across the whole pack. Its stamp is the pack's (FileHeader::reserved, random for each new pack), so an index left from
// an earlier pack is not used.
struct IndexHeader {
    std::uint32_t magic;
    std::uint32_t format;
    std::uint32_t stamp;
    std::uint32_t reserved;
};

struct IndexEntry {
    std::uint64_t key;
    std::uint64_t offset;
    std::uint32_t bytes;
    std::uint32_t reserved;
    std::uint64_t checksum;
};
static_assert(sizeof(FileHeader) == 16 && sizeof(RecordHeader) == 24 && sizeof(IndexHeader) == 16 && sizeof(IndexEntry) == 32);

std::uint64_t Fnv(const void* data, size_t size, std::uint64_t hash = 14695981039346656037ull)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::filesystem::path CacheRoot() { return ModDirectory() / L"cache"; }

// Before the single file, every translation was a file of its own in cache\shaders (hundreds of thousands after the
// prewarm that compiled every combo of the game's shaders), and that prewarm's failures went to
// cache\prewarm_failures. Both are deleted once, in the background.
void RemoveLegacyCache()
{
    std::vector<std::filesystem::path> legacy;
    for (const wchar_t* name : { L"shaders", L"prewarm_failures" }) {
        std::error_code error;
        if (std::filesystem::is_directory(CacheRoot() / name, error))
            legacy.push_back(CacheRoot() / name);
    }
    if (legacy.empty())
        return;
    // Detached: the process may exit before it finishes, and the next launch carries on.
    PinThisModule();
    std::thread([legacy = std::move(legacy)] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        for (const std::filesystem::path& directory : legacy) {
            std::error_code error;
            const std::uintmax_t removed = std::filesystem::remove_all(directory, error);
            if (error)
                log::Info("Shader cache: removing the old cache {} failed after {} files: {}", directory.string(), removed, error.message());
            else
                log::Info("Shader cache: removed the old cache {} ({} files)", directory.string(), removed);
        }
    }).detach();
}

class Writer {
public:
    template <class T>
    void Put(const T& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        m_bytes.insert(m_bytes.end(), bytes, bytes + sizeof(T));
    }
    void Put(std::span<const unsigned char> bytes) { m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end()); }
    std::vector<unsigned char>& Bytes() { return m_bytes; }

private:
    std::vector<unsigned char> m_bytes;
};

class Reader {
public:
    explicit Reader(std::span<const unsigned char> bytes) : m_bytes(bytes) {}
    template <class T>
    bool Get(T& value)
    {
        if (m_bytes.size() - m_at < sizeof(T))
            return false;
        std::memcpy(&value, m_bytes.data() + m_at, sizeof(T));
        m_at += sizeof(T);
        return true;
    }
    bool Get(void* data, size_t size)
    {
        if (m_bytes.size() - m_at < size)
            return false;
        std::memcpy(data, m_bytes.data() + m_at, size);
        m_at += size;
        return true;
    }
    std::span<const unsigned char> Rest() const { return m_bytes.subspan(m_at); }

private:
    std::span<const unsigned char> m_bytes;
    size_t m_at = 0;
};

// A record's contents: the translation's interface, then the DXIL to the record's end.
std::vector<unsigned char> Serialize(const translator::Translation& info, std::span<const unsigned char> dxil)
{
    Writer writer;
    writer.Put(static_cast<std::uint32_t>(info.stage));
    writer.Put(static_cast<std::uint32_t>(info.firstFloatConstant));
    writer.Put(static_cast<std::uint32_t>(info.floatConstantCount));
    writer.Put(static_cast<std::uint32_t>(info.colorOutputs));
    writer.Put(static_cast<std::uint32_t>(info.writesDepth ? 1 : 0));
    writer.Put(info.samplers);
    writer.Put(static_cast<std::uint32_t>(info.inputs.size()));
    for (const translator::VertexInput& input : info.inputs)
        writer.Put(input);
    writer.Put(dxil);
    return std::move(writer.Bytes());
}

bool Deserialize(std::span<const unsigned char> payload, translator::Translation& info, std::vector<unsigned char>& dxil)
{
    Reader reader(payload);
    std::uint32_t stage = 0, first = 0, count = 0, outputs = 0, depth = 0, inputs = 0;
    if (!reader.Get(stage) || !reader.Get(first) || !reader.Get(count) || !reader.Get(outputs) || !reader.Get(depth) || !reader.Get(info.samplers)
        || !reader.Get(inputs) || stage > static_cast<std::uint32_t>(translator::Stage::Pixel))
        return false;
    info.stage = static_cast<translator::Stage>(stage);
    info.firstFloatConstant = first;
    info.floatConstantCount = count;
    info.colorOutputs = outputs;
    info.writesDepth = depth != 0;
    if (inputs > reader.Rest().size() / sizeof(translator::VertexInput))
        return false;
    info.inputs.resize(inputs);
    if (!reader.Get(info.inputs.data(), inputs * sizeof(translator::VertexInput)))
        return false;
    dxil.assign(reader.Rest().begin(), reader.Rest().end());
    return !dxil.empty();
}

class PackFile {
public:
    bool Contains(std::uint64_t key)
    {
        std::scoped_lock lock(m_mutex);
        Open();
        return m_index.contains(key);
    }

    bool Writable()
    {
        std::scoped_lock lock(m_mutex);
        Open();
        return m_writable;
    }

    bool Find(std::uint64_t key, std::vector<unsigned char>& payload)
    {
        std::scoped_lock lock(m_mutex);
        Open();
        const auto found = m_index.find(key);
        if (found == m_index.end())
            return false;
        const Entry entry = found->second;
        payload.resize(entry.bytes);
        m_file.seekg(static_cast<std::streamoff>(entry.offset));
        m_file.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!m_file || Fnv(payload.data(), payload.size()) != entry.checksum) {
            // Recompiled and appended again.
            m_file.clear();
            m_index.erase(found);
            return false;
        }
        return true;
    }

    void Append(std::uint64_t key, std::span<const unsigned char> payload)
    {
        std::scoped_lock lock(m_mutex);
        Open();
        if (!m_writable || m_index.contains(key))
            return;
        const RecordHeader record = { kRecordMagic, static_cast<std::uint32_t>(payload.size()), key, Fnv(payload.data(), payload.size()) };
        m_file.seekp(static_cast<std::streamoff>(m_end));
        m_file.write(reinterpret_cast<const char*>(&record), sizeof(record));
        m_file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        m_file.flush();
        if (!m_file) {
            // A torn record is dropped by the next launch's scan.
            m_file.clear();
            m_writable = false;
            log::Info("Shader cache: writing {} failed; compiled shaders are no longer saved this session", Path().string());
            return;
        }
        const Entry entry = { m_end + sizeof(record), record.bytes, record.checksum };
        m_index[key] = entry;
        m_end += sizeof(record) + payload.size();
        AppendIndex(key, entry);
    }

private:
    struct Entry {
        std::uint64_t offset;
        std::uint32_t bytes;
        std::uint64_t checksum;
    };

    static std::filesystem::path Path() { return CacheRoot() / L"shaders.bin"; }
    static std::filesystem::path IndexPath() { return CacheRoot() / L"shaders.idx"; }

    // Reads the pack's header; returns its stamp, or nothing when the file is missing, from another version or too large.
    static std::optional<std::uint32_t> ReadHeader(std::uint64_t size)
    {
        if (size < sizeof(FileHeader))
            return std::nullopt;
        if (size > kMaxFileBytes) {
            log::Info("Shader cache: {} has grown to {} MB; starting over", Path().string(), size >> 20);
            return std::nullopt;
        }
        std::ifstream file(Path(), std::ios::binary);
        FileHeader header = {};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file || header.magic != kFileMagic || header.format != kFormatVersion || header.translator != kTranslatorVersion)
            return std::nullopt;
        return header.reserved;
    }

    // Fills the record index from shaders.idx; returns the end of the last record it lists, or 0 when the index is
    // missing, from another pack or lists records past the pack's end.
    std::uint64_t ReadIndex(std::uint32_t stamp, std::uint64_t size)
    {
        std::ifstream file(IndexPath(), std::ios::binary | std::ios::ate);
        if (!file)
            return 0;
        const std::uint64_t bytes = static_cast<std::uint64_t>(file.tellg());
        IndexHeader header = {};
        file.seekg(0);
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file || header.magic != kIndexMagic || header.format != kIndexFormat || header.stamp != stamp)
            return 0;
        // A torn last entry is left out, and cut off when the index is next written.
        std::vector<IndexEntry> entries((bytes - sizeof(header)) / sizeof(IndexEntry));
        file.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entries.size() * sizeof(IndexEntry)));
        if (!file)
            return 0;
        std::uint64_t end = sizeof(FileHeader);
        for (const IndexEntry& entry : entries) {
            if (entry.offset < sizeof(FileHeader) + sizeof(RecordHeader) || entry.offset > size || entry.bytes > size - entry.offset) {
                m_index.clear();
                return 0;
            }
            m_index[entry.key] = { entry.offset, entry.bytes, entry.checksum };
            end = std::max(end, entry.offset + entry.bytes);
        }
        m_indexEntries = entries.size();
        return end;
    }

    // Reads the record index from every record's header, for a pack without a usable shaders.idx; returns the length of
    // its valid part.
    std::uint64_t ScanPack(std::uint64_t size)
    {
        std::ifstream file(Path(), std::ios::binary);
        std::uint64_t at = sizeof(FileHeader);
        while (size - at >= sizeof(RecordHeader)) {
            RecordHeader record = {};
            file.seekg(static_cast<std::streamoff>(at));
            file.read(reinterpret_cast<char*>(&record), sizeof(record));
            if (!file || record.magic != kRecordMagic || record.bytes > size - at - sizeof(RecordHeader))
                break;
            // A key stored again (after a corrupt record of it) is read from its latest record.
            m_index[record.key] = { at + sizeof(RecordHeader), record.bytes, record.checksum };
            at += sizeof(RecordHeader) + record.bytes;
        }
        if (at < size)
            log::Info("Shader cache: dropping {} bytes after the last whole record of {}", size - at, Path().string());
        return at;
    }

    // Writes shaders.idx for every record in m_index, beside it and renamed into place, and opens it for appending.
    void WriteIndex()
    {
        std::filesystem::path temporary = IndexPath();
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            const IndexHeader header = { kIndexMagic, kIndexFormat, m_stamp, 0 };
            file.write(reinterpret_cast<const char*>(&header), sizeof(header));
            for (const auto& [key, entry] : m_index) {
                const IndexEntry written = { key, entry.offset, entry.bytes, 0, entry.checksum };
                file.write(reinterpret_cast<const char*>(&written), sizeof(written));
            }
            if (!file) {
                log::Info("Shader cache: writing {} failed; the next launch reads every record of {}", temporary.string(), Path().string());
                return;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, IndexPath(), error);
        if (error) {
            log::Info("Shader cache: replacing {} failed ({}); the next launch reads every record of {}", IndexPath().string(), error.message(),
                Path().string());
            std::filesystem::remove(temporary, error);
            return;
        }
        m_indexEntries = m_index.size();
        OpenIndexForAppend();
    }

    void OpenIndexForAppend()
    {
        m_indexFile.open(IndexPath(), std::ios::binary | std::ios::in | std::ios::out);
        if (m_indexFile)
            m_indexFile.seekp(static_cast<std::streamoff>(sizeof(IndexHeader) + m_indexEntries * sizeof(IndexEntry)));
    }

    // Under m_mutex, after the record was written.
    void AppendIndex(std::uint64_t key, const Entry& entry)
    {
        if (!m_indexFile.is_open())
            return;
        const IndexEntry written = { key, entry.offset, entry.bytes, 0, entry.checksum };
        m_indexFile.write(reinterpret_cast<const char*>(&written), sizeof(written));
        m_indexFile.flush();
        if (!m_indexFile) {
            // Without the entry the index no longer matches the pack: the next launch reads every record instead.
            m_indexFile.close();
            std::error_code error;
            std::filesystem::remove(IndexPath(), error);
            log::Info("Shader cache: writing {} failed; the next launch reads every record of {}", IndexPath().string(), Path().string());
            return;
        }
        ++m_indexEntries;
    }

    // Under m_mutex.
    void Open()
    {
        if (m_opened)
            return;
        m_opened = true;
        const auto start = std::chrono::steady_clock::now();
        std::error_code error;
        std::filesystem::create_directories(CacheRoot(), error);
        RemoveLegacyCache();
        const std::uint64_t size = std::filesystem::file_size(Path(), error);
        const std::optional<std::uint32_t> stamp = error ? std::nullopt : ReadHeader(size);
        std::uint64_t valid = 0;
        bool indexed = false;
        if (stamp) {
            m_stamp = *stamp;
            valid = ReadIndex(m_stamp, size);
            indexed = valid != 0;
            if (!indexed)
                valid = ScanPack(size);
        }
        if (valid == 0) {
            m_index.clear();
            // A new pack's stamp tells its index from an earlier pack's.
            m_stamp = static_cast<std::uint32_t>(std::random_device()()) | 1u;
            std::ofstream create(Path(), std::ios::binary | std::ios::trunc);
            const FileHeader header = { kFileMagic, kFormatVersion, kTranslatorVersion, m_stamp };
            create.write(reinterpret_cast<const char*>(&header), sizeof(header));
            if (!create) {
                log::Info("Shader cache: creating {} failed; compiled shaders are not saved", Path().string());
                return;
            }
            valid = sizeof(header);
            indexed = false;
        } else if (valid < size) {
            // A torn record, or records the index does not list (their index entry was never written), end the pack.
            std::filesystem::resize_file(Path(), valid, error);
            if (error) {
                log::Info("Shader cache: truncating {} failed ({}); compiled shaders are not saved", Path().string(), error.message());
                m_file.open(Path(), std::ios::binary | std::ios::in);
                return;
            }
        }
        m_file.open(Path(), std::ios::binary | std::ios::in | std::ios::out);
        m_writable = m_file.is_open();
        m_end = valid;
        if (indexed)
            OpenIndexForAppend();
        else
            WriteIndex();
        log::Info("Shader cache: {} compiled shaders in {} ({} in {:.0f} ms)", m_index.size(), Path().string(),
            indexed ? "index read" : "records scanned", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }

    std::mutex m_mutex;
    bool m_opened = false;
    bool m_writable = false;
    std::fstream m_file;
    std::fstream m_indexFile;
    std::size_t m_indexEntries = 0;
    std::uint32_t m_stamp = 0;
    std::unordered_map<std::uint64_t, Entry> m_index;
    std::uint64_t m_end = 0;
};

PackFile& Pack()
{
    static PackFile pack;
    return pack;
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
    if (variant.opacityMicromaps != 0)
        key = Fnv("micromaps", 9, key);
    return Fnv(&kTranslatorVersion, sizeof(kTranslatorVersion), key);
}

bool Load(std::uint64_t key, translator::Translation& info, std::vector<unsigned char>& dxil)
{
    std::vector<unsigned char> payload;
    return Pack().Find(key, payload) && Deserialize(payload, info, dxil);
}

void Store(std::uint64_t key, const translator::Translation& info, std::span<const unsigned char> dxil)
{
    Pack().Append(key, Serialize(info, dxil));
}

bool Contains(std::uint64_t key) { return Pack().Contains(key); }

bool Writable() { return Pack().Writable(); }

void OpenInBackground()
{
    static std::once_flag once;
    std::call_once(once, [] {
        // Detached: the pack outlives it, and a thread that asks for a shader meanwhile waits for the pack's mutex.
        PinThisModule();
        std::thread([] { Pack().Writable(); }).detach();
    });
}

std::filesystem::path DumpDirectory() { return CacheRoot() / L"shader_dumps"; }

}
