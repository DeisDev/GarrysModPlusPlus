#pragma once

#include "view_events.h"

#include <d3d9.h>

namespace gmodpp::effects {

// Called from the D3D9 hooks. Returns false when the device cannot provide a readable depth buffer.
bool PrepareDeviceParameters(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS& params);
void OnDeviceReady(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& params);
void OnBeforeReset();
void OnPresent();

// The viewmodel is drawn with its own depth range (viewport z 0..0.1); its capture follows that range.
void OnViewport(const D3DVIEWPORT9& viewport);
// Before the game copies `source` (StretchRect); post-processing copies the back buffer straight after the viewmodel.
void OnBeforeStretchRect(IDirect3DSurface9* source);

}
