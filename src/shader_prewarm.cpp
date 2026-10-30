#include "shader_prewarm.h"

#include "log.h"
#include "shader_cache.h"
#include "shader_compiler.h"
#include "shader_translator.h"
#include "vcs.h"
#include "vpk.h"

#include <d3d9types.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_set>

namespace gmodpp {

namespace {

constexpr std::string_view kShaderDirectory = "shaders/fxc/";

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

    const unsigned threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    std::atomic<size_t> compiled = 0;
    size_t translateFailures = 0, compileFailures = 0;
    for (const Vpk::Entry* entry : files) {
        std::vector<vcs::Combo> combos;
        if (!readCombos(*entry, combos))
            continue;
        const std::string name = entry->path.substr(kShaderDirectory.size());
        const std::vector<const vcs::Combo*> work = UniqueCombos(combos);
        FileResult result;
        std::atomic<size_t> next = 0;
        auto build = [&](const vcs::Combo& combo, const translator::Variant& variant, translator::Translation& translation, bool& available) {
            const std::uint64_t key = shader_cache::Key(combo.bytecode, variant);
            std::vector<unsigned char> dxil;
            if (shader_cache::Load(cache, key, translation, dxil)) {
                available = true;
                return;
            }
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
            const std::string stem = std::format("{}_{}_{}", name, combo.staticCombo, combo.dynamicCombo);
            std::ofstream(failureDirectory / (stem + ".hlsl")) << translation.hlsl;
            std::ofstream(failureDirectory / (stem + ".txt")) << reason;
        };
        auto worker = [&] {
            for (size_t i = next++; i < work.size() && !progress.cancel; i = next++) {
                const vcs::Combo& combo = *work[i];
                translator::Translation translation;
                bool available = false;
                build(combo, {}, translation, available);
                const bool vertexShader = !combo.bytecode.empty() && (combo.bytecode[0] & 0xFFFF0000u) == 0xFFFE0000u;
                if (available && vertexShader) {
                    if (const std::optional<translator::Variant> compressed = CompressedVariant(translation)) {
                        translator::Translation variantTranslation;
                        build(combo, *compressed, variantTranslation, available);
                    }
                }
                ++progress.done;
            }
        };
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back(worker);
        pool.clear();
        translateFailures += result.translateFailures;
        compileFailures += result.compileFailures;
        if (!result.firstError.empty())
            log::Info("Shader prewarm: {}: {} translation and {} compile failures; first {}", name, result.translateFailures.load(),
                result.compileFailures.load(), result.firstError);
        if (progress.cancel) {
            log::Info("Shader prewarm: cancelled after {} of {} combos", progress.done.load(), total);
            return;
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    log::Info("Shader prewarm: {} combos, {} shaders compiled, {} translation and {} compile failures in {:.1f} s on {} threads", total,
        compiled.load(), translateFailures, compileFailures, seconds, threads);
    if (const std::optional<std::string> marker = MarkerText(gameDirectory))
        std::ofstream(MarkerPath(gameDirectory), std::ios::trunc) << *marker;
}

}
