#pragma once

#include "gbuffer_shaders.h"
#include "lighting.h"
#include "shaderapi_pipelines.h"
#include "vcs.h"
#include "view_types.h"

#include <d3d12.h>

#include <array>
#include <span>
#include <vector>

// The G-buffer capture inside the native shader API: while a view's opaque pass is drawn into the back buffer, the
// game's lighting preview shaders (selected through INT_RENDERPARM_ENABLE_FIXED_LIGHTING) write albedo, normal,
// self-illumination and coverage into our targets, and the passes ray tracing replaces are skipped. After the capture
// the translucent surfaces are drawn with variants lit by the probe volume or traced (forward twins).
namespace gmodpp::native::capture {

// Starts capturing a view drawn into the back buffer with its depth buffer; `jitter` (NDC units) is added to its
// projection. False when the targets cannot be created.
bool Begin(std::array<float, 2> jitter);
bool Active();
// Ends the capture and fills the view's inputs, including the textures lighting copies the main view's probe volume
// into. They are left in D3D12_RESOURCE_STATE_COMMON, as lighting leaves them, and its commands must run after the
// backend's (Backend::Submit). False when nothing was captured.
bool End(gbuffer::ViewKind kind, ViewInputs& inputs);

// Lights the translucent surfaces drawn until EndForward from the probe volume.
void BeginForward(const gbuffer::ForwardConstants& constants);
void EndForward();

// INT_RENDERPARM_ENABLE_FIXED_LIGHTING while capturing or lighting translucent surfaces, or -1 to keep the game's.
int FixedLighting();

// A constant register replaced in one draw's upload.
struct RegisterPatch {
    UINT reg;
    std::array<float, 4> value;
};

// Where one pass of a draw goes during a capture, and what it draws with.
struct Pass {
    enum class Route { Game, GBuffer, Albedo, Light, Skip };
    Route route = Route::Game;
    // The pixel shader code to draw with instead of the game's, if any.
    std::span<const DWORD> pixelCode;
    // A stable name for the replaced code (logs and the shader cache's developer dumps).
    const char* pixelName = nullptr;
    // Self-illumination written to the emissive target.
    bool emissive = false;
    std::vector<RegisterPatch> vertexPatches;
    std::vector<RegisterPatch> pixelPatches;
    // Samplers that read the probe volume instead of the game's textures.
    std::array<UINT, 3> probeSamplers = {};
    bool probes = false;
    // An opaque draw of the game's own shader into a captured view: what it draws hides the G-buffer behind it (an
    // eyeball over the face mesh behind it), so it clears the coverage there (translator::Variant::clearCoverage).
    bool clearsCoverage = false;
    // A forward twin, lit in the shader from these frame constants and scene (translator::Variant::forwardLighting).
    D3D12_GPU_VIRTUAL_ADDRESS forwardFrame = 0;
    D3D12_GPU_VIRTUAL_ADDRESS forwardScene = 0;
};

// The game's pixel shader combo a pass draws: its shader file's header and combo indices (shader_twins.h).
struct ComboId {
    const vcs::Header* header = nullptr;
    int staticIndex = 0;
    int dynamicIndex = 0;
};

// Decides a pass of the draw being recorded. `vertexCode` and `pixelCode` are the game's combos, from the shader file
// `pixelFile` (null without a pixel shader).
Pass Prepare(std::span<const DWORD> vertexCode, std::span<const DWORD> pixelCode, const char* pixelFile, const ComboId& pixelCombo, bool depthTest,
    bool blend, int sourceBlend, int destinationBlend, bool srgbWrite, std::uint32_t srgbRead);
// Binds the capture's targets for a pass routed to them.
Targets BindTargets(const Pass& pass, bool srgb);
// Adds the G-buffer's coverage target to a pass's targets as render target 1, for a pass with clearsCoverage.
void AddCoverageTarget(Targets& targets);
// The probe volume's red, green or blue texture for a probe sampler of a forward draw, ready to be read.
UINT ProbeView(size_t index);

// Releases the targets (the back buffer's size changes).
void Release();

}
