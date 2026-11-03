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
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
                variant.clipPlanes = request.clipPlanes & 7u;
                Ensure(combo.bytecode, variant, request.name);
                // The compressed variant depends on the inputs the translation lists; translating without compiling is
                // cheap.
                translator::Translation base;
                if (translator::Translate(combo.bytecode, variant, base, error)) {
                    if (std::optional<translator::Variant> compressed = CompressedVariant(base)) {
                        compressed->clipPlanes = variant.clipPlanes;
                        Ensure(combo.bytecode, *compressed, request.name);
                    }
                }
            } else if (request.clipPlanes == 0) {
                variant.alphaTest = request.alphaTest;
                Ensure(combo.bytecode, variant, request.name);
                // Particles drawn after the capture are fogged by the volumetric light (capture::Prepare).
                if (request.forwardTwin && twins::IsParticleShader(request.name)) {
                    translator::Variant fogged = variant;
                    fogged.fogEpilogue = 1;
                    Ensure(combo.bytecode, fogged, request.name);
                }
                // DrawPass clears the coverage only with a single colour output, which the translation lists.
                translator::Translation base;
                if (request.clearCoverage && translator::Translate(combo.bytecode, variant, base, error) && base.colorOutputs == 1) {
                    variant.clearCoverage = 1;
                    variant.markMirror = std::string_view(request.name).starts_with("lightmappedreflective_ps") ? 1 : 0;
                    Ensure(combo.bytecode, variant, request.name);
                }
            }
        }
        for (const bool forward : { false, true }) {
            if (!(forward ? request.forwardTwin : request.gbufferTwin) || request.clipPlanes != 0)
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

// A request's identity: the same pass of the same file is compiled once, whichever buffer holds the file (a map's
// materials read it through the game's filesystem, StartReplay from the game's folder). A copy with other contents (an
// addon's) is another request, so a replay of one copy does not stand in for the copy the game draws with.
using RequestId = std::tuple<std::string, std::uint64_t, std::int32_t, std::uint8_t, std::uint8_t, bool, bool, bool, bool>;

std::string LowerName(const Request& request)
{
    std::string name = request.name;
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

RequestId IdOf(const Request& request)
{
    return { LowerName(request), request.fileHash, request.staticIndex, request.clipPlanes, request.alphaTest, request.gbufferTwin, request.forwardTwin,
        request.clearCoverage, request.opacityMicromaps };
}

// Requests run on worker threads below normal priority, one fewer than the logical cores so the game's main thread keeps
// one, taking the most urgent priority first. The pool lives as long as the process; its threads are detached, as the
// process ends them at exit.
class Pool {
public:
    void Enqueue(const Request& request, Priority priority)
    {
        {
            std::scoped_lock lock(m_mutex);
            RequestId id = IdOf(request);
            const size_t p = static_cast<size_t>(priority);
            if (m_started.contains(id)) {
                // Already compiling at a lower priority (StartReplay's): counted here too, done when that run ends.
                if (const auto running = m_inProgress.find(id); running != m_inProgress.end() && running->second.front() > priority) {
                    running->second.push_back(priority);
                    ++m_total[p];
                }
                return;
            }
            // A request still waiting at a lower priority is queued again at this one; the first copy taken runs.
            const auto queued = m_queuedAt.find(id);
            if (queued != m_queuedAt.end() && queued->second <= priority)
                return;
            m_queuedAt[id] = priority;
            m_queues[p].push_back({ request, std::move(id) });
            ++m_total[p];
            if (!m_running) {
                m_running = true;
                PinThisModule();
                const unsigned threads = std::max(2u, std::thread::hardware_concurrency()) - 1;
                m_backgroundLimit = std::max(1u, threads / 2);
                for (unsigned i = 0; i < threads; ++i)
                    std::thread([this] { Work(); }).detach();
                log::Info("Shaders: compiling the shaders of materials on {} threads", threads);
            }
        }
        m_ready.notify_one();
    }

    Progress Status(Priority priority)
    {
        std::scoped_lock lock(m_mutex);
        const size_t p = static_cast<size_t>(priority);
        return { m_done[p], m_total[p] };
    }

    // Calls `callback` on a worker once every request queued so far, up to Background, has finished (a Background
    // request may have been run by a more urgent copy); returns true instead when they all have already.
    bool OnBackgroundDone(std::function<void()> callback)
    {
        std::scoped_lock lock(m_mutex);
        if (AllDone())
            return true;
        m_backgroundDone = std::move(callback);
        return false;
    }

private:
    struct Queued {
        Request request;
        RequestId id;
    };

    void Work()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            Queued queued;
            size_t p = 0;
            bool run = false;
            {
                std::unique_lock lock(m_mutex);
                m_ready.wait(lock, [&] { return Takeable() < m_queues.size(); });
                p = Takeable();
                if (p == static_cast<size_t>(Priority::Background))
                    ++m_backgroundRunning;
                queued = std::move(m_queues[p].front());
                m_queues[p].pop_front();
                // A copy queued again at a more urgent priority may have run already.
                run = m_started.insert(queued.id).second;
                if (run) {
                    m_queuedAt.erase(queued.id);
                    m_inProgress[queued.id] = { static_cast<Priority>(p) };
                }
            }
            const bool background = p == static_cast<size_t>(Priority::Background);
            // Background requests keep the workers' priority: a draw may wait in Build for a shader one compiles.
            if (run)
                Run(queued.request);
            std::function<void()> callback;
            {
                std::scoped_lock lock(m_mutex);
                ++m_done[p];
                if (run) {
                    // The more urgent priorities that came to wait for this run while it went on.
                    const auto running = m_inProgress.find(queued.id);
                    for (size_t i = 1; i < running->second.size(); ++i)
                        ++m_done[static_cast<size_t>(running->second[i])];
                    m_inProgress.erase(running);
                }
                if (background) {
                    --m_backgroundRunning;
                    m_ready.notify_one();
                }
                if (m_backgroundDone && AllDone())
                    callback = std::exchange(m_backgroundDone, nullptr);
            }
            if (callback)
                callback();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::array<std::deque<Queued>, static_cast<size_t>(Priority::Count)> m_queues;
    std::map<RequestId, Priority> m_queuedAt;
    std::set<RequestId> m_started;
    // Requests running: the priority they run at, then the more urgent ones that wait for them.
    std::map<RequestId, std::vector<Priority>> m_inProgress;
    std::array<std::size_t, static_cast<size_t>(Priority::Count)> m_total = {};
    std::array<std::size_t, static_cast<size_t>(Priority::Count)> m_done = {};
    std::function<void()> m_backgroundDone;
    // Background requests run on at most half the workers, so a map's requests and the game keep the rest.
    unsigned m_backgroundLimit = 1;
    unsigned m_backgroundRunning = 0;
    bool m_running = false;

    // Whether every request of the priorities up to Background has finished. Developer requests never stand in for
    // those: a more urgent copy always runs first. Under m_mutex.
    bool AllDone() const
    {
        for (size_t p = 0; p <= static_cast<size_t>(Priority::Background); ++p) {
            if (m_done[p] != m_total[p])
                return false;
        }
        return true;
    }

    // The most urgent queue a worker may take from, or the queue count when none. Under m_mutex.
    size_t Takeable() const
    {
        for (size_t p = 0; p < m_queues.size(); ++p) {
            if (!m_queues[p].empty() && (p != static_cast<size_t>(Priority::Background) || m_backgroundRunning < m_backgroundLimit))
                return p;
        }
        return m_queues.size();
    }
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

std::uint64_t Fnv(const void* data, size_t size, std::uint64_t hash = 14695981039346656037ull)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

template <class T>
std::uint64_t FnvValue(const T& value, std::uint64_t hash)
{
    return Fnv(&value, sizeof(value), hash);
}

constexpr std::string_view kRememberedHeader = "gmodpp-precache 1";

// A remembered request's flags (RememberedRequests).
std::uint32_t FlagsOf(const Request& request)
{
    return request.alphaTest | (request.vertex ? 1u << 8 : 0u) | (static_cast<std::uint32_t>(request.clipPlanes & 7u) << 9)
        | (request.gbufferTwin ? 1u << 12 : 0u) | (request.forwardTwin ? 1u << 13 : 0u) | (request.clearCoverage ? 1u << 14 : 0u)
        | (request.opacityMicromaps ? 1u << 15 : 0u);
}

void ApplyFlags(std::uint32_t flags, Request& request)
{
    request.alphaTest = static_cast<std::uint8_t>(flags & 0xffu);
    request.vertex = (flags & 1u << 8) != 0;
    request.clipPlanes = static_cast<std::uint8_t>(flags >> 9 & 7u);
    request.gbufferTwin = (flags & 1u << 12) != 0;
    request.forwardTwin = (flags & 1u << 13) != 0;
    request.clearCoverage = (flags & 1u << 14) != 0;
    request.opacityMicromaps = (flags & 1u << 15) != 0;
}

// The requests of every session so far, in gmodplusplus\cache\precache.txt: a header line with the generation
// (ReplayGeneration) they were last all compiled for, then a line per request with its file, static index and flags.
// Thread-safe.
class RememberedRequests {
public:
    struct Line {
        std::string name;
        std::int32_t staticIndex = 0;
        std::uint32_t flags = 0;
    };

    void Add(const Request& request)
    {
        std::string name = LowerName(request);
        std::string line = std::format("{} {} {:x}", name, request.staticIndex, FlagsOf(request));
        std::scoped_lock lock(m_mutex);
        Load();
        if (!m_known.insert(line).second)
            return;
        m_lines.push_back({ std::move(name), request.staticIndex, FlagsOf(request) });
        if (m_append.is_open()) {
            m_append << line << '\n';
            if (++m_unflushed >= 256)
                FlushLocked();
        } else if (!m_writeFailed) {
            // After a failed write the lines stay in memory only: retrying would rewrite the whole file for every
            // request, on the render thread while a map loads.
            Rewrite();
        }
    }

    // Writes out the lines added since the last flush.
    void Flush()
    {
        std::scoped_lock lock(m_mutex);
        FlushLocked();
    }

    std::vector<Line> Lines(std::uint64_t& generation)
    {
        std::scoped_lock lock(m_mutex);
        Load();
        generation = m_generation;
        return m_lines;
    }

    void MarkCompiled(std::uint64_t generation)
    {
        std::scoped_lock lock(m_mutex);
        Load();
        if (m_generation == generation && m_append.is_open())
            return;
        m_generation = generation;
        Rewrite();
    }

private:
    static std::filesystem::path Path() { return ModDirectory() / L"cache" / L"precache.txt"; }

    // Under m_mutex.
    void FlushLocked()
    {
        if (m_append.is_open())
            m_append.flush();
        m_unflushed = 0;
    }

    // Under m_mutex.
    void Load()
    {
        if (std::exchange(m_loaded, true))
            return;
        std::ifstream read(Path(), std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(read)), std::istreambuf_iterator<char>());
        std::istringstream file(contents);
        std::string header;
        if (!std::getline(file, header) || !header.starts_with(kRememberedHeader))
            return;
        std::istringstream(header.substr(kRememberedHeader.size())) >> std::hex >> m_generation;
        // A file cut off in a line loses that line, and is written again whole by the next Add rather than appended to.
        const bool whole = contents.ends_with('\n');
        for (std::string text; std::getline(file, text);) {
            if (!whole && file.eof())
                break;
            Line line;
            std::istringstream fields(text);
            if (!(fields >> line.name >> std::dec >> line.staticIndex >> std::hex >> line.flags) || line.staticIndex < 0)
                continue;
            // Keyed as Add writes it, whatever line ending the file has.
            if (!m_known.insert(std::format("{} {} {:x}", line.name, line.staticIndex, line.flags)).second)
                continue;
            m_lines.push_back(std::move(line));
        }
        if (whole)
            m_append.open(Path(), std::ios::binary | std::ios::app);
    }

    // Writes the file again (beside it, renamed into place) and opens it for appending. Under m_mutex.
    void Rewrite()
    {
        m_append.close();
        m_writeFailed = true;
        std::error_code error;
        std::filesystem::create_directories(Path().parent_path(), error);
        std::filesystem::path temporary = Path();
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << kRememberedHeader << ' ' << std::format("{:016x}", m_generation) << '\n';
            for (const Line& line : m_lines)
                file << std::format("{} {} {:x}", line.name, line.staticIndex, line.flags) << '\n';
            if (!file) {
                log::Info("Shaders: writing {} failed; the shaders of maps loaded from now on are not remembered", temporary.string());
                return;
            }
        }
        std::filesystem::rename(temporary, Path(), error);
        if (error) {
            log::Info("Shaders: replacing {} failed ({}); the shaders of maps loaded from now on are not remembered", Path().string(), error.message());
            std::filesystem::remove(temporary, error);
            return;
        }
        m_append.open(Path(), std::ios::binary | std::ios::app);
        m_writeFailed = !m_append.is_open();
        if (m_writeFailed)
            log::Info("Shaders: opening {} failed; the shaders of maps loaded from now on are not remembered", Path().string());
    }

    std::mutex m_mutex;
    bool m_loaded = false;
    bool m_writeFailed = false;
    std::uint64_t m_generation = 0;
    std::vector<Line> m_lines;
    std::unordered_set<std::string> m_known;
    std::ofstream m_append;
    size_t m_unflushed = 0;
};

RememberedRequests& Remembered()
{
    static RememberedRequests* remembered = new RememberedRequests;
    return *remembered;
}

// What the remembered requests compile to: a new shader cache (its stamp), translator, lighting headers, twins or
// game shaders (the VPK holding them) each change it.
std::uint64_t ReplayGeneration(const std::filesystem::path& vpk)
{
    std::uint64_t hash = FnvValue(shader_cache::kTranslatorVersion, 14695981039346656037ull);
    hash = FnvValue(shader_cache::Stamp(), hash);
    hash = FnvValue(shader_cache::LightingHeadersHash(), hash);
    hash = FnvValue(twins::SourceHash(), hash);
    std::error_code error;
    const auto written = std::filesystem::last_write_time(vpk, error);
    hash = FnvValue(error ? 0 : written.time_since_epoch().count(), hash);
    hash = FnvValue(error ? 0 : std::filesystem::file_size(vpk, error), hash);
    return hash;
}

bool Writable()
{
    // Without a cache file what workers compile would be lost, and draws would compile it again.
    static const bool writable = shader_cache::Writable();
    return writable;
}

void EnqueueRequest(const Request& request, Priority priority)
{
    if (request.name == nullptr || request.directory == nullptr || request.file.empty() || request.staticIndex < 0 || !Writable())
        return;
    ThePool().Enqueue(request, priority);
}

// A shader file read by StartReplay or EnqueueEveryShader. Requests point into these for the life of the process.
struct LoadedFile {
    std::string name;
    std::vector<std::uint8_t> bytes;
    std::uint64_t hash = 0;
    vcs::Directory directory;
};

}

std::uint64_t HashFile(std::span<const std::uint8_t> bytes) { return Fnv(bytes.data(), bytes.size()); }

void Enqueue(const Request& request)
{
    if (request.name == nullptr || request.directory == nullptr || request.file.empty() || request.staticIndex < 0 || !Writable())
        return;
    EnqueueRequest(request, Priority::Map);
    Remembered().Add(request);
    if (request.vertex && request.clipPlanes == 0) {
        // The game clips the views of water's reflection and refraction with one user clip plane (UseFastClipping is
        // false), and every vertex shader drawn in them takes that variant.
        Request clipped = request;
        clipped.clipPlanes = 1;
        EnqueueRequest(clipped, Priority::Water);
        Remembered().Add(clipped);
    }
}

void StartReplay(const std::filesystem::path& gameDirectory)
{
    static std::once_flag once;
    std::call_once(once, [&] {
        PinThisModule();
        std::thread([gameDirectory] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            if (!Writable())
                return;
            const std::filesystem::path vpkPath = gameDirectory / L"garrysmod" / L"garrysmod_dir.vpk";
            const std::uint64_t generation = ReplayGeneration(vpkPath);
            std::uint64_t compiled = 0;
            const std::vector<RememberedRequests::Line> lines = Remembered().Lines(compiled);
            if (compiled == generation)
                return;
            if (lines.empty()) {
                Remembered().MarkCompiled(generation);
                return;
            }
            constexpr std::string_view kShaderDirectory = "shaders/fxc/";
            Vpk vpk;
            std::string error;
            if (!vpk.Open(vpkPath, error))
                log::Info("Shaders: {}; earlier sessions' shaders are read from the shaders folder alone", error);
            std::unordered_map<std::string, const Vpk::Entry*> entries;
            for (const Vpk::Entry& entry : vpk.Entries()) {
                if (entry.path.starts_with(kShaderDirectory) && entry.path.ends_with(".vcs"))
                    entries.emplace(entry.path.substr(kShaderDirectory.size(), entry.path.size() - kShaderDirectory.size() - 4), &entry);
            }
            static std::deque<LoadedFile> files;
            std::unordered_map<std::string, const LoadedFile*> loaded;
            auto file = [&](const std::string& name) -> const LoadedFile* {
                const auto found = loaded.find(name);
                if (found != loaded.end())
                    return found->second;
                LoadedFile read{ name, {}, 0, {} };
                // A loose file in the game's shaders folder, else the VPK's.
                std::ifstream loose(gameDirectory / L"garrysmod" / L"shaders" / L"fxc" / (name + ".vcs"), std::ios::binary);
                if (loose)
                    read.bytes.assign(std::istreambuf_iterator<char>(loose), std::istreambuf_iterator<char>());
                else if (const auto entry = entries.find(name); entry != entries.end())
                    read.bytes = vpk.Read(*entry->second, error).value_or(std::vector<std::uint8_t>());
                const LoadedFile* result = nullptr;
                if (!read.bytes.empty() && vcs::ReadDirectory(read.bytes, read.directory, error)) {
                    read.hash = HashFile(read.bytes);
                    result = &files.emplace_back(std::move(read));
                }
                loaded.emplace(name, result);
                return result;
            };
            size_t queued = 0;
            for (const RememberedRequests::Line& line : lines) {
                const LoadedFile* shader = file(line.name);
                if (shader == nullptr)
                    continue;
                Request request;
                request.name = shader->name.c_str();
                request.file = shader->bytes;
                request.fileHash = shader->hash;
                request.directory = &shader->directory;
                request.staticIndex = line.staticIndex;
                ApplyFlags(line.flags, request);
                EnqueueRequest(request, Priority::Background);
                ++queued;
            }
            log::Info("Shaders: compiling {} passes of {} shader files from earlier sessions in the background", queued, files.size());
            auto finished = [generation] {
                Remembered().MarkCompiled(generation);
                log::Info("Shaders: the shaders of earlier sessions are compiled");
            };
            if (ThePool().OnBackgroundDone(finished))
                finished();
        }).detach();
    });
}

void EnqueueEveryShader(const std::filesystem::path& gameDirectory)
{
    // Requests point into these for the life of the process.
    static std::mutex mutex;
    static std::deque<LoadedFile> files;
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
        LoadedFile file{ entry.path.substr(kShaderDirectory.size()), bytes ? std::move(*bytes) : std::vector<std::uint8_t>(), 0, {} };
        if (!bytes || !vcs::ReadDirectory(file.bytes, file.directory, error)) {
            log::Info("Shaders: {}: {}", entry.path, error);
            continue;
        }
        file.hash = HashFile(file.bytes);
        LoadedFile& stored = files.emplace_back(std::move(file));
        const std::int32_t dynamicCombos = std::max(stored.directory.header.dynamicCombos, 1);
        for (const vcs::Directory::Record& record : stored.directory.records) {
            if (record.staticCombo == 0xffffffff || record.staticCombo > static_cast<std::uint32_t>(INT32_MAX / dynamicCombos))
                continue;
            Request request;
            request.name = stored.name.c_str();
            request.file = stored.bytes;
            request.fileHash = stored.hash;
            request.directory = &stored.directory;
            request.staticIndex = static_cast<std::int32_t>(record.staticCombo) * dynamicCombos;
            // Files without twins (vertex shaders among them) have none to compile.
            request.gbufferTwin = true;
            request.forwardTwin = true;
            EnqueueRequest(request, Priority::Developer);
            ++requests;
        }
    }
    log::Info("Shaders: queued all {} static combos of the game's {} shader files", requests, files.size());
}

Progress Status(Priority priority) { return ThePool().Status(priority); }

void SaveRemembered() { Remembered().Flush(); }

}
