#include "restir.h"

#include "log.h"

#include <Rtxdi/RtxdiUtils.h>

#include <algorithm>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

// RTXDI's neighbour offsets for spatial reuse (ReSTIRDIStaticParameters::NeighborOffsetCount).
constexpr uint32_t kNeighborOffsets = 8192;
// Room in the light PDF for the engine's dynamic lights and the projected lights (Scene's and Lighting's maxima).
constexpr uint32_t kFrameLights = 32 + 8;
constexpr uint32_t kStyles = 128;

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES properties = {heap};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer))))
        return nullptr;
    return buffer;
}

}

bool Restir::Initialize(ID3D12Device* device, DescriptorHeap& descriptors)
{
    m_device = device;
    m_descriptors = &descriptors;
    m_localLightRisOffset = m_risAllocator.AllocateSegment(kTileSize * kTileCount);
    rtxdi::ReGIRStaticParameters regir;
    regir.lightsPerCell = kRegirLightsPerCell;
    m_regir = std::make_unique<rtxdi::ReGIRContext>(regir, m_risAllocator);
    m_ris = CreateBuffer(m_device, D3D12_HEAP_TYPE_DEFAULT, static_cast<UINT64>(m_risAllocator.GetTotalSizeInElements()) * 8,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    std::vector<uint8_t> packed(kNeighborOffsets * 2);
    rtxdi::FillNeighborOffsetBuffer(packed.data(), kNeighborOffsets);
    // Signed bytes as R8G8_SNORM reads them.
    std::vector<float> offsets(packed.size());
    for (size_t i = 0; i < packed.size(); ++i)
        offsets[i] = std::max(static_cast<float>(static_cast<int8_t>(packed[i])) / 127.0f, -1.0f);
    m_neighborOffsets = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, offsets.size() * sizeof(float), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    if (m_neighborOffsets && SUCCEEDED(m_neighborOffsets->Map(0, nullptr, &mapped))) {
        std::memcpy(mapped, offsets.data(), offsets.size() * sizeof(float));
        m_neighborOffsets->Unmap(0, nullptr);
    }
    m_previousStyles = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, kSlots * kStyles * sizeof(float), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (m_previousStyles && FAILED(m_previousStyles->Map(0, nullptr, reinterpret_cast<void**>(&m_previousStylesMapped))))
        m_previousStyles.Reset();
    for (UINT slot = 0; slot < kSlots && m_previousStyles; ++slot)
        m_previousStylesViews[slot] = m_descriptors->CreateRawView(m_previousStyles.Get(), slot * kStyles * sizeof(float), kStyles * sizeof(float));
    m_pdfView = m_descriptors->Allocate();
    m_pdfMipUavs = m_descriptors->Reserve(kMaxPdfMips);
    if (!m_ris || !m_neighborOffsets || !mapped || !m_previousStyles || m_pdfView == DescriptorHeap::kInvalid || m_pdfMipUavs == DescriptorHeap::kInvalid) {
        log::Info("RTXDI: creating its buffers failed; local lights are sampled from the light grid");
        return false;
    }
    log::Info("RTXDI: {} ReGIR cells of {} light slots, {} presampling tiles of {} lights", m_regir->GetReGIROnionCalculatedParameters().regirOnionCells,
        m_regir->GetReGIRStaticParameters().lightsPerCell, kTileCount, kTileSize);
    return true;
}

bool Restir::CreatePdf(uint32_t lights)
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips = 0;
    rtxdi::ComputePdfTextureSize(lights, width, height, mips);
    m_pdf.Reset();
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(std::min<uint32_t>(mips, kMaxPdfMips));
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (mips > kMaxPdfMips || FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                  IID_PPV_ARGS(&m_pdf))))
        return false;
    m_pdfState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = mips;
    m_device->CreateShaderResourceView(m_pdf.Get(), &srv, m_descriptors->Cpu(m_pdfView));
    for (UINT mip = 0; mip < mips; ++mip) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = desc.Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Texture2D.MipSlice = mip;
        m_device->CreateUnorderedAccessView(m_pdf.Get(), nullptr, &uav, m_descriptors->Cpu(m_pdfMipUavs + mip));
    }
    m_pdfCapacity = width * height;
    m_pdfSize[0] = width;
    m_pdfSize[1] = height;
    m_pdfMips = mips;
    return true;
}

bool Restir::CreateReservoirs(UINT width, UINT height)
{
    rtxdi::ReSTIRDIStaticParameters parameters;
    parameters.NeighborOffsetCount = kNeighborOffsets;
    parameters.RenderWidth = width;
    parameters.RenderHeight = height;
    m_context = std::make_unique<rtxdi::ReSTIRDIContext>(parameters);
    const RTXDI_ReservoirBufferParameters buffers = m_context->GetReservoirBufferParameters();
    m_reservoirs = CreateBuffer(m_device, D3D12_HEAP_TYPE_DEFAULT,
        static_cast<UINT64>(buffers.reservoirArrayPitch) * rtxdi::c_NumReSTIRDIReservoirBuffers * sizeof(RTXDI_PackedDIReservoir),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!m_reservoirs)
        return false;
    m_width = width;
    m_height = height;
    return true;
}

bool Restir::NeedsResources(UINT width, UINT height, const RestirLights& lights) const
{
    const uint32_t wanted = std::max(lights.mapLights + lights.emitters + kFrameLights, 1u);
    const bool pdf = !m_pdf || m_pdfCapacity < lights.Total() || m_pdfCapacity / 4 >= wanted;
    return !m_failed && (pdf || !m_reservoirs || width != m_width || height != m_height);
}

bool Restir::Prepare(UINT width, UINT height, const RestirLights& lights)
{
    if (m_failed || !m_ris)
        return false;
    const uint32_t wanted = std::max(lights.mapLights + lights.emitters + kFrameLights, 1u);
    const bool pdf = !m_pdf || m_pdfCapacity < lights.Total() || m_pdfCapacity / 4 >= wanted;
    const bool reservoirs = !m_reservoirs || width != m_width || height != m_height;
    if ((pdf && !CreatePdf(wanted)) || (reservoirs && !CreateReservoirs(width, height))) {
        log::Info("RTXDI: creating its {}x{} reservoirs or its light PDF for {} lights failed; local lights are sampled from the light grid", width,
            height, wanted);
        m_failed = true;
        return false;
    }
    return true;
}

void Restir::FillRegir(ReGIR_Parameters& parameters) const
{
    parameters = {};
    const rtxdi::ReGIRStaticParameters fixed = m_regir->GetReGIRStaticParameters();
    const rtxdi::ReGIRDynamicParameters dynamic = m_regir->GetReGIRDynamicParameters();
    const rtxdi::ReGIROnionCalculatedParameters onion = m_regir->GetReGIROnionCalculatedParameters();
    ReGIR_CommonParameters& common = parameters.commonParams;
    common.localLightSamplingFallbackMode = static_cast<uint32_t>(dynamic.fallbackSamplingMode);
    common.centerX = dynamic.center.x;
    common.centerY = dynamic.center.y;
    common.centerZ = dynamic.center.z;
    common.risBufferOffset = m_regir->GetReGIRCellOffset();
    common.lightsPerCell = fixed.lightsPerCell;
    // The onion works in radii, so half the size, as RTXDI's sample does (LightingPasses.cpp).
    common.cellSize = dynamic.regirCellSize * 0.5f;
    // The jitter is plus or minus regirSamplingJitter cells: the shaders scale a [-0.5, 0.5] offset by it.
    common.samplingJitter = std::max(0.0f, dynamic.regirSamplingJitter * 2.0f);
    common.localLightPresamplingMode = static_cast<uint32_t>(dynamic.presamplingMode);
    common.numRegirBuildSamples = dynamic.regirNumBuildSamples;
    parameters.gridParams = {fixed.gridParameters.gridSize.x, fixed.gridParameters.gridSize.y, fixed.gridParameters.gridSize.z, 0};
    ReGIR_OnionParameters& rings = parameters.onionParams;
    const size_t layerGroups = std::min<size_t>(onion.regirOnionLayers.size(), RTXDI_ONION_MAX_LAYER_GROUPS);
    std::copy_n(onion.regirOnionLayers.begin(), layerGroups, rings.layers);
    // The context builds the radii in cells; the shaders compare world-space distances with them.
    for (size_t g = 0; g < layerGroups; ++g) {
        rings.layers[g].innerRadius *= common.cellSize;
        rings.layers[g].outerRadius *= common.cellSize;
    }
    std::copy_n(onion.regirOnionRings.begin(), std::min<size_t>(onion.regirOnionRings.size(), RTXDI_ONION_MAX_RINGS), rings.rings);
    rings.numLayerGroups = static_cast<uint32_t>(layerGroups);
    rings.cubicRootFactor = onion.regirOnionCubicRootFactor;
    rings.linearFactor = onion.regirOnionLinearFactor;
}

void Restir::FillConstants(RestirConstants& constants, uint32_t frameIndex, RestirMode mode, bool rayReconstruction, const float eye[3],
    const float previousInvViewProj[4][4], const RestirLights& lights, float emitterPower, UINT previousLightStylesView, bool historyValid)
{
    m_context->SetFrameIndex(frameIndex);
    m_context->SetResamplingMode(mode == RestirMode::Reuse ? rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial : rtxdi::ReSTIRDI_ResamplingMode::None);
    RTXDI_DIInitialSamplingParameters initial = rtxdi::GetDefaultReSTIRDIInitialSamplingParams();
    initial.numLocalLightSamples = 8;
    initial.numInfiniteLightSamples = 0;
    initial.numEnvironmentSamples = 0;
    initial.environmentMapImportanceSampling = 0;
    // A BRDF ray finds glowing triangles that light sampling picks rarely; other lights are not in the scene's BVH.
    initial.numBrdfSamples = lights.emitters != 0 ? 1 : 0;
    // Source units are inches; the ray starts this far along its direction from the surface.
    initial.brdfRayMinT = 1.0f;
    initial.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
    m_context->SetInitialSamplingParameters(initial);
    m_context->SetTemporalResamplingParameters(rtxdi::GetDefaultReSTIRDITemporalResamplingParams());
    m_context->SetBoilingFilterParameters(rtxdi::GetDefaultReSTIRDIBoilingFilterParams());
    m_context->SetSpatialResamplingParameters(rtxdi::GetDefaultReSTIRDISpatialResamplingParams());
    RTXDI_ShadingParameters shading = rtxdi::GetDefaultReSTIRDIShadingParams();
    shading.reuseFinalVisibility = !rayReconstruction;
    m_context->SetShadingParameters(shading);
    rtxdi::ReGIRDynamicParameters regir = m_regir->GetReGIRDynamicParameters();
    regir.center = {eye[0], eye[1], eye[2]};
    regir.regirCellSize = kRegirCellSize;
    regir.presamplingMode = rtxdi::LocalLightReGIRPresamplingMode::Power_RIS;
    regir.fallbackSamplingMode = rtxdi::LocalLightReGIRFallbackSamplingMode::Power_RIS;
    m_regir->SetDynamicParameters(regir);

    constants = {};
    RTXDI_Parameters& restir = constants.restir;
    restir.reservoirBufferParams = m_context->GetReservoirBufferParameters();
    restir.bufferIndices = m_context->GetBufferIndices();
    // Without reuse the initial samples are shaded as they are.
    if (mode == RestirMode::Ris)
        restir.bufferIndices.shadingInputBufferIndex = restir.bufferIndices.initialSamplingOutputBufferIndex;
    restir.initialSamplingParams = m_context->GetInitialSamplingParameters();
    restir.temporalResamplingParams = m_context->GetTemporalResamplingParameters();
    restir.boilingFilterParams = m_context->GetBoilingFilterParameters();
    restir.spatialResamplingParams = m_context->GetSpatialResamplingParameters();
    restir.spatioTemporalResamplingParams = m_context->GetSpatioTemporalResamplingParameters();
    restir.shadingParams = m_context->GetShadingParameters();
    constants.lightBuffer.localLightBufferRegion = {0, lights.Total(), 0, 0};
    constants.runtime = m_context->GetRuntimeParams();
    constants.localLightRis = {m_localLightRisOffset, kTileSize, kTileCount, 0};
    FillRegir(constants.regir);
    std::memcpy(constants.previousInvViewProj, previousInvViewProj, sizeof(constants.previousInvViewProj));
    constants.mapLights = lights.mapLights;
    constants.emitters = lights.emitters;
    constants.dynamicLights = lights.dynamicLights;
    constants.projectors = lights.projectors;
    constants.pdfView = m_pdfView;
    constants.pdfMipUav = m_pdfMipUavs;
    constants.pdfSize[0] = m_pdfSize[0];
    constants.pdfSize[1] = m_pdfSize[1];
    constants.previousLightStylesView = previousLightStylesView;
    constants.emitterPower = emitterPower;
    constants.historyValid = historyValid ? 1u : 0u;
    constants.regirLightSlots = RegirLightSlots();
}

UINT Restir::UploadPreviousStyles(UINT slot, const float* values, size_t count)
{
    if (!m_previousStylesMapped)
        return DescriptorHeap::kInvalid;
    std::memcpy(m_previousStylesMapped + static_cast<size_t>(slot % kSlots) * kStyles * sizeof(float), values, std::min<size_t>(count, kStyles) * sizeof(float));
    return m_previousStylesViews[slot % kSlots];
}

}
