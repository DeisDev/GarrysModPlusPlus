#include "shader_prewarm.h"

#include "log.h"
#include "shader_compiler.h"
#include "shader_translator.h"
#include "vcs.h"
#include "vpk.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>

namespace gmodpp {

namespace {

constexpr std::string_view kShaderDirectory = "shaders/fxc/";

struct FileResult {
    size_t combos = 0;
    size_t unique = 0;
    std::atomic<size_t> translateFailures = 0;
    std::atomic<size_t> compileFailures = 0;
    std::mutex mutex;
    std::string firstTranslateError;
    std::string firstCompileError;
};

std::string FirstLine(const std::string& text)
{
    return text.substr(0, text.find('\n'));
}

}

void PrewarmShaders(const std::filesystem::path& gameDirectory, const std::filesystem::path& compilerDirectory)
{
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    ShaderCompiler compiler;
    if (!compiler.Load(compilerDirectory, error)) {
        log::Info("Shader prewarm: {}", error);
        return;
    }
    Vpk vpk;
    if (!vpk.Open(gameDirectory / L"garrysmod" / L"garrysmod_dir.vpk", error)) {
        log::Info("Shader prewarm: {}", error);
        return;
    }
    const std::filesystem::path failureDirectory = gameDirectory / L"gmodpp_cache" / L"prewarm_failures";
    std::filesystem::remove_all(failureDirectory);
    std::filesystem::create_directories(failureDirectory);

    const unsigned threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    std::map<std::string, size_t> translateReasons;
    std::mutex reasonsMutex;
    size_t files = 0, combos = 0, unique = 0, translateFailures = 0, compileFailures = 0;
    for (const Vpk::Entry& entry : vpk.Entries()) {
        if (!entry.path.starts_with(kShaderDirectory) || !entry.path.ends_with(".vcs"))
            continue;
        ++files;
        const std::string name = entry.path.substr(kShaderDirectory.size());
        auto bytes = vpk.Read(entry, error);
        vcs::Header header;
        std::vector<vcs::Combo> all;
        if (!bytes || !vcs::ReadCombos(*bytes, header, all, error)) {
            log::Info("Shader prewarm: {}: {}", name, error);
            continue;
        }
        std::vector<const vcs::Combo*> work;
        std::unordered_set<std::string_view> seen;
        for (const vcs::Combo& combo : all) {
            std::string_view key(reinterpret_cast<const char*>(combo.bytecode.data()), combo.bytecode.size() * sizeof(DWORD));
            if (seen.insert(key).second)
                work.push_back(&combo);
        }
        FileResult result;
        result.combos = all.size();
        result.unique = work.size();
        std::atomic<size_t> next = 0;
        auto worker = [&] {
            std::string reason;
            translator::Translation translation;
            std::vector<unsigned char> dxil;
            for (size_t i = next++; i < work.size(); i = next++) {
                const vcs::Combo& combo = *work[i];
                if (!translator::Translate(combo.bytecode, {}, translation, reason)) {
                    ++result.translateFailures;
                    {
                        std::scoped_lock lock(reasonsMutex);
                        ++translateReasons[reason];
                    }
                    std::scoped_lock lock(result.mutex);
                    if (result.firstTranslateError.empty())
                        result.firstTranslateError = std::format("combo {}/{}: {}", combo.staticCombo, combo.dynamicCombo, reason);
                    continue;
                }
                const wchar_t* profile = translation.stage == translator::Stage::Vertex ? L"vs_6_6" : L"ps_6_6";
                if (compiler.Compile(translation.hlsl, profile, dxil, reason))
                    continue;
                ++result.compileFailures;
                std::scoped_lock lock(result.mutex);
                if (!result.firstCompileError.empty())
                    continue;
                result.firstCompileError = std::format("combo {}/{}: {}", combo.staticCombo, combo.dynamicCombo, FirstLine(reason));
                const std::string stem = std::format("{}_{}_{}", name, combo.staticCombo, combo.dynamicCombo);
                std::ofstream(failureDirectory / (stem + ".hlsl")) << translation.hlsl;
                std::ofstream(failureDirectory / (stem + ".txt")) << reason;
            }
        };
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back(worker);
        pool.clear();

        combos += result.combos;
        unique += result.unique;
        translateFailures += result.translateFailures;
        compileFailures += result.compileFailures;
        log::Info("Shader prewarm: {}: {} combos, {} unique, {} translation and {} compile failures{}{}", name, result.combos, result.unique,
            result.translateFailures.load(), result.compileFailures.load(),
            result.firstTranslateError.empty() ? "" : "; first translation failure " + result.firstTranslateError,
            result.firstCompileError.empty() ? "" : "; first compile failure " + result.firstCompileError);
    }
    for (const auto& [reason, count] : translateReasons)
        log::Info("Shader prewarm: translation failure x{}: {}", count, reason);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    log::Info("Shader prewarm: {} files, {} combos, {} unique, {} translation and {} compile failures in {:.1f} s on {} threads", files, combos,
        unique, translateFailures, compileFailures, seconds, threads);
}

}
