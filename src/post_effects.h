#pragma once

#include "display_output.h"

#include <d3d12.h>

namespace gmodpp::native {

// Runs the camera effects and the colour grading (the menu's Camera and Image pages) on `rect` of the back buffer, where
// the game has just drawn the main view and its post-processing (DoEnginePostProcessing), before its screen-space
// effects and the HUD: motion blur, depth of field (AMD FidelityFX DoF), the lens (distortion, chromatic aberration, the
// lens flare, vignetting), contrast adaptive sharpening (AMD FidelityFX CAS), grading and film grain. Shaders in
// shaders/post and shaders/amd.
void PostProcessView(D3D12_RECT rect, bool postVgui);

// DLSS Frame Generation's UI alpha at the present (Streamline's kBufferTypeUIAlpha): when this frame's view without the
// HUD was tagged and is the back buffer's size and format, points `mapping` at it and at the UI alpha's target, so the
// display mapping draws the mask as it maps the frame (UiAlphaMain in shaders/post/display_output.hlsl), and returns true;
// then TagUiAlpha tags the mask.
bool PrepareUiAlpha(display::Mapping& mapping, UINT width, UINT height);
void TagUiAlpha();

}
