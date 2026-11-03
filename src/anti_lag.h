#pragma once

#include <d3d12.h>

namespace gmodpp::anti_lag {

// AMD Anti-Lag 2 (its SDK's header, from FSR SDK 2.4.0) through the AMD driver's extension (amdxc64.dll). Without it
// these do nothing.

// After the device is created. False when the driver has no Anti-Lag 2 (logged).
bool Initialize(ID3D12Device* device);
// Before the game reads input for a frame: turns it on or off as Settings::antiLag says, and waits for it.
void Update();
// Once the frame's rendering is submitted, before Present.
void MarkEndOfFrameRendering();
// With FSR Frame Generation, right before each frame is presented: whether it is a generated one.
void SetFrameType(bool generated);

}
