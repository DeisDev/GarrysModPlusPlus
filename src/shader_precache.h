#pragma once

#include "shader_translator.h"
#include "vcs.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

// Shaders are compiled for the materials the game sets up, when it sets them up, instead of for every combo the game
// ships: Source materials name each pass's shader files and static combos when their state snapshots are taken (map
// loads and model precaches), so their dynamic combos are translated and compiled then, on worker threads, while the
// loading screen is up. A draw that needs a shader no worker has finished compiles it itself, or waits for the worker
// already compiling it.
namespace gmodpp::shader_precache {

// The translation of `code` in `variant` and its DXIL: from the shader cache (shader_cache.h), or translated,
// compiled and stored now. `name` names the shader in logs. Returns false when it cannot be translated or compiled.
// Thread-safe: while one thread compiles a key, others asking for it wait for its result instead of compiling it again,
// and a key that failed is not tried again.
bool Build(std::span<const DWORD> code, const translator::Variant& variant, const char* name, translator::Translation& info,
    std::vector<unsigned char>& dxil);

// Who waits for a request, in the order workers take them.
enum class Priority : std::uint8_t {
    // The materials of the map being loaded: its loading screen waits for them.
    Map,
    // Vertex shaders with a user clip plane, as the game draws the views of water's reflection and refraction: the
    // loading screen waits for them on maps with water.
    Water,
    // Shaders of earlier sessions compiled again after an update (StartReplay), and developer requests: nothing waits.
    Background,
    Count,
};

// One static combo of a shader file that a material pass uses.
struct Request {
    // The .vcs file's name, bytes and static combo directory, which must stay in place for the life of the process (as
    // FindShaderFile's do).
    const char* name = nullptr;
    std::span<const std::uint8_t> file;
    const vcs::Directory* directory = nullptr;
    // As IShaderShadow::SetVertexShader and SetPixelShader take it: the static combo times the file's dynamic combos.
    std::int32_t staticIndex = 0;
    // Whether the file holds vertex shaders.
    bool vertex = false;
    // Vertex shaders: the user clip planes they clip by (translator::Variant::clipPlanes).
    std::uint8_t clipPlanes = 0;
    // Pixel shaders: the pass's alpha test (translator::Variant::alphaTest).
    std::uint8_t alphaTest = 0;
    // Pixel shaders with twins (shader_twins.h): the G-buffer twin that opaque passes draw during real-time lighting, and
    // the forward twin that translucent ones draw.
    bool gbufferTwin = false;
    bool forwardTwin = false;
    // Pixel shaders of opaque passes that write depth and colour: during real-time lighting the game's own shaders with one
    // colour output also clear the G-buffer's coverage (translator::Variant::clearCoverage; markMirror for
    // lightmappedreflective_ps).
    bool clearCoverage = false;
    // The device has opacity micromaps (translator::Variant::opacityMicromaps).
    bool opacityMicromaps = false;
};

// Queues compiling every dynamic combo of the request's static combo on worker threads, in the variants its draws
// usually take: vertex shaders as they are and, when they read a normal, for compressed vertices; pixel shaders with
// the pass's alpha test, and those with one colour output also clearing the coverage when requested; and the requested
// twins. Vertex shaders are queued again with a user clip plane (Priority::Water). Requests are remembered in
// gmodplusplus\cache\precache.txt for StartReplay. A request equal to an earlier one is ignored, and so is every
// request when the shader cache cannot store what the workers compile.
void Enqueue(const Request& request);

// Once the game is up: when the shader cache, the translator, the lighting headers, the twins or the game's shaders have
// changed since the requests remembered from earlier sessions were last compiled, queues them all again
// (Priority::Background), so the maps played before do not compile their shaders while they load or while they are
// played. Reads the files from the game's VPK on another thread.
void StartReplay(const std::filesystem::path& gameDirectory);

// Writes out the requests Enqueue remembered since the last save (it writes them out in batches).
void SaveRemembered();

// Developer tool (-gmodpp_dev, shaders.request): queues every static combo of every shader the game ships, with both
// twins, to find combos the translator or the compiler cannot handle (Priority::Background, not remembered).
void EnqueueEveryShader(const std::filesystem::path& gameDirectory);

struct Progress {
    std::size_t done = 0;
    std::size_t total = 0;
};
// The requests of a priority finished, of those queued so far.
Progress Status(Priority priority = Priority::Map);

}
