#include "sun_shadows.h"

#include "log.h"

#include <g_sunShadowsCS.h>

namespace gmodpp {

bool SunShadows::Initialize(ID3D12Device5* device)
{
    HRESULT hr = device->CreateRootSignature(0, g_sunShadowsCS, sizeof(g_sunShadowsCS), IID_PPV_ARGS(&m_rootSignature));
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_rootSignature.Get();
        desc.CS = {g_sunShadowsCS, sizeof(g_sunShadowsCS)};
        hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_pipeline));
    }
    if (FAILED(hr))
        log::Info("Sun shadows: pipeline creation failed ({:#010x})", static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
}

void SunShadows::Record(const EffectFrame& frame)
{
    frame.list->SetComputeRootSignature(m_rootSignature.Get());
    frame.list->SetPipelineState(m_pipeline.Get());
    frame.list->SetComputeRootConstantBufferView(0, frame.constants);
    frame.list->SetComputeRootShaderResourceView(1, frame.tlas);
    frame.list->SetComputeRootDescriptorTable(2, frame.inputTable);
    frame.list->Dispatch((frame.width + 7) / 8, (frame.height + 7) / 8, 1);
}

}
