#include "shader_prewarm.h"

#include "log.h"
#include "shader_cache.h"
#include "shader_compiler.h"
#include "shader_translator.h"
#include "vcs.h"
#include "vpk.h"

#include <windows.h>
#include <d3d9types.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_set>

namespace gmodpp {

namespace {

constexpr std::string_view kShaderDirectory = "shaders/fxc/";
constexpr size_t kFilesAhead = 4;

std::filesystem::path VpkPath(const std::filesystem::path& gameDirectory)
{
    return gameDirectory / L"garrysmod" / L"garrysmod_dir.vpk";
}

std::filesystem::path MarkerPath(const std::filesystem::path& gameDirectory)
{
    return shader_cache::Directory(gameDirectory) / L"prewarm.txt";
}

// The translator version and the shader VPK's size and time, which a game update changes.
std::optional<std::string> MarkerText(const std::filesystem::path& gameDirectory)
{
    std::error_code error;
    const std::filesystem::path vpk = VpkPath(gameDirectory);
    const auto size = std::filesystem::file_size(vpk, error);
    if (error)
        return std::nullopt;
    const auto time = std::filesystem::last_write_time(vpk, error);
    if (error)
        return std::nullopt;
    return std::format("{} {} {}", shader_cache::kTranslatorVersion, size, time.time_since_epoch().count());
}

struct FileResult {
    std::atomic<size_t> translateFailures = 0;
    std::atomic<size_t> compileFailures = 0;
    std::mutex mutex;
    std::string firstError;
};

struct FileWork {
    std::string name;
    std::vector<vcs::Combo> combos;
    std::vector<const vcs::Combo*> work;
    size_t next = 0;
    std::atomic<size_t> remaining = 0;
    FileResult result;
};

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

std::vector<const vcs::Combo*> UniqueCombos(const std::vector<vcs::Combo>& combos)
{
    std::vector<const vcs::Combo*> unique;
    std::unordered_set<std::string_view> seen;
    for (const vcs::Combo& combo : combos) {
        std::string_view key(reinterpret_cast<const char*>(combo.bytecode.data()), combo.bytecode.size() * sizeof(DWORD));
        if (seen.insert(key).second)
            unique.push_back(&combo);
    }
    return unique;
}

}

bool PrewarmIsCurrent(const std::filesystem::path& gameDirectory)
{
    const std::optional<std::string> expected = MarkerText(gameDirectory);
    std::ifstream file(MarkerPath(gameDirectory));
    std::stringstream text;
    text << file.rdbuf();
    return expected && file && text.str() == *expected;
}

void PrewarmShaders(const std::filesystem::path& gameDirectory, const std::filesystem::path& compilerDirectory, PrewarmProgress& progress)
{
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    ShaderCompiler compiler;
    if (!compiler.Load(compilerDirectory, error)) {
        log::Info("Shader prewarm: {}", error);
        return;
    }
    Vpk vpk;
    if (!vpk.Open(VpkPath(gameDirectory), error)) {
        log::Info("Shader prewarm: {}", error);
        return;
    }
    const std::filesystem::path cache = shader_cache::Directory(gameDirectory);
    std::filesystem::create_directories(cache);
    const std::filesystem::path failureDirectory = gameDirectory / L"gmodpp_cache" / L"prewarm_failures";
    std::filesystem::remove_all(failureDirectory);
    std::filesystem::create_directories(failureDirectory);

    std::vector<const Vpk::Entry*> files;
    for (const Vpk::Entry& entry : vpk.Entries()) {
        if (entry.path.starts_with(kShaderDirectory) && entry.path.ends_with(".vcs"))
            files.push_back(&entry);
    }
    auto readCombos = [&](const Vpk::Entry& entry, std::vector<vcs::Combo>& combos) {
        std::string reason;
        auto bytes = vpk.Read(entry, reason);
        vcs::Header header;
        if (bytes && vcs::ReadCombos(*bytes, header, combos, reason))
            return true;
        log::Info("Shader prewarm: {}: {}", entry.path, reason);
        return false;
    };
    size_t total = 0;
    for (const Vpk::Entry* entry : files) {
        std::vector<vcs::Combo> combos;
        if (readCombos(*entry, combos))
            total += UniqueCombos(combos).size();
        if (progress.cancel)
            return;
    }
    progress.total = total;
    log::Info("Shader prewarm: {} files, {} unique combos (counted in {:.1f} s)", files.size(), total,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());

    // Every logical core compiles while a reader decodes the next files, so no core waits at the end of a file.
    // Below normal priority keeps the desktop responsive.
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<size_t> compiled = 0;
    std::atomic<size_t> translateFailures = 0, compileFailures = 0;
    auto build = [&](FileWork& file, const vcs::Combo& combo, const translator::Variant& variant, translator::Translation& translation, bool& available) {
        const std::uint64_t key = shader_cache::Key(combo.bytecode, variant);
        std::vector<unsigned char> dxil;
        if (shader_cache::Load(cache, key, translation, dxil)) {
            available = true;
            return;
        }
        FileResult& result = file.result;
        std::string reason;
        if (!translator::Translate(combo.bytecode, variant, translation, reason)) {
            ++result.translateFailures;
            std::scoped_lock lock(result.mutex);
            if (result.firstError.empty())
                result.firstError = std::format("translating combo {}/{}: {}", combo.staticCombo, combo.dynamicCombo, reason);
            return;
        }
        available = true;
        const wchar_t* profile = translation.stage == translator::Stage::Vertex ? L"vs_6_6" : L"ps_6_6";
        if (compiler.Compile(translation.hlsl, profile, dxil, reason)) {
            shader_cache::Store(cache, key, translation, dxil);
            ++compiled;
            return;
        }
        ++result.compileFailures;
        std::scoped_lock lock(result.mutex);
        if (!result.firstError.empty())
            return;
        result.firstError = std::format("compiling combo {}/{}: {}", combo.staticCombo, combo.dynamicCombo, reason.substr(0, reason.find('\n')));
        const std::string stem = std::format("{}_{}_{}", file.name, combo.staticCombo, combo.dynamicCombo);
        std::ofstream(failureDirectory / (stem + ".hlsl")) << translation.hlsl;
        std::ofstream(failureDirectory / (stem + ".txt")) << reason;
    };
    auto finish = [&](FileWork& file) {
        translateFailures += file.result.translateFailures;
        compileFailures += file.result.compileFailures;
        if (!file.result.firstError.empty())
            log::Info("Shader prewarm: {}: {} translation and {} compile failures; first {}", file.name, file.result.translateFailures.load(),
                file.result.compileFailures.load(), file.result.firstError);
    };

    std::mutex queueMutex;
    std::condition_variable queueChanged;
    std::deque<std::shared_ptr<FileWork>> queue;
    bool readerDone = false;
    std::jthread reader([&] {
        for (const Vpk::Entry* entry : files) {
            auto file = std::make_shared<FileWork>();
            if (!readCombos(*entry, file->combos))
                continue;
            file->name = entry->path.substr(kShaderDirectory.size());
            file->work = UniqueCombos(file->combos);
            file->remaining = file->work.size();
            if (file->work.empty())
                continue;
            std::unique_lock lock(queueMutex);
            queueChanged.wait(lock, [&] { return queue.size() < kFilesAhead || progress.cancel; });
            if (progress.cancel)
                break;
            queue.push_back(std::move(file));
            queueChanged.notify_all();
        }
        std::scoped_lock lock(queueMutex);
        readerDone = true;
        queueChanged.notify_all();
    });
    auto worker = [&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        while (!progress.cancel) {
            std::shared_ptr<FileWork> file;
            size_t index = 0;
            {
                std::unique_lock lock(queueMutex);
                queueChanged.wait(lock, [&] { return !queue.empty() || readerDone; });
                if (queue.empty())
                    break;
                file = queue.front();
                index = file->next++;
                if (file->next == file->work.size()) {
                    queue.pop_front();
                    queueChanged.notify_all();
                }
            }
            const vcs::Combo& combo = *file->work[index];
            translator::Translation translation;
            bool available = false;
            build(*file, combo, {}, translation, available);
            const bool vertexShader = !combo.bytecode.empty() && (combo.bytecode[0] & 0xFFFF0000u) == 0xFFFE0000u;
            if (available && vertexShader) {
                if (const std::optional<translator::Variant> compressed = CompressedVariant(translation)) {
                    translator::Translation variantTranslation;
                    build(*file, combo, *compressed, variantTranslation, available);
                }
            }
            ++progress.done;
            if (--file->remaining == 0)
                finish(*file);
        }
        // The reader may be waiting for room in the queue when a cancel stops the workers.
        queueChanged.notify_all();
    };
    std::vector<std::jthread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back(worker);
    pool.clear();
    reader.join();
    if (progress.cancel) {
        log::Info("Shader prewarm: cancelled after {} of {} combos", progress.done.load(), total);
        return;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    log::Info("Shader prewarm: {} combos, {} shaders compiled, {} translation and {} compile failures in {:.1f} s on {} threads", total,
        compiled.load(), translateFailures.load(), compileFailures.load(), seconds, threads);
    if (const std::optional<std::string> marker = MarkerText(gameDirectory))
        std::ofstream(MarkerPath(gameDirectory), std::ios::trunc) << *marker;
}

}
