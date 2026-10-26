#pragma once

#include <d3d12.h>
#include <d3d9.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace gmodpp {

struct Options;
class Presenter;

struct CameraConstants {
    float eyePos[4] = {};
    float viewProj[4][4] = {};
};

class Renderer {
public:
    static Renderer* Create(LUID adapterLuid, const Options& options);

    ID3D12Device5* Device() const { return m_device.Get(); }
    ID3D12CommandQueue* Queue() const { return m_queue.Get(); }
    const LUID& AdapterLuid() const { return m_adapterLuid; }

    bool AttachGameDevice(IDirect3DDevice9* device, HWND focusWindow, const D3DPRESENT_PARAMETERS& params);
    Presenter* GetPresenter() const { return m_presenter.get(); }
    const CameraConstants& Camera() const { return m_camera; }
    bool SupportsRaytracing() const { return m_raytracing; }
    void OnVertexShaderConstants(UINT startRegister, const float* data, UINT vector4fCount);
    void OnPresent();
    void OnReset();

private:
    void DrainDebugMessages();

    Microsoft::WRL::ComPtr<ID3D12Device5> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_infoQueue;
    std::unique_ptr<Presenter> m_presenter;
    LUID m_adapterLuid = {};
    bool m_raytracing = false;
    uint64_t m_frame = 0;
    CameraConstants m_camera;
};

Renderer* GetRenderer();

}
