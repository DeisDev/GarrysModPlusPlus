#include "shader_twins.h"

#include "log.h"
#include "options.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace gmodpp::twins {

namespace {

struct Switch {
    std::string name;
    int low = 0;
    int count = 1;
};

using Values = std::map<std::string, int, std::less<>>;

int ValueOf(const Values& values, std::string_view name)
{
    const auto found = values.find(name);
    return found != values.end() ? found->second : 0;
}

struct Family {
    std::string_view gameFile;
    const char* source;
    // The platform tag the source's combo lines use for this file.
    std::string_view target;
    const char* profile;
    const char* model;
    // The game file's header the twin was checked against (NATIVE_DX12_PLAN.md, Phase 3 status).
    std::int32_t totalCombos;
    std::int32_t dynamicCombos;
    std::uint32_t sourceCrc32;
    // Combos the twin leaves to the game's shader.
    bool (*keepGame)(const Values& values);
    // Dynamic combos the G-buffer output does not depend on (lights, fog, alpha writes, envmap contrast), compiled as
    // 0 so their combos share one twin. LIGHTING_PREVIEW is always 0: the game binds its preview combos during a
    // capture, whose pow(x, 2.2) would convert the sRGB-decoded textures again.
    std::string_view ignoredDynamics;
};

// Flashlight passes are skipped in real-time mode; GMod's lightmap switch is not in the twin yet; unlit combos keep
// the game's colour.
bool KeepVertexLitGeneric(const Values& values)
{
    return ValueOf(values, "FLASHLIGHT") != 0 || ValueOf(values, "LIGHTMAP") != 0 || ValueOf(values, "DIFFUSELIGHTING") == 0;
}

bool KeepBump(const Values& values) { return ValueOf(values, "FLASHLIGHT") != 0 || ValueOf(values, "DIFFUSELIGHTING") == 0; }

bool KeepSkin(const Values& values) { return ValueOf(values, "FLASHLIGHT") != 0; }

bool KeepNone(const Values&) { return false; }

constexpr Family kFamilies[] = {
    { "vertexlit_and_unlit_generic_ps20b", "vertexlit_and_unlit_generic_ps2x.fxc", "ps20b", "ps_2_b", "SHADER_MODEL_PS_2_B", 377487360, 12, 0x6e13aa07,
        &KeepVertexLitGeneric, "" },
    { "vertexlit_and_unlit_generic_bump_ps20b", "vertexlit_and_unlit_generic_bump_ps2x.fxc", "ps20b", "ps_2_b", "SHADER_MODEL_PS_2_B", 860160, 20,
        0x7df2bfaa, &KeepBump, "NUM_LIGHTS AMBIENT_LIGHT" },
    { "skin_ps20b", "skin_ps20b.fxc", "ps20b", "ps_2_b", "SHADER_MODEL_PS_2_B", 6881280, 80, 0xa7b42ee9, &KeepSkin,
        "WRITEWATERFOGTODESTALPHA PIXELFOGTYPE NUM_LIGHTS WRITE_DEPTH_TO_DESTALPHA" },
    // GMod's total of 2,717,908,992 combos wraps in the header's 32-bit signed field.
    { "lightmappedgeneric_ps30", "lightmappedgeneric_ps2x.fxc", "ps30", "ps_3_0", "SHADER_MODEL_PS_3_0", -1577058304, 96, 0x3b558d77, &KeepNone,
        "FASTPATHENVMAPCONTRAST WRITEWATERFOGTODESTALPHA PIXELFOGTYPE WRITE_DEPTH_TO_DESTALPHA" },
};

std::filesystem::path TwinDirectory() { return GetOptions().gameDirectory / L"bin" / L"win64" / L"gmodpp" / L"twins"; }

std::filesystem::path CacheDirectory() { return GetOptions().gameDirectory / L"gmodpp_cache" / L"twins"; }

std::uint64_t Fnv(std::string_view data, std::uint64_t hash = 14695981039346656037ull)
{
    for (const char c : data) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool ReadText(const std::filesystem::path& path, std::string& text)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream contents;
    contents << file.rdbuf();
    text = contents.str();
    return true;
}

class Includes final : public ID3DInclude {
public:
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override
    {
        auto found = m_files.find(name);
        if (found == m_files.end()) {
            std::string text;
            if (!ReadText(TwinDirectory() / name, text))
                return E_FAIL;
            found = m_files.emplace(name, std::move(text)).first;
        }
        *data = found->second.data();
        *bytes = static_cast<UINT>(found->second.size());
        return S_OK;
    }
    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }

private:
    std::unordered_map<std::string, std::string> m_files;
};

// Combo lines of a Valve shader source ("// STATIC: "NAME" "0..1" [ps20b] [PC]"): the switches of one target in
// declaration order, which is their stride order; lines for another target or the Xbox are left out. `names` gets
// every combo name of the file, which the shader compiler defines (0 unless set).
void ReadSwitches(const std::string& source, std::string_view target, std::vector<Switch>& statics, std::vector<Switch>& dynamics,
    std::vector<std::string>& names)
{
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t comment = line.find("//");
        if (comment == std::string::npos)
            continue;
        std::string_view rest = std::string_view(line).substr(comment + 2);
        rest.remove_prefix(std::min(rest.find_first_not_of(" \t"), rest.size()));
        const bool isStatic = rest.starts_with("STATIC:");
        if (!isStatic && !rest.starts_with("DYNAMIC:"))
            continue;
        const size_t nameStart = rest.find('"');
        const size_t nameEnd = rest.find('"', nameStart + 1);
        const size_t rangeStart = rest.find('"', nameEnd + 1);
        const size_t rangeEnd = rest.find('"', rangeStart + 1);
        if (rangeEnd == std::string_view::npos)
            continue;
        std::string name(rest.substr(nameStart + 1, nameEnd - nameStart - 1));
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
        const std::string_view tags = rest.substr(rangeEnd + 1);
        if (tags.find("XBOX") != std::string_view::npos)
            continue;
        // A line with platform tags counts when one of them is the target.
        bool tagged = false, matched = false;
        for (size_t at = tags.find('['); at != std::string_view::npos; at = tags.find('[', at + 1)) {
            const std::string_view tag = tags.substr(at + 1, tags.find(']', at) - at - 1);
            if ((tag.starts_with("ps") || tag.starts_with("vs")) && tag.size() > 2 && std::isdigit(static_cast<unsigned char>(tag[2]))) {
                tagged = true;
                matched = matched || tag == target;
            }
        }
        if (tagged && !matched)
            continue;
        const std::string_view range = rest.substr(rangeStart + 1, rangeEnd - rangeStart - 1);
        const size_t dots = range.find("..");
        if (dots == std::string_view::npos)
            continue;
        const int low = std::stoi(std::string(range.substr(0, dots)));
        const int high = std::stoi(std::string(range.substr(dots + 2)));
        (isStatic ? statics : dynamics).push_back({ std::move(name), low, high - low + 1 });
    }
}

struct LoadedFamily {
    const Family* family = nullptr;
    bool usable = false;
    std::string source;
    std::vector<Switch> statics;
    std::vector<Switch> dynamics;
    std::vector<std::string> names;
    // Every file the twins are compiled from, which names the compiled twins in the cache.
    std::uint64_t contentHash = 0;
    bool loggedFailure = false;
    bool loggedUse = false;
};

pD3DCompile Compiler()
{
    static const pD3DCompile compile = [] {
        const HMODULE module = LoadLibraryW(D3DCOMPILER_DLL_W);
        const auto function = module != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile")) : nullptr;
        if (function == nullptr)
            log::Info("Twins: {} is unavailable; the game's shaders are drawn instead", "d3dcompiler_47.dll");
        return function;
    }();
    return compile;
}

LoadedFamily* FamilyFor(std::string_view file, const vcs::Header& header)
{
    static std::unordered_map<std::string, LoadedFamily> loaded;
    std::string key(file);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key.ends_with(".vcs"))
        key.resize(key.size() - 4);
    const auto existing = loaded.find(key);
    if (existing != loaded.end())
        return existing->second.usable ? &existing->second : nullptr;
    LoadedFamily& entry = loaded[key];
    const auto family = std::find_if(std::begin(kFamilies), std::end(kFamilies), [&](const Family& f) { return f.gameFile == key; });
    if (family == std::end(kFamilies))
        return nullptr;
    entry.family = &*family;
    if (header.totalCombos != family->totalCombos || header.dynamicCombos != family->dynamicCombos || header.sourceCrc32 != family->sourceCrc32) {
        log::Info("Twins: {} is not the version its twin was made for (combos {} / {}, source CRC {:#010x}); the game's shader is drawn", key,
            header.totalCombos, header.dynamicCombos, header.sourceCrc32);
        return nullptr;
    }
    if (!ReadText(TwinDirectory() / family->source, entry.source)) {
        log::Info("Twins: {} is missing; the game's shader is drawn", (TwinDirectory() / family->source).string());
        return nullptr;
    }
    ReadSwitches(entry.source, family->target, entry.statics, entry.dynamics, entry.names);
    std::uint64_t dynamicCount = 1, staticCount = 1;
    for (const Switch& s : entry.dynamics)
        dynamicCount *= static_cast<std::uint64_t>(s.count);
    for (const Switch& s : entry.statics)
        staticCount *= static_cast<std::uint64_t>(s.count);
    if (dynamicCount != static_cast<std::uint64_t>(header.dynamicCombos) ||
        static_cast<std::uint32_t>(dynamicCount * staticCount) != static_cast<std::uint32_t>(header.totalCombos)) {
        log::Info("Twins: {} declares {} x {} combos, the game file {} / {}; the game's shader is drawn", family->source, dynamicCount, staticCount,
            header.dynamicCombos, header.totalCombos);
        return nullptr;
    }
    entry.contentHash = Fnv(family->profile, SourceHash());
    entry.usable = Compiler() != nullptr;
    return entry.usable ? &entry : nullptr;
}

std::uint64_t Encode(const std::vector<Switch>& switches, const Values& values)
{
    std::uint64_t index = 0;
    std::uint64_t stride = 1;
    for (const Switch& s : switches) {
        index += static_cast<std::uint64_t>(ValueOf(values, s.name) - s.low) * stride;
        stride *= static_cast<std::uint64_t>(s.count);
    }
    return index;
}

void Decode(std::uint64_t index, const std::vector<Switch>& switches, Values& values)
{
    for (const Switch& s : switches) {
        values[s.name] = s.low + static_cast<int>(index % static_cast<std::uint64_t>(s.count));
        index /= static_cast<std::uint64_t>(s.count);
    }
}

// Compiles a twin for a combo's switch values, or loads it from the cache (gmodpp_cache\twins) when the same files
// were compiled for the same values before.
bool Compile(LoadedFamily& entry, const Values& values, std::vector<DWORD>& code, std::string& error)
{
    std::vector<std::string> storage;
    storage.reserve(entry.names.size() * 2 + 4);
    std::vector<D3D_SHADER_MACRO> macros;
    std::uint64_t key = entry.contentHash;
    for (const std::string& name : entry.names) {
        storage.push_back(name);
        const char* macroName = storage.back().c_str();
        storage.push_back(std::to_string(ValueOf(values, name)));
        macros.push_back({ macroName, storage.back().c_str() });
        key = Fnv(std::format("{}={};", name, storage.back()), key);
    }
    macros.push_back({ entry.family->model, "1" });
    macros.push_back({ "GBUFFER_TWIN", "1" });
    macros.push_back({ nullptr, nullptr });
    const std::filesystem::path cached = CacheDirectory() / std::format("{:016x}.bin", key);
    if (std::string bytes; ReadText(cached, bytes) && !bytes.empty() && bytes.size() % sizeof(DWORD) == 0) {
        code.resize(bytes.size() / sizeof(DWORD));
        std::memcpy(code.data(), bytes.data(), bytes.size());
        return true;
    }
    Includes includes;
    ID3DBlob* blob = nullptr;
    ID3DBlob* messages = nullptr;
    const HRESULT hr = Compiler()(entry.source.data(), entry.source.size(), entry.family->source, macros.data(), &includes, "main",
        entry.family->profile, 0, 0, &blob, &messages);
    if (FAILED(hr)) {
        error = messages != nullptr ? std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize())
                                    : std::format("D3DCompile failed: {:#010x}", static_cast<unsigned>(hr));
    } else {
        code.resize(blob->GetBufferSize() / sizeof(DWORD));
        std::memcpy(code.data(), blob->GetBufferPointer(), code.size() * sizeof(DWORD));
        std::error_code directoryError;
        std::filesystem::create_directories(CacheDirectory(), directoryError);
        std::ofstream(cached, std::ios::binary).write(reinterpret_cast<const char*>(code.data()), static_cast<std::streamsize>(code.size() * sizeof(DWORD)));
    }
    if (blob != nullptr)
        blob->Release();
    if (messages != nullptr)
        messages->Release();
    return SUCCEEDED(hr);
}

}

std::span<const DWORD> GBufferShader(std::string_view file, const vcs::Header& header, int staticIndex, int dynamicIndex)
{
    // The prewarm compiles twins on worker threads while nothing draws; the render thread uses them alone later.
    static std::mutex mutex;
    static std::map<std::tuple<const LoadedFamily*, int, int>, std::vector<DWORD>> compiled;
    LoadedFamily* entry = nullptr;
    {
        std::scoped_lock lock(mutex);
        entry = FamilyFor(file, header);
    }
    if (entry == nullptr || staticIndex < 0 || dynamicIndex < 0)
        return {};
    Values values;
    Decode(static_cast<std::uint64_t>(dynamicIndex), entry->dynamics, values);
    Decode(static_cast<std::uint64_t>(staticIndex) / static_cast<std::uint64_t>(std::max(header.dynamicCombos, 1)), entry->statics, values);
    const bool keepGame = entry->family->keepGame(values);
    for (const Switch& s : entry->dynamics) {
        if (s.name == "LIGHTING_PREVIEW" || (" " + std::string(entry->family->ignoredDynamics) + " ").find(" " + s.name + " ") != std::string::npos)
            values[s.name] = 0;
    }
    const auto key = std::tuple(static_cast<const LoadedFamily*>(entry), staticIndex, static_cast<int>(Encode(entry->dynamics, values)));
    {
        std::scoped_lock lock(mutex);
        if (const auto found = compiled.find(key); found != compiled.end())
            return found->second;
    }
    std::vector<DWORD> code;
    std::string error;
    const bool failed = !keepGame && !Compile(*entry, values, code, error);
    std::scoped_lock lock(mutex);
    if (failed && !entry->loggedFailure) {
        entry->loggedFailure = true;
        // The compiler lists warnings before the error.
        const size_t at = error.find("error");
        const size_t line = at == std::string::npos ? 0 : error.rfind('\n', at) + 1;
        log::Info("Twins: compiling {} for combo {} / {} failed; the game's shader is drawn: {}", entry->family->source, staticIndex, dynamicIndex,
            error.substr(line, error.find('\n', line) - line));
    } else if (!code.empty() && !entry->loggedUse) {
        entry->loggedUse = true;
        log::Info("Twins: {} draws its G-buffer twin", entry->family->gameFile);
    }
    return compiled.try_emplace(key, std::move(code)).first->second;
}

std::uint64_t SourceHash()
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& item : std::filesystem::directory_iterator(TwinDirectory(), error))
        files.push_back(item.path());
    std::sort(files.begin(), files.end());
    std::uint64_t hash = Fnv("");
    for (const std::filesystem::path& path : files) {
        std::string text;
        if (ReadText(path, text))
            hash = Fnv(text, Fnv(path.filename().string(), hash));
    }
    return hash;
}

bool HasTwin(std::string_view file)
{
    std::string key(file);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key.ends_with(".vcs"))
        key.resize(key.size() - 4);
    return std::any_of(std::begin(kFamilies), std::end(kFamilies), [&](const Family& f) { return f.gameFile == key; });
}

}
