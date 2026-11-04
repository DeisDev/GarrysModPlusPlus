#pragma once

#include "descriptor_heap.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace gmodpp::native::display {

// How the display mapping pass (shaders/post/display_output.hlsl) encodes what it writes.
enum class Encoding : std::uint32_t {
    Srgb = 0,
    // HDR10: Rec. 2020 primaries and the PQ curve, in absolute nits.
    Pq = 1,
    // The sRGB curve alone, clipped at white, for a promoted render target read back as the 8-bit target it stands for.
    PlainSrgb = 2,
};

// What the pass applies besides the frame. Built by ForOutput and ForScreenshot.
struct Mapping {
    Encoding encoding = Encoding::Srgb;
    // The display's peak in paper whites (1 in SDR; the tone curve's, which the camera effects' last pass applies to the
    // 3D view, post_effects.cpp), paper white and the black level in nits (HDR).
    float peak = 1.0f;
    float paperWhite = 80.0f;
    float blackLevel = 0.0f;
    float saturation = 1.0f;
    // The HUD's and menu's white in nits, the frame's 1 (HDR): 0 for paper white times the HDR exposure (UiWhite in
    // shaders/tone_curve.hlsli), whose state the camera effects' last pass left this frame (SetHdrExposure) or
    // DescriptorHeap::kInvalid.
    float uiWhite = 0.0f;
    UINT exposureView = DescriptorHeap::kInvalid;
    // Dither steps over the encoded range: 255 for 8 bits, 1023 for 10.
    float ditherSteps = 255.0f;
    // The game's gamma ramp (mat_monitorgamma), SDR only.
    bool gammaRamp = false;
    // The menu's texture (UiTarget), or DescriptorHeap::kInvalid.
    UINT uiView = DescriptorHeap::kInvalid;
    // Settings::debugView 16 and 17, and the display's peak in nits.
    std::uint32_t debugView = 0;
    float displayPeak = 80.0f;
};

// The mapping the swap chain's buffers take: SDR in `bits` (8 or 10) or HDR10, with the display's paper white and peak
// (Settings, Statistics).
Mapping ForOutput(bool hdr, UINT bits);

// A display output as DXGI and Windows' display configuration describe it (Using DirectX with Advanced Color,
// Microsoft Learn): its GDI device name and monitor name, whether Windows runs it in HDR (DXGI_OUTPUT_DESC1::ColorSpace
// is G2084_P2020), its luminance range in nits, Windows' SDR white level in nits (DISPLAYCONFIG_SDR_WHITE_LEVEL, 0 when
// unknown) and the bits per colour of its link.
struct DisplayDesc {
    std::wstring device;
    std::string name;
    bool hdr = false;
    float peakNits = 0.0f;
    float fullFrameNits = 0.0f;
    float blackNits = 0.0f;
    float sdrWhiteNits = 0.0f;
    UINT bits = 8;
    RECT desktop = {};
};
// The output of `adapter` (by LUID, enumerated through `factory`) whose desktop area intersects the window most, as
// Microsoft's guidance picks it (not IDXGISwapChain::GetContainingOutput), or nothing when the window covers none.
std::optional<DisplayDesc> FindDisplay(IDXGIFactory1* factory, LUID adapter, HWND window);
// A GDI device name in UTF-8, for the log.
std::string Utf8Name(const std::wstring& device);
// An 8-bit SDR view of the frame without the menu, as the screen shows it in SDR: what ReadPixels, screenshots and
// capture.request read.
Mapping ForScreenshot();

// Records the pass from the frame (`frameView`, in a pixel shader resource state) into `target`, a render target view of
// `format`, `width` x `height`, which shows the frame from its pixel `originX`, `originY`. False when the pipeline
// cannot be made. Leaves the shader API's bindings to be set again (ForgetBindings).
bool Map(ID3D12GraphicsCommandList* commands, UINT frameView, D3D12_CPU_DESCRIPTOR_HANDLE target, DXGI_FORMAT format, UINT width, UINT height,
    int originX, int originY, const Mapping& mapping);

// The menu's render target: R8G8B8A8_UNORM of the window's size, cleared to transparent and moved into the render target
// state; the menu (Dear ImGui) leaves premultiplied colour in it, which Map composites. Returns its render target view and
// sets `view` to its shader resource view, or a null handle when it cannot be made.
D3D12_CPU_DESCRIPTOR_HANDLE BeginUi(UINT width, UINT height, UINT& view);
// Moves the menu's texture into a pixel shader resource state for Map.
void EndUi();
// The menu's texture format, for Dear ImGui's pipeline.
inline constexpr DXGI_FORMAT kUiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// The engine's colour correction at its engine_post draw (shaderapi_pipelines.cpp): the look-up count, default weight,
// weights and the textures' handles, which the camera effects' last pass applies to the 3D view after the tone curve.
// Cleared at each present (ClearColorCorrection), as the engine sets it each frame it draws engine_post.
struct ColorCorrection {
    std::uint32_t count = 0;
    float defaultWeight = 1.0f;
    std::array<float, 4> weights = {};
    std::array<std::intptr_t, 4> textures = {};
};
void SetColorCorrection(const ColorCorrection& correction);
void ClearColorCorrection();
// The HDR exposure's state buffer (an unordered access view, shaders/post/post.hlsl's ExposureMain) when the camera
// effects' last pass applied it to this frame's 3D view; cleared at each present (ClearHdrExposure), so frames without a
// 3D view show the HUD and menu at paper white.
void SetHdrExposure(UINT view);
void ClearHdrExposure();
// The recorded volumes' shader resource views (moved into a shader resource state) and weights, and the default weight;
// returns how many there are.
UINT ColorCorrectionViews(std::array<UINT, 4>& views, std::array<float, 4>& weights, float& defaultWeight);

}
