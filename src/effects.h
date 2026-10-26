#pragma once

#include <d3d12.h>
#include <d3d9.h>

#include <cstdint>

namespace gmodpp {

class Scene;

// Mirrors FrameConstants in shaders/common.hlsli.
struct FrameConstants {
    float invViewProj[4][4];
    float eyePosition[3];
    float sunEnabled;
    float sunDirection[3];
    float ambientRatio;
    float invSize[2];
    uint32_t size[2];
    float shadowStrength;
    float bakedRadius;
    float normalBias;
    uint32_t debugView;
};

// Everything an effect needs to record its work for one frame. Effects read the scene depth and the
// previous effect's output through `inputTable` (t1 depth, t2 color) and write `outputTable` (u0).
struct EffectFrame {
    ID3D12GraphicsCommandList4* list;
    D3D12_GPU_VIRTUAL_ADDRESS constants;
    D3D12_GPU_VIRTUAL_ADDRESS tlas;
    D3D12_GPU_DESCRIPTOR_HANDLE inputTable;
    UINT width;
    UINT height;
    const Scene& scene;
};

class Effect {
public:
    virtual ~Effect() = default;
    virtual const char* Name() const = 0;
    virtual bool Initialize(ID3D12Device5* device) = 0;
    virtual void Record(const EffectFrame& frame) = 0;
};

namespace effects {

// Called from the D3D9 hooks. Returns false when the device cannot provide a readable depth buffer.
bool PrepareDeviceParameters(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS& params);
void OnDeviceReady(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& params);
void OnBeforeReset();
void OnPresent();

// Called from client.dll when a 3D view has drawn its opaque geometry.
void OnOpaqueViewFinished();

}

}
