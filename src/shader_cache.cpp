#include "shader_cache.h"

#include "command_line.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gmodpp::shader_cache {

namespace {

constexpr std::uint32_t kFileMagic = 0x43504d47;   // "GMPC"
constexpr std::uint32_t kRecordMagic = 0x53504d47; // "GMPS"
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
static_assert(sizeof(FileHeader) == 16 && sizeof(RecordHeader) == 24);

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
        m_index[key] = { m_end + sizeof(record), record.bytes, record.checksum };
        m_end += sizeof(record) + payload.size();
    }

private:
    struct Entry {
        std::uint64_t offset;
        std::uint32_t bytes;
        std::uint64_t checksum;
    };

    static std::filesystem::path Path() { return CacheRoot() / L"shaders.bin"; }

    // Reads the record index of an existing file; returns the length of its valid part, or 0 when it is missing, from
    // another version or too large.
    std::uint64_t Scan()
    {
        std::error_code error;
        const std::uint64_t size = std::filesystem::file_size(Path(), error);
        if (error || size < sizeof(FileHeader))
            return 0;
        if (size > kMaxFileBytes) {
            log::Info("Shader cache: {} has grown to {} MB; starting over", Path().string(), size >> 20);
            return 0;
        }
        std::ifstream file(Path(), std::ios::binary);
        FileHeader header = {};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file || header.magic != kFileMagic || header.format != kFormatVersion || header.translator != kTranslatorVersion)
            return 0;
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

    // Under m_mutex.
    void Open()
    {
        if (m_opened)
            return;
        m_opened = true;
        std::error_code error;
        std::filesystem::create_directories(CacheRoot(), error);
        RemoveLegacyCache();
        std::uint64_t valid = Scan();
        if (valid == 0) {
            m_index.clear();
            std::ofstream create(Path(), std::ios::binary | std::ios::trunc);
            const FileHeader header = { kFileMagic, kFormatVersion, kTranslatorVersion, 0 };
            create.write(reinterpret_cast<const char*>(&header), sizeof(header));
            if (!create) {
                log::Info("Shader cache: creating {} failed; compiled shaders are not saved", Path().string());
                return;
            }
            valid = sizeof(header);
        } else if (valid < std::filesystem::file_size(Path(), error)) {
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
        log::Info("Shader cache: {} compiled shaders in {}", m_index.size(), Path().string());
    }

    std::mutex m_mutex;
    bool m_opened = false;
    bool m_writable = false;
    std::fstream m_file;
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

std::filesystem::path DumpDirectory() { return CacheRoot() / L"shader_dumps"; }

}
