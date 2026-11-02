#pragma once

#include "descriptor_heap.h"

#include <d3d12.h>
#include <windows.h>

namespace gmodpp {

// The in-game settings menu (Dear ImGui), drawn into the swap chain the game is presented with. Insert opens and
// closes it; while it is open the game gets no keyboard or mouse input.
namespace overlay {

// `queue` is the one the menu's commands run on; `descriptors` holds its font texture.
bool Initialize(HWND window, ID3D12Device* device, ID3D12CommandQueue* queue, DescriptorHeap& descriptors, DXGI_FORMAT format, UINT framesInFlight);
bool Open();
// Whether there is anything to draw: the menu, or the notice of background work (shaders being compiled, ray tracing
// being prepared) shown in a corner of the screen while the loading screen is not up.
bool Visible();
// Puts the menu's window procedure back in front if the game replaced it; call once per frame.
void EnsureWindowHook();
// Records the menu and the notice into `list`, whose render target `rtv` is bound by this call. Only call while Visible().
void Render(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv);

}

}
