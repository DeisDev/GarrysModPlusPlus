#pragma once

#include "descriptor_heap.h"
#include "lighting.h"
#include "view_types.h"

#include <d3d12.h>

#include <array>
#include <filesystem>
#include <optional>

// The ray-traced scene of the loaded map and its entities, and the lighting that uses it.
namespace gmodpp::world {

// Creates the lighting on the device the game's frames go to; false (logged) when it cannot.
bool Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue, DescriptorHeap& descriptors, bool opacityMicromaps,
    const std::filesystem::path& dlssDirectory);
// Waits for the lighting's commands and destroys it, with the scene.
void Shutdown();
// Whether views can be lit: the lighting exists and the map's scene is loaded.
bool Ready();
void WaitForGpu();

// Once per presented frame, with the size of the main view: installs the engine hooks, follows the settings file,
// loads the map's scene and, every few frames, the developer requests (probe.request and, with
// `consoleRequests`, console.txt).
void OnPresent(UINT width, UINT height, bool consoleRequests);

// The clip-space jitter (NDC units) for the next captured view of an output this size, for temporal anti-aliasing
// or Ray Reconstruction; zero without either.
std::array<float, 2> ViewJitter(UINT width, UINT height);

// Lights a captured view. Fills in the frame, the jitter's pixel offset and the local player, and collects the
// entities and dynamic lights the view is lit with.
void LightView(ViewInputs& inputs);

// What the forward-lit translucent surfaces of the main view read, once its irradiance volume is ready.
std::optional<gbuffer::ForwardConstants> ForwardLighting(const ViewInputs& inputs);

// The fence lighting's commands signal and its last value, for handing resources back.
ID3D12Fence* Fence();
uint64_t FenceValue();

}
