#include "shader_precache.h"

#include "command_line.h"
#include "log.h"
#include "shader_cache.h"
#include "shader_compiler.h"
#include "shader_twins.h"
#include "vpk.h"

#include <windows.h>
#include <d3d9types.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_set>

namespace gmodpp::shader_precache {

namespace {

// The keys being compiled, those that failed, and the files with a failure logged.
struct InFlight {
    std::mutex mutex;
    std::condition_variable finished;
    std::unordered_set<std::uint64_t> building;
    std::unordered_set<std::uint64_t> failed;
    std::unordered_set<std::string> loggedFiles;
};

InFlight& Flight()
{
    static InFlight flight;
    return flight;
}

const ShaderCompiler* Compiler(std::string& error)
{
    static std::string loadError;
    static const std::optional<ShaderCompiler> compiler = [] {
        ShaderCompiler loaded;
        if (!loaded.Load(ModDirectory(), loadError))
            return std::optional<ShaderCompiler>();
        return std::optional(std::move(loaded));
    }();
    if (!compiler)
        error = loadError;
    return compiler ? &*compiler : nullptr;
}

void Dump(const std::filesystem::path& path, std::string_view text)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

// A file's combos usually fail alike, and precaching tries them all: only its first failure is logged and dumped.
bool FirstFailureOf(const char* name)
{
    InFlight& flight = Flight();
    std::scoped_lock lock(flight.mutex);
    return flight.loggedFiles.insert(name).second;
}

bool Compile(std::span<const DWORD> code, const translator::Variant& variant, const char* name, std::uint64_t key, translator::Translation& info,
    std::vector<unsigned char>& dxil)
{
    std::string error;
    if (!translator::Translate(code, variant, info, error)) {
        if (FirstFailureOf(name))
            log::Info("Shaders: translating a combo of {} failed (later failures of the file are not logged): {}", name, error);
        return false;
    }
    const ShaderCompiler* compiler = Compiler(error);
    if (compiler == nullptr || !compiler->Compile(info.hlsl, translator::Profile(info.stage, variant), dxil, error)) {
        if (FirstFailureOf(name)) {
            log::Info("Shaders: compiling a combo of {} failed (later failures of the file are not logged): {}", name,
                std::string_view(error).substr(0, error.find('\n')));
            Dump(shader_cache::DumpDirectory() / std::format("{:016x}.failed.hlsl", key), info.hlsl + "\n/*\n" + error + "\n*/\n");
        }
        info.hlsl.clear();
        return false;
    }
    shader_cache::Store(key, info, dxil);
    static const bool developer = HasArgument(L"-gmodpp_dev");
    if (developer)
        Dump(shader_cache::DumpDirectory() / std::format("{:016x}.{}.hlsl", key, name), info.hlsl);
    info.hlsl.clear();
    return true;
}

// The variant a vertex shader takes for compressed vertices (shaderapi_meshes.cpp's BuildVertexElements): the normal
// and the tangent packed in it as UBYTE4, the bone weights as SHORT2.
std::optional<translator::Variant> CompressedVariant(const translator::Translation& translation)
{
    translator::Variant variant;
    bool normal = false;
    for (const translator::VertexInput& input : translation.inputs) {
        if (input.usageIndex != 0)
            continue;
        const std::uint16_t bit = static_cast<std::uint16_t>(1u << input.reg);
        if (input.usage == D3DDECLUSAGE_NORMAL) {
            normal = true;
            variant.integerInputs |= bit;
        } else if (input.usage == D3DDECLUSAGE_TANGENT) {
            variant.integerInputs |= bit;
        } else if (input.usage == D3DDECLUSAGE_BLENDWEIGHT) {
            variant.integerInputs |= bit;
            variant.signedInputs |= bit;
        }
    }
    return normal ? std::optional(variant) : std::nullopt;
}

// Build for workers, which keep nothing: a shader the cache holds is not read.
void Ensure(std::span<const DWORD> code, const translator::Variant& variant, const char* name)
{
    if (shader_cache::Contains(shader_cache::Key(code, variant)))
        return;
    translator::Translation info;
    std::vector<unsigned char> dxil;
    Build(code, variant, name, info, dxil);
}

// What DrawPass (shaderapi_pipelines.cpp) compiles for the request's combos, as far as the snapshot tells.
void Run(const Request& request)
{
    const vcs::Header& header = request.directory->header;
    const std::uint32_t dynamicCombos = static_cast<std::uint32_t>(std::max(header.dynamicCombos, 1));
    const std::uint32_t staticCombo = static_cast<std::uint32_t>(request.staticIndex) / dynamicCombos;
    std::vector<vcs::Combo> combos;
    std::string error;
    if (!vcs::ReadStaticCombo(request.file, *request.directory, staticCombo, combos, error)) {
        log::Info("Shaders: {}: {}", request.name, error);
        return;
    }
    const bool lightWarp = request.gbufferTwin && twins::LightWarpSampler(request.name, header, request.staticIndex) >= 0;
    std::unordered_set<std::string_view> seen;
    std::unordered_set<const DWORD*> seenTwins;
    for (const vcs::Combo& combo : combos) {
        const std::string_view bytes(reinterpret_cast<const char*>(combo.bytecode.data()), combo.bytecode.size() * sizeof(DWORD));
        if (seen.insert(bytes).second) {
            translator::Variant variant;
            const bool vertex = !combo.bytecode.empty() && (combo.bytecode[0] & 0xFFFF0000u) == 0xFFFE0000u;
            if (vertex) {
                Ensure(combo.bytecode, variant, request.name);
                // The compressed variant depends on the inputs the translation lists; translating without compiling is
                // cheap.
                translator::Translation base;
                if (translator::Translate(combo.bytecode, variant, base, error)) {
                    if (const std::optional<translator::Variant> compressed = CompressedVariant(base))
                        Ensure(combo.bytecode, *compressed, request.name);
                }
            } else {
                variant.alphaTest = request.alphaTest;
                Ensure(combo.bytecode, variant, request.name);
            }
        }
        for (const bool forward : { false, true }) {
            if (!(forward ? request.forwardTwin : request.gbufferTwin))
                continue;
            const int dynamicIndex = static_cast<int>(combo.dynamicCombo);
            const std::span<const DWORD> twin = forward ? twins::ForwardShader(request.name, header, request.staticIndex, dynamicIndex)
                                                        : twins::GBufferShader(request.name, header, request.staticIndex, dynamicIndex);
            if (twin.empty() || !seenTwins.insert(twin.data()).second)
                continue;
            translator::Variant variant;
            variant.alphaTest = request.alphaTest;
            variant.forwardLighting = forward ? 1 : 0;
            variant.opacityMicromaps = forward && request.opacityMicromaps ? 1 : 0;
            variant.lightWarpSlot = !forward && lightWarp ? 1 : 0;
            Ensure(twin, variant, request.name);
        }
    }
}

// Requests run on worker threads below normal priority, one fewer than the logical cores so the game's main thread keeps
// one. The pool lives as long as the process; its threads are detached, as the process ends them at exit.
class Pool {
public:
    // `counted` requests are waited for by the loading screen (Status).
    void Enqueue(const Request& request, bool counted)
    {
        {
            std::scoped_lock lock(m_mutex);
            const auto id = std::tuple(static_cast<const void*>(request.file.data()), request.staticIndex, request.alphaTest, request.gbufferTwin,
                request.forwardTwin, request.opacityMicromaps);
            if (!m_seen.insert(id).second)
                return;
            m_queue.push_back({ request, counted });
            m_total += counted ? 1 : 0;
            if (!m_started) {
                m_started = true;
                PinThisModule();
                const unsigned threads = std::max(2u, std::thread::hardware_concurrency()) - 1;
                for (unsigned i = 0; i < threads; ++i)
                    std::thread([this] { Work(); }).detach();
                log::Info("Shaders: compiling the shaders of materials on {} threads", threads);
            }
        }
        m_ready.notify_one();
    }

    Progress Status()
    {
        std::scoped_lock lock(m_mutex);
        return { m_done, m_total };
    }

private:
    void Work()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            Queued queued;
            {
                std::unique_lock lock(m_mutex);
                m_ready.wait(lock, [&] { return !m_queue.empty(); });
                queued = m_queue.front();
                m_queue.pop_front();
            }
            Run(queued.request);
            std::scoped_lock lock(m_mutex);
            m_done += queued.counted ? 1 : 0;
        }
    }

    struct Queued {
        Request request;
        bool counted = true;
    };

    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Queued> m_queue;
    std::set<std::tuple<const void*, std::int32_t, std::uint8_t, bool, bool, bool>> m_seen;
    std::size_t m_total = 0;
    std::size_t m_done = 0;
    bool m_started = false;
};

Pool& ThePool()
{
    static Pool* pool = new Pool;
    return *pool;
}

}

bool Build(std::span<const DWORD> code, const translator::Variant& variant, const char* name, translator::Translation& info,
    std::vector<unsigned char>& dxil)
{
    const std::uint64_t key = shader_cache::Key(code, variant);
    InFlight& flight = Flight();
    for (;;) {
        if (shader_cache::Load(key, info, dxil))
            return true;
        std::unique_lock lock(flight.mutex);
        if (flight.failed.contains(key))
            return false;
        if (flight.building.insert(key).second)
            break;
        flight.finished.wait(lock, [&] { return !flight.building.contains(key); });
    }
    // This thread compiles the key. Another may have stored it between the cache miss and the claim; and whatever
    // happens, the claim is released so no waiter is left behind.
    bool built = false;
    struct Release {
        InFlight& flight;
        std::uint64_t key;
        const bool& built;
        ~Release()
        {
            {
                std::scoped_lock lock(flight.mutex);
                flight.building.erase(key);
                if (!built)
                    flight.failed.insert(key);
            }
            flight.finished.notify_all();
        }
    } release{ flight, key, built };
    built = shader_cache::Load(key, info, dxil) || Compile(code, variant, name, key, info, dxil);
    return built;
}

namespace {

void EnqueueRequest(const Request& request, bool counted)
{
    if (request.name == nullptr || request.directory == nullptr || request.file.empty() || request.staticIndex < 0)
        return;
    // Without a cache file what workers compile would be lost, and draws would compile it again.
    static const bool writable = shader_cache::Writable();
    if (!writable)
        return;
    ThePool().Enqueue(request, counted);
}

}

void Enqueue(const Request& request) { EnqueueRequest(request, true); }

void EnqueueEveryShader(const std::filesystem::path& gameDirectory)
{
    struct File {
        std::string name;
        std::vector<std::uint8_t> bytes;
        vcs::Directory directory;
    };
    // Requests point into these for the life of the process.
    static std::mutex mutex;
    static std::deque<File> files;
    std::scoped_lock lock(mutex);
    if (!files.empty())
        return;
    constexpr std::string_view kShaderDirectory = "shaders/fxc/";
    Vpk vpk;
    std::string error;
    if (!vpk.Open(gameDirectory / L"garrysmod" / L"garrysmod_dir.vpk", error)) {
        log::Info("Shaders: {}", error);
        return;
    }
    size_t requests = 0;
    for (const Vpk::Entry& entry : vpk.Entries()) {
        if (!entry.path.starts_with(kShaderDirectory) || !entry.path.ends_with(".vcs"))
            continue;
        std::optional<std::vector<std::uint8_t>> bytes = vpk.Read(entry, error);
        File file{ entry.path.substr(kShaderDirectory.size()), bytes ? std::move(*bytes) : std::vector<std::uint8_t>(), {} };
        if (!bytes || !vcs::ReadDirectory(file.bytes, file.directory, error)) {
            log::Info("Shaders: {}: {}", entry.path, error);
            continue;
        }
        File& stored = files.emplace_back(std::move(file));
        const std::int32_t dynamicCombos = std::max(stored.directory.header.dynamicCombos, 1);
        for (const vcs::Directory::Record& record : stored.directory.records) {
            if (record.staticCombo == 0xffffffff || record.staticCombo > static_cast<std::uint32_t>(INT32_MAX / dynamicCombos))
                continue;
            Request request;
            request.name = stored.name.c_str();
            request.file = stored.bytes;
            request.directory = &stored.directory;
            request.staticIndex = static_cast<std::int32_t>(record.staticCombo) * dynamicCombos;
            // Files without twins (vertex shaders among them) have none to compile.
            request.gbufferTwin = true;
            request.forwardTwin = true;
            EnqueueRequest(request, false);
            ++requests;
        }
    }
    log::Info("Shaders: queued all {} static combos of the game's {} shader files", requests, files.size());
}

Progress Status() { return ThePool().Status(); }

}
