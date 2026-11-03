#include "anti_lag.h"

#include "log.h"
#include "settings.h"

#include <windows.h>

#include <ffx_antilag2_dx12.h>

namespace gmodpp::anti_lag {

namespace {

AMD::AntiLag2DX12::Context g_context = {};
bool g_available = false;

}

bool Initialize(ID3D12Device* device)
{
    const HRESULT hr = AMD::AntiLag2DX12::Initialize(&g_context, device);
    g_available = hr == S_OK;
    if (g_available)
        log::Info("Anti-Lag 2: available");
    else
        log::Info("Anti-Lag 2: unavailable ({:#010x}); it needs an AMD Radeon GPU and driver", static_cast<unsigned>(hr));
    return g_available;
}

void Update()
{
    if (g_available)
        AMD::AntiLag2DX12::Update(&g_context, GetSettings().antiLag, 0);
}

void MarkEndOfFrameRendering()
{
    if (g_available)
        AMD::AntiLag2DX12::MarkEndOfFrameRendering(&g_context);
}

void SetFrameType(bool generated)
{
    if (g_available)
        AMD::AntiLag2DX12::SetFrameGenFrameType(&g_context, generated);
}

}
