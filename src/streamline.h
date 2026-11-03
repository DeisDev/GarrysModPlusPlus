#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>

namespace gmodpp {

struct PostView;

namespace streamline {

// NVIDIA Streamline 2.14.16 (MIT) with its signed plugins in gmodplusplus: Reflex (low latency mode and PC latency
// markers) and DLSS Frame Generation, through Streamline's manual hooking (ProgrammingGuideManualHooking.md): only the
// swap chain, its factory and the queue go through Streamline's proxies. Without sl.interposer.dll, a valid NVIDIA
// signature or slInit, nothing here does anything.

// Before the device and the DXGI factory are created. False when Streamline is unavailable (logged).
bool Initialize(const std::filesystem::path& directory);
bool Active();
// After the device is created: tells Streamline the device and checks what its adapter supports.
void SetDevice(ID3D12Device* device, const LUID& adapter);
// The device to create the queue with and the factory to create the swap chain with: Streamline's proxies, which hook
// those calls (sl_hooks.h), or the native ones. Keep them as long as the device: Streamline still uses its proxies after
// those calls (the swap chain's creation called into a released device proxy).
Microsoft::WRL::ComPtr<ID3D12Device> QueueDevice(ID3D12Device* device);
Microsoft::WRL::ComPtr<IDXGIFactory2> SwapChainFactory(IDXGIFactory2* factory);

// The frame's latency markers (ProgrammingGuidePCL.md): BeginFrame after the last present takes the frame's token,
// applies the menu's Reflex and Frame Generation choices and sleeps for Reflex, then marks the simulation's start;
// MarkRenderStart (the first view drawn) its end and the render submission's start; MarkPresent around Present.
void BeginFrame();
void MarkRenderStart();
void MarkPresent(bool end);
// After each present: the frames Frame Generation presented since the last call, generated ones included; 1 while it
// is off.
uint32_t PresentedFrames();

// DLSS Frame Generation's inputs for the frame, recorded into `list`: the main view's device depth (R32_FLOAT) and motion
// in its pixels toward last frame's position, both the view's size, and the back buffer before the HUD, all in `state`
// and left alone until the present; with the camera's constants.
struct FrameInputs {
    ID3D12Resource* depth;
    ID3D12Resource* motion;
    ID3D12Resource* hudless;
    D3D12_RESOURCE_STATES state;
    // The view in the back buffer.
    D3D12_RECT view;
};
void TagFrame(ID3D12GraphicsCommandList* list, const FrameInputs& inputs, const PostView& post);
// Whether Frame Generation wants this frame's inputs.
bool FrameGenerationWanted();

bool ReflexAvailable();
bool FrameGenerationAvailable();
// The most frames Frame Generation can generate between two rendered ones on this GPU.
uint32_t MaxGeneratedFrames();

}

}
