#pragma once

#include "settings.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>

struct ffxFunctions;

namespace gmodpp {

struct PostView;

namespace fsr {

// The AMD FSR API (FSR SDK 2.4.0), loaded from amd_fidelityfx_loader_dx12.dll in `directory`, which finds the effect
// DLLs beside it. Null when the loader is missing or lacks its exports (logged once).
const ffxFunctions* Load(const std::filesystem::path& directory);
// Whether FSR Frame Generation runs on this device (logged).
bool FrameGenerationSupported(ID3D12Device* device, const std::filesystem::path& directory);

}

// AMD FSR upscaling (src/lighting.cpp, AntiAliasing::Fsr): FSR 4 on Radeon RX 9000 GPUs, FSR 3.1 elsewhere, as the FSR
// SDK's loader picks. Takes the main view's linear HDR light at render resolution with its depth and motion, and
// writes it at the output resolution.
class FsrUpscaler {
public:
    // False when FSR cannot run on this device (logged).
    bool Initialize(ID3D12Device* device, const std::filesystem::path& directory);
    bool Usable() const { return m_functions != nullptr && !m_failed; }
    // The render resolution for `mode` at an output size.
    bool RenderSize(UpscalingMode mode, UINT outputWidth, UINT outputHeight, UINT& width, UINT& height);

    struct Inputs {
        // Linear light, device depth (as drawn, jittered) and motion in render pixels toward last frame's position, all
        // in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; the output in UNORDERED_ACCESS.
        ID3D12Resource* color;
        ID3D12Resource* depth;
        ID3D12Resource* motion;
        ID3D12Resource* output;
        UINT renderWidth;
        UINT renderHeight;
        UINT outputWidth;
        UINT outputHeight;
        // The jitter in render pixels (x right, y down).
        float jitterX;
        float jitterY;
        float frameMilliseconds;
        float cameraNear;
        float cameraFar;
        float verticalFov;
        bool reversedDepth;
        bool reset;
    };
    // Records the upscale, leaving FSR's descriptor heaps bound. False when it failed (logged once; FSR is then not used).
    bool Evaluate(ID3D12GraphicsCommandList* list, const Inputs& inputs);
    // Destroys the context, which the GPU must have finished with.
    void Release();

private:
    bool EnsureContext(UINT outputWidth, UINT outputHeight, bool reversedDepth);

    const ffxFunctions* m_functions = nullptr;
    ID3D12Device* m_device = nullptr;
    void* m_context = nullptr;
    UINT m_contextSize[2] = {};
    bool m_contextReversed = false;
    bool m_failed = false;
};

// AMD FSR Ray Regeneration (DenoiserType::RayRegeneration): denoises the main view's indirect light and reflections in
// place of NRD. It needs a Radeon RX 9000 GPU (denoising.md, 6. Requirements).
class FsrRayRegeneration {
public:
    // False when Ray Regeneration cannot run on this device (logged).
    bool Initialize(ID3D12Device* device, const std::filesystem::path& directory);
    bool Usable() const { return m_functions != nullptr && !m_failed; }

    struct Inputs {
        // All in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE but the outputs, in UNORDERED_ACCESS (shaders/ray_regeneration.hlsl
        // lays them out): signed linear view depth, motion (texture coordinates and view depth to last frame), the
        // world normal with roughness, linear (not encoded) albedos, and diffuse and specular light with hit distance.
        ID3D12Resource* linearDepth;
        ID3D12Resource* motion;
        ID3D12Resource* normals;
        ID3D12Resource* diffuseAlbedo;
        ID3D12Resource* specularAlbedo;
        ID3D12Resource* diffuse;
        ID3D12Resource* specular;
        ID3D12Resource* diffuseOut;
        ID3D12Resource* specularOut;
        // Row-major matrices for row vectors (Ray Reconstruction's), the jitter in pixels, the eye's move since last
        // frame (last minus this) and the view depths denoised.
        const float* worldToView;
        const float* viewToClip;
        float jitterX;
        float jitterY;
        float cameraDelta[3];
        float maxDepth;
        UINT width;
        UINT height;
        // The size the context must cover, and the frame's index (one more than last frame's unless reset).
        UINT maxWidth;
        UINT maxHeight;
        uint32_t frame;
        bool reset;
    };
    // Records the denoising, leaving FSR's descriptor heaps bound. False when it failed (logged; NRD is used instead).
    bool Evaluate(ID3D12GraphicsCommandList* list, const Inputs& inputs);
    void Release();

private:
    const ffxFunctions* m_functions = nullptr;
    ID3D12Device* m_device = nullptr;
    void* m_context = nullptr;
    UINT m_contextSize[2] = {};
    bool m_failed = false;
};

// AMD FSR Frame Generation (FrameGeneration::Fsr): FSR's swap chain takes the DXGI one's place when the device is made,
// paces the presents on its own threads and generates a frame between two of the game's from what Prepare records
// (frame-interpolation-api.md, frame-interpolation-swap-chain.md). Its present callback copies each frame to the
// display and tells AMD Anti-Lag 2 which frames are generated.
class FsrFrameGeneration {
public:
    // A swap chain for `window` made through FSR, or null when FSR cannot make one (logged).
    Microsoft::WRL::ComPtr<IDXGISwapChain4> CreateSwapChain(ID3D12Device* device, IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND window,
        const DXGI_SWAP_CHAIN_DESC1& desc);
    bool Active() const { return m_swapChainContext != nullptr && !m_failed; }

    struct Inputs {
        // Device depth (R32_FLOAT) and motion in the view's pixels toward last frame's position, both the view's size, and
        // the back buffer before the HUD; all in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE.
        ID3D12Resource* depth;
        ID3D12Resource* motion;
        ID3D12Resource* hudless;
        // The view in the back buffer, and the back buffer's size.
        D3D12_RECT view;
        UINT displayWidth;
        UINT displayHeight;
        float frameMilliseconds;
    };
    // Records the frame's preparation for generation into `list` and turns generation on for it.
    void Prepare(ID3D12GraphicsCommandList* list, const Inputs& inputs, const PostView& post);
    // Right before Present: turns generation off for a frame Prepare did not see, and counts the frame.
    void BeforePresent();
    // Before the swap chain's buffers are resized (the GPU idle): generation's context is made again at the new size.
    void ReleaseContext();

private:
    bool EnsureContext(UINT width, UINT height, bool reversedDepth);
    bool Configure(bool enabled, ID3D12Resource* hudless, const D3D12_RECT& view);

    const ffxFunctions* m_functions = nullptr;
    ID3D12Device* m_device = nullptr;
    // Owned by the backend, which holds the swap chain.
    IDXGISwapChain4* m_swapChain = nullptr;
    void* m_swapChainContext = nullptr;
    void* m_context = nullptr;
    UINT m_contextSize[2] = {};
    bool m_contextReversed = false;
    uint64_t m_frameId = 0;
    bool m_prepared = false;
    bool m_preparedLast = false;
    bool m_failed = false;
};

}
