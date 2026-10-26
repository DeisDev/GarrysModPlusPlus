#pragma once

#include <d3d12.h>
#include <windows.h>

namespace gmodpp {

class Renderer;

// The in-game settings menu (Dear ImGui), drawn into the presenter's swap chain. Insert opens and closes it;
// while it is open the game gets no keyboard or mouse input.
namespace overlay {

bool Initialize(HWND window, Renderer& renderer, DXGI_FORMAT format, UINT framesInFlight);
bool Open();
// Puts the menu's window procedure back in front if the game replaced it; call once per frame.
void EnsureWindowHook();
// Records the menu into `list`, whose render target `rtv` is bound by this call. Only call while Open().
void Render(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv);

}

}
