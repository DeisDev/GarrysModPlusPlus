#pragma once

#include "view_types.h"

#include <d3d9.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include <wrl/client.h>

namespace gmodpp::gbuffer {

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
// Forward: wrap the draw in BeginForwardDraw and EndForwardDraw.
enum class DrawAction { Draw, Skip, RedirectToLight, Forward };
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
// The offsets SetJitter last set, which the current or last capture was drawn with.
std::array<float, 2> Jitter();

// Source's view-projection rows (c8-c11), unjittered, as the last G-buffer draw of the current or last capture
// used them. The game resets them before a capture ends.
const Matrix4& LastCaptureViewProj();
void OnTexture(DWORD stage, IDirect3DBaseTexture9* texture);
void OnSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value);
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
// Light added by additive passes and self-illuminated surfaces, in the back buffer's encoding.
IDirect3DTexture9* LightTexture();
// The tonemap scale Source multiplies lit colours by (cLightScale.x, pixel shader c30).
float TonemapScale();
// Whether the game draws lit surfaces with sRGB writes, which is how the back buffer is encoded.
bool OutputIsSrgb();

// Projected lights seen in the last main view, and each one's cookie texture (null for none).
std::span<const ProjectedLight> ProjectedLights();
std::span<const Microsoft::WRL::ComPtr<IDirect3DBaseTexture9>> ProjectedCookies();

const SwayClock& LastSwayClock();
const Fog& ViewFog(ViewKind kind);

// The irradiance volume's textures (kProbeGrid), null when the device could not create them.
std::array<IDirect3DVolumeTexture9*, 3> ProbeTextures();

// Until EndForward the game's LIGHTING_PREVIEW shaders are selected and swapped for variants lit by the probe
// volume, as are the model shader replacements, so the view's translucent surfaces get real-time light.
void BeginForward(IDirect3DDevice9* device, const ForwardConstants& constants);
void EndForward(IDirect3DDevice9* device);
// Around a draw for which BeforeDraw returned DrawAction::Forward: binds the volume and the constants, and puts
// the game's sampler state back afterwards.
void BeginForwardDraw(IDirect3DDevice9* device);
void EndForwardDraw(IDirect3DDevice9* device);

}
