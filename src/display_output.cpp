#include "display_output.h"

#include "engine_interfaces.h"
#include "log.h"
#include "settings.h"
#include "shaderapi_device.h"
#include "shaderapi_pipelines.h"
#include "shaderapi_textures.h"

#include <wrl/client.h>

#include <g_displayOutputPS.h>
#include <g_displayOutputVS.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp::native::display {

namespace {

constexpr UINT kInvalid = DescriptorHeap::kInvalid;
// How often (in presents) the gamma ramp's console variables are read again.
constexpr unsigned kGammaPollPresents = 30;

// Mirrors DisplayConstants in shaders/post/display_output.hlsl, in 16-byte rows.
struct DisplayConstants {
    std::int32_t origin[2];
    std::uint32_t frameView;
    std::uint32_t uiView;
    std::uint32_t encoding;
    std::uint32_t exposureView;
    float paperWhite;
    float blackLevel;
    float saturation;
    float uiWhite;
    float ditherSteps;
    std::uint32_t gammaRamp;
    float gammaExponent;
    float tvExponent;
    float tvMin;
    float tvMax;
    std::uint32_t debugView;
    float displayPeak;
    std::uint32_t padding[2];
};
static_assert(sizeof(DisplayConstants) == 80);

// IShaderDevice::SetHardwareGammaRamp's arguments, from the console variables materialsystem passes it
// (mat_monitorgamma and its TV range).
struct GammaRamp {
    float gamma = 2.2f;
    float tvMin = 16.0f;
    float tvMax = 255.0f;
    float tvExponent = 2.5f;
    bool tvEnabled = false;
};

struct UiTarget {
    TrackedResource resource;
    D3D12_CPU_DESCRIPTOR_HANDLE target = {};
    UINT view = kInvalid;
    UINT width = 0;
    UINT height = 0;
    bool failed = false;
};

struct State {
    ComPtr<ID3D12RootSignature> root;
    std::unordered_map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> pipelines;
    bool failed = false;
    GammaRamp gamma;
    unsigned presents = 0;
    UiTarget ui;
    ColorCorrection correction;
    UINT exposureView = kInvalid;
};

State g_state;

float ConVarFloat(const char* name, float fallback)
{
    const std::optional<std::string> value = engine::ConVarString(name);
    if (!value)
        return fallback;
    char* end = nullptr;
    const float parsed = std::strtof(value->c_str(), &end);
    return end != value->c_str() ? parsed : fallback;
}

const GammaRamp& Gamma()
{
    if (g_state.presents++ % kGammaPollPresents == 0) {
        GammaRamp& ramp = g_state.gamma;
        ramp.gamma = ConVarFloat("mat_monitorgamma", ramp.gamma);
        ramp.tvMin = ConVarFloat("mat_monitorgamma_tv_range_min", ramp.tvMin);
        ramp.tvMax = ConVarFloat("mat_monitorgamma_tv_range_max", ramp.tvMax);
        ramp.tvExponent = ConVarFloat("mat_monitorgamma_tv_exp", ramp.tvExponent);
        ramp.tvEnabled = ConVarFloat("mat_monitorgamma_tv_enabled", ramp.tvEnabled ? 1.0f : 0.0f) != 0.0f;
    }
    return g_state.gamma;
}

ID3D12PipelineState* Pipeline(DXGI_FORMAT format)
{
    if (g_state.failed)
        return nullptr;
    Backend& backend = GetBackend();
    if (!g_state.root) {
        const HRESULT hr = backend.Device()->CreateRootSignature(0, g_displayOutputPS, sizeof(g_displayOutputPS), IID_PPV_ARGS(&g_state.root));
        if (FAILED(hr)) {
            log::Info("Display: creating the root signature failed: {:#010x}", static_cast<unsigned>(hr));
            g_state.failed = true;
            return nullptr;
        }
    }
    ComPtr<ID3D12PipelineState>& pipeline = g_state.pipelines[format];
    if (!pipeline) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = g_state.root.Get();
        desc.VS = {g_displayOutputVS, sizeof(g_displayOutputVS)};
        desc.PS = {g_displayOutputPS, sizeof(g_displayOutputPS)};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = format;
        desc.SampleDesc.Count = 1;
        const HRESULT hr = backend.Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr)) {
            log::Info("Display: creating the pipeline for format {} failed: {:#010x}", static_cast<int>(format), static_cast<unsigned>(hr));
            g_state.pipelines.erase(format);
            return nullptr;
        }
    }
    return pipeline.Get();
}

}

Mapping ForOutput(bool hdr, UINT bits)
{
    Mapping mapping;
    mapping.encoding = hdr ? Encoding::Pq : Encoding::Srgb;
    mapping.ditherSteps = bits >= 10 ? 1023.0f : 255.0f;
    mapping.gammaRamp = !hdr;
    if (hdr) {
        const Settings& settings = GetSettings();
        const Statistics& statistics = GetStatistics();
        mapping.paperWhite = settings.paperWhiteFollowWindows && statistics.sdrWhiteNits > 0.0f ? statistics.sdrWhiteNits : settings.paperWhite;
        const float peak = settings.peakFollowDisplay && statistics.displayPeakNits > 0.0f ? statistics.displayPeakNits : settings.peakBrightness;
        mapping.peak = std::max(peak / mapping.paperWhite, 1.0f);
        mapping.blackLevel = settings.hdrBlackLevel;
        mapping.saturation = settings.hdrSaturation;
        mapping.uiWhite = settings.uiBrightness;
        mapping.displayPeak = peak;
        mapping.exposureView = g_state.exposureView;
    }
    const DebugView debugView = GetSettings().debugView;
    mapping.debugView = debugView == DebugView::Luminance || debugView == DebugView::OverPeak ? static_cast<std::uint32_t>(debugView) : 0u;
    return mapping;
}

Mapping ForScreenshot()
{
    Mapping mapping;
    mapping.encoding = Encoding::Srgb;
    // Direct3D 9's gamma ramp changed what the display showed, not the back buffer that screenshots read.
    mapping.gammaRamp = false;
    return mapping;
}

bool Map(ID3D12GraphicsCommandList* commands, UINT frameView, D3D12_CPU_DESCRIPTOR_HANDLE target, DXGI_FORMAT format, UINT width, UINT height,
    int originX, int originY, const Mapping& mapping)
{
    ID3D12PipelineState* pipeline = Pipeline(format);
    if (pipeline == nullptr || width == 0 || height == 0)
        return false;
    Backend& backend = GetBackend();
    DisplayConstants c = {};
    c.origin[0] = originX;
    c.origin[1] = originY;
    c.frameView = frameView;
    c.uiView = mapping.uiView;
    c.encoding = static_cast<std::uint32_t>(mapping.encoding);
    c.exposureView = mapping.exposureView;
    c.paperWhite = mapping.paperWhite;
    c.blackLevel = mapping.blackLevel;
    c.saturation = mapping.saturation;
    c.uiWhite = mapping.uiWhite;
    c.ditherSteps = mapping.ditherSteps;
    if (mapping.gammaRamp && mapping.encoding == Encoding::Srgb) {
        const GammaRamp& ramp = Gamma();
        // shaderapidx9.dll 0x180027c80: each entry is pow(i / 255, gamma / 2.2); with the TV range, that again to the
        // power 2.2 / its exponent, moved into [min, max] / 255.
        c.gammaRamp = ramp.tvEnabled ? 2u : ramp.gamma != 2.2f ? 1u : 0u;
        c.gammaExponent = ramp.gamma / 2.2f;
        c.tvExponent = ramp.tvExponent > 0.0f ? 2.2f / ramp.tvExponent : 1.0f;
        c.tvMin = ramp.tvMin / 255.0f;
        c.tvMax = ramp.tvMax / 255.0f;
    }
    c.debugView = mapping.debugView;
    c.displayPeak = mapping.displayPeak;
    const D3D12_GPU_VIRTUAL_ADDRESS constants = UploadConstants(&c, sizeof(c));
    if (constants == 0)
        return false;
    ID3D12DescriptorHeap* heaps[] = {backend.Descriptors().Heap(), backend.SamplerHeap()};
    commands->SetDescriptorHeaps(2, heaps);
    commands->SetGraphicsRootSignature(g_state.root.Get());
    commands->SetPipelineState(pipeline);
    commands->SetGraphicsRootConstantBufferView(0, constants);
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const D3D12_RECT scissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
    ForgetBindings();
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE BeginUi(UINT width, UINT height, UINT& view)
{
    Backend& backend = GetBackend();
    UiTarget& ui = g_state.ui;
    view = kInvalid;
    if (ui.failed)
        return {};
    if (!ui.resource.resource || ui.width != width || ui.height != height) {
        if (ui.resource.resource)
            backend.Retire(std::move(ui.resource.resource));
        ui.resource = {};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kUiFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = kUiFormat;
        const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        const HRESULT hr = backend.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
            IID_PPV_ARGS(&ui.resource.resource));
        if (FAILED(hr)) {
            log::Info("Display: creating the menu's {}x{} texture failed: {:#010x}", width, height, static_cast<unsigned>(hr));
            ui.failed = true;
            return {};
        }
        ui.resource.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (ui.target.ptr == 0)
            ui.target = backend.RenderTargetViews().Allocate();
        if (ui.view == kInvalid)
            ui.view = backend.Descriptors().Allocate();
        backend.Device()->CreateRenderTargetView(ui.resource.resource.Get(), nullptr, ui.target);
        backend.Device()->CreateShaderResourceView(ui.resource.resource.Get(), nullptr, backend.Descriptors().Cpu(ui.view));
        ui.width = width;
        ui.height = height;
    }
    backend.Transition(ui.resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
    constexpr float kTransparent[4] = {};
    backend.Commands()->ClearRenderTargetView(ui.target, kTransparent, 0, nullptr);
    view = ui.view;
    return ui.target;
}

void EndUi() { GetBackend().Transition(g_state.ui.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); }

namespace {

std::string Utf8(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? static_cast<size_t>(size) - 1 : 0, '\0');
    if (size > 1)
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

// Windows' SDR white level and the monitor's name for the display path whose source is the GDI device `device`
// (QueryDisplayConfig, DisplayConfigGetDeviceInfo).
void ReadDisplayConfig(DisplayDesc& display)
{
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return;
    for (UINT32 i = 0; i < pathCount; ++i) {
        const DISPLAYCONFIG_PATH_INFO& path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || display.device != source.viewGdiDeviceName)
            continue;
        // SDRWhiteLevel: the SDR white level in multiples of 80 nits, times 1000.
        DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS)
            display.sdrWhiteNits = static_cast<float>(white.SDRWhiteLevel) / 1000.0f * 80.0f;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0] != L'\0')
            display.name = Utf8(target.monitorFriendlyDeviceName);
        return;
    }
}

}

std::string Utf8Name(const std::wstring& device) { return Utf8(device.c_str()); }

std::optional<DisplayDesc> FindDisplay(IDXGIFactory1* factory, LUID adapterLuid, HWND window)
{
    ComPtr<IDXGIFactory4> factory4;
    ComPtr<IDXGIAdapter1> adapter;
    if (factory == nullptr || FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory4))) || FAILED(factory4->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(&adapter))))
        return std::nullopt;
    RECT area = {};
    if (window == nullptr || !GetWindowRect(window, &area))
        return std::nullopt;
    std::optional<DisplayDesc> best;
    LONG bestArea = 0;
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIOutput6> output6;
        DXGI_OUTPUT_DESC1 desc = {};
        if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc)))
            continue;
        const RECT& d = desc.DesktopCoordinates;
        const LONG width = std::max(0L, std::min(area.right, d.right) - std::max(area.left, d.left));
        const LONG height = std::max(0L, std::min(area.bottom, d.bottom) - std::max(area.top, d.top));
        if (width * height <= bestArea)
            continue;
        bestArea = width * height;
        DisplayDesc display;
        display.device = desc.DeviceName;
        display.name = Utf8(desc.DeviceName);
        display.hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        display.peakNits = desc.MaxLuminance;
        display.fullFrameNits = desc.MaxFullFrameLuminance;
        display.blackNits = desc.MinLuminance;
        display.bits = desc.BitsPerColor;
        display.desktop = d;
        best = display;
    }
    if (best)
        ReadDisplayConfig(*best);
    return best;
}

void SetColorCorrection(const ColorCorrection& correction) { g_state.correction = correction; }

UINT ColorCorrectionViews(std::array<UINT, 4>& views, std::array<float, 4>& weights, float& defaultWeight)
{
    Backend& backend = GetBackend();
    const ColorCorrection& correction = g_state.correction;
    defaultWeight = correction.defaultWeight;
    UINT count = 0;
    for (; count < std::min<UINT>(correction.count, 4); ++count) {
        Texture* texture = TextureFromHandle(correction.textures[count]);
        if (texture == nullptr || texture->copies.empty() || texture->depth <= 1)
            break;
        Texture::Copy& copy = texture->Current();
        backend.Transition(copy.resource, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        views[count] = copy.view;
        weights[count] = correction.weights[count];
    }
    return count;
}

void ClearColorCorrection() { g_state.correction = {}; }

void SetHdrExposure(UINT view) { g_state.exposureView = view; }

void ClearHdrExposure() { g_state.exposureView = kInvalid; }

}
