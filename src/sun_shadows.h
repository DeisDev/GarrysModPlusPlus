#pragma once

#include "effects.h"

#include <wrl/client.h>

namespace gmodpp {

class SunShadows final : public Effect {
public:
    const char* Name() const override { return "sun shadows"; }
    bool Initialize(ID3D12Device5* device) override;
    void Record(const EffectFrame& frame) override;

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
};

}
