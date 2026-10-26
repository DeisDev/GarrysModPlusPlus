#pragma once

#include <d3d9.h>

#include <array>
#include <optional>
#include <span>
#include <vector>
#include <wrl/client.h>

namespace gmodpp::gbuffer {

// The views captured into the G-buffer: the main view, the 3D skybox (drawn before it) and the viewmodel.
enum class ViewKind { Main, Skybox, Viewmodel };

using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);

// Creates the extra render targets. Returns false (with a log message) when the device cannot do it.
bool Initialize(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& params);
void Release();

// Classifies every pixel shader the game creates; LIGHTING_PREVIEW variants are patched and model shaders
// without one get a replacement.
HRESULT CreatePixelShader(IDirect3DDevice9* device, const DWORD* code, IDirect3DPixelShader9** shader, CreatePixelShaderFn create);

// Device hook callbacks. `SubstitutePixelShader` returns the shader to bind for the requested one.
IDirect3DPixelShader9* SubstitutePixelShader(IDirect3DPixelShader9* requested);
enum class DrawAction { Draw, Skip, RedirectToLight };
DrawAction BeforeDraw(IDirect3DDevice9* device);
// Wrap a draw for which BeforeDraw returned RedirectToLight.
void BeginLightRedirect(IDirect3DDevice9* device);
void EndLightRedirect(IDirect3DDevice9* device);
// After the game sets a render target.
void OnRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface);
void OnViewport(const D3DVIEWPORT9& viewport);
HRESULT CreateVertexShader(IDirect3DDevice9* device, const DWORD* code, IDirect3DVertexShader9** shader, CreateVertexShaderFn create);
void OnVertexShader(IDirect3DVertexShader9* shader);
void OnVertexShaderConstants(UINT start, const float* data, UINT count);
// Returns the constants to send to the device: during a capture, Source's view-projection rows (c4-c11) carry
// the sub-pixel jitter set by SetJitter. `scratch` holds the modified copy.
const float* JitterVertexConstants(UINT start, const float* data, UINT count, std::vector<float>& scratch);
// Clip-space offsets (NDC units) added to the next captures' projection, for temporal anti-aliasing.
void SetJitter(float x, float y);

using Matrix4 = std::array<std::array<float, 4>, 4>;
// Source's view-projection rows (c8-c11), unjittered, as the last G-buffer draw of the current or last capture
// used them. The game resets them before a capture ends.
const Matrix4& LastCaptureViewProj();
// The eye position of a view-projection, or nothing when it is degenerate.
std::optional<std::array<float, 3>> EyePosition(const Matrix4& viewProj);
void OnTexture(DWORD stage, IDirect3DBaseTexture9* texture);
void OnRenderState(D3DRENDERSTATETYPE state, DWORD value);
void OnPixelShaderConstants(UINT start, const float* data, UINT count);

// Starts capturing the opaque pass of a view into the G-buffer; RT0 must already be the back buffer.
bool Begin(IDirect3DDevice9* device);
void End(IDirect3DDevice9* device, ViewKind kind);
bool Active();

// Linear albedo and self-illuminated colour of captured surfaces.
IDirect3DTexture9* AlbedoTexture();
IDirect3DTexture9* EmissiveTexture();
IDirect3DTexture9* NormalTexture();
IDirect3DTexture9* FlagsTexture();
// Light added by flashlight and projected-texture passes, other additive passes and self-illuminated surfaces, in
// the back buffer's encoding.
IDirect3DTexture9* LightTexture();
// The tonemap scale Source multiplies lit colours by (cLightScale.x, pixel shader c30).
float TonemapScale();
// Whether the game draws lit surfaces with sRGB writes, which is how the back buffer is encoded.
bool OutputIsSrgb();

// A flashlight or projected texture (env_projectedtexture, GMod lamps) as its shader pass received it, in
// Source's DoFlashlight terms (common_flashlight_fxc.h). texture = dot(row, (position, 1)) for each row.
struct ProjectedLight {
    std::array<std::array<float, 4>, 4> worldToTexture;
    std::array<float, 3> origin;
    float farZ;
    std::array<float, 3> color;
    std::array<float, 3> attenuation;
    Microsoft::WRL::ComPtr<IDirect3DBaseTexture9> cookie;
};

// Projected lights seen in the last main view.
std::span<const ProjectedLight> ProjectedLights();

// Source's range fog as the view's forward shaders last used it: params are g_FogParams (x start over
// range, z max density, w 1 / range), color is g_LinearFogColor.
struct Fog {
    std::array<float, 4> params = {};
    std::array<float, 3> color = {};
    bool known = false;
};
const Fog& ViewFog(ViewKind kind);

}
