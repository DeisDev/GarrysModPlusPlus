#pragma once

#include "shaderapi_meshes.h"
#include "shaderapi_shadow.h"

namespace gmodpp::native {

// A compiled shader file (shaders\fxc\<name>.vcs, read through the game's filesystem like shaderapidx9.dll's
// CShaderManager at 0x180034cc0), or null when the file is missing or unreadable.
const ShaderFile* FindShaderFile(const char* name, bool vertex);

// Starts compiling, on worker threads, the shaders a new state snapshot's passes will draw with (shader_precache.h).
void PrecacheShaders(const ShadowState& state);

// The bound render and depth targets, moved into their writable states.
struct Targets {
    D3D12_CPU_DESCRIPTOR_HANDLE colors[4] = {};
    DXGI_FORMAT colorFormats[4] = {};
    UINT count = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE depth = {};
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    UINT width = 0;
    UINT height = 0;
};
Targets PrepareTargets(bool srgb);

// The size of render target 0 (the back buffer's for the default target).
void TargetSize(UINT& width, UINT& height);

// One side of a copy between render targets and textures (Direct3D 9's StretchRect).
struct CopySurface {
    TrackedResource* resource;
    // Shader resource view of the source; render target view of the destination (null when it is not a target).
    UINT texture;
    D3D12_CPU_DESCRIPTOR_HANDLE target;
    DXGI_FORMAT format;
    UINT width;
    UINT height;
};
// Copies `sourceRect` of `source` to `destinationRect` of `destination` with linear filtering, as StretchRect with
// D3DTEXF_LINEAR does: a plain copy when the sizes and formats allow it, otherwise a draw.
void StretchRect(const CopySurface& source, const D3D12_RECT& sourceRect, const CopySurface& destination, const D3D12_RECT& destinationRect);

// Grades `rect` of the back buffer by the menu's colour grading (shaders/grade.hlsl), when it is on and changes anything.
void GradeView(D3D12_RECT rect);

// Starts reading the pipeline library from disk on another thread, so the first draw does not wait for the file.
void ReadPipelineLibraryInBackground();
// Saves new pipeline states to the pipeline library on disk: when `now`, on this thread after any save in progress;
// otherwise at most every 30 seconds, on another thread.
void SavePipelineLibrary(bool now);

// Logs the next draws' state (developer diagnostics).
void LogNextDraws(int count);
// Draws only the first `count` passes of each frame from now on (-1 for all), and restarts the frame's count.
void LimitDraws(int count);

// Records the draws of one pass of the mesh being drawn, with the snapshot's state and the dynamic state.
void DrawPass(const ShadowState& shadow, const MeshDraw& mesh);

}
