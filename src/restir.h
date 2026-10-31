#pragma once

#include "descriptor_heap.h"

#include <Rtxdi/DI/ReSTIRDI.h>
#include <Rtxdi/LightSampling/RISBufferSegmentAllocator.h>
#include <Rtxdi/ReGIR/ReGIR.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <memory>

namespace gmodpp {

// Mirrors RestirConstants in shaders/nvidia/rtxdi_bridge.hlsli.
struct RestirConstants {
    RTXDI_Parameters restir;
    RTXDI_LightBufferParameters lightBuffer;
    RTXDI_RuntimeParameters runtime;
    RTXDI_RISBufferSegmentParameters localLightRis;
    RTXDI_RISBufferSegmentParameters environmentRis;
    ReGIR_Parameters regir;
    float previousInvViewProj[4][4];
    uint32_t mapLights;
    uint32_t emitters;
    uint32_t dynamicLights;
    uint32_t projectors;
    uint32_t pdfView;
    uint32_t pdfMipUav;
    uint32_t pdfSize[2];
    uint32_t previousLightStylesView;
    float emitterPower;
    uint32_t historyValid;
    uint32_t regirLightSlots;
};

// What RTXDI lights in a frame: the map's lights, its glowing triangles, the engine's dynamic lights and the projected
// lights, in that order in one index space.
struct RestirLights {
    uint32_t mapLights = 0;
    uint32_t emitters = 0;
    uint32_t dynamicLights = 0;
    uint32_t projectors = 0;
    uint32_t Total() const { return mapLights + emitters + dynamicLights + projectors; }
};

// How the main view's local lights are sampled (shaders/restir.hlsl): RIS from ReGIR's cells alone, or with temporal
// and spatial reuse (ReSTIR DI).
enum class RestirMode { Ris, Reuse };

// NVIDIA RTXDI's state for the main view: the ReSTIR DI and ReGIR contexts (RTXDI-Library) and their GPU resources,
// the light PDF texture, the RIS buffer (presampled tiles, then ReGIR's cells), the reservoirs and the neighbour offsets.
class Restir {
public:
    // Light sampling RIS tiles: RTXDI's defaults (ImportanceSamplingContext_StaticParameters).
    static constexpr uint32_t kTileSize = 1024;
    static constexpr uint32_t kTileCount = 128;
    // ReGIR's cell size in world units: RTXDI's default of 1 m (ReGIRDynamicParameters), 52 Source units.
    static constexpr float kRegirCellSize = 52.0f;
    // Light slots per ReGIR cell. RTXDI's default of 512 made filling the 2229 cells cost more than Legacy sampling's
    // whole direct pass on de_investment; 128 cut it below that with equal noise.
    static constexpr uint32_t kRegirLightsPerCell = 128;

    bool Initialize(ID3D12Device* device, DescriptorHeap& descriptors);
    // Whether Prepare must replace resources (the GPU must be done with them first).
    bool NeedsResources(UINT width, UINT height, const RestirLights& lights) const;
    // Readies the resources for a frame of `width` x `height` pixels lighting `lights`; false (logged once) when they
    // cannot be made.
    bool Prepare(UINT width, UINT height, const RestirLights& lights);
    // Advances the contexts to a frame and fills its constants. `eye` centres ReGIR; `previousInvViewProj` is last frame's
    // view-projection inverted; `rayReconstruction` follows RTXPT and the DLSS-RR guide (no visibility reuse, which
    // correlates frames).
    void FillConstants(RestirConstants& constants, uint32_t frameIndex, RestirMode mode, bool rayReconstruction, const float eye[3],
        const float previousInvViewProj[4][4], const RestirLights& lights, float emitterPower, UINT previousLightStylesView, bool historyValid);
    // Last frame's light style values, for the next frame's temporal reuse; returns their view for `slot`.
    UINT UploadPreviousStyles(UINT slot, const float* values, size_t count);

    ID3D12Resource* RisBuffer() const { return m_ris.Get(); }
    ID3D12Resource* Reservoirs() const { return m_reservoirs.Get(); }
    ID3D12Resource* NeighborOffsets() const { return m_neighborOffsets.Get(); }
    ID3D12Resource* Pdf() const { return m_pdf.Get(); }
    D3D12_RESOURCE_STATES& PdfState() { return m_pdfState; }
    UINT PdfMips() const { return m_pdfMips; }
    UINT PdfWidth() const { return m_pdfSize[0]; }
    UINT PdfHeight() const { return m_pdfSize[1]; }
    uint32_t RegirLightSlots() const { return m_regir ? m_regir->GetReGIRLightSlotCount() : 0; }

private:
    bool CreatePdf(uint32_t lights);
    bool CreateReservoirs(UINT width, UINT height);
    void FillRegir(ReGIR_Parameters& parameters) const;

    ID3D12Device* m_device = nullptr;
    DescriptorHeap* m_descriptors = nullptr;
    bool m_failed = false;
    rtxdi::RISBufferSegmentAllocator m_risAllocator;
    uint32_t m_localLightRisOffset = 0;
    std::unique_ptr<rtxdi::ReGIRContext> m_regir;
    std::unique_ptr<rtxdi::ReSTIRDIContext> m_context;
    UINT m_width = 0;
    UINT m_height = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ris;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_reservoirs;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_neighborOffsets;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_pdf;
    D3D12_RESOURCE_STATES m_pdfState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    uint32_t m_pdfCapacity = 0;
    uint32_t m_pdfSize[2] = {};
    UINT m_pdfMips = 0;
    UINT m_pdfView = DescriptorHeap::kInvalid;
    UINT m_pdfMipUavs = DescriptorHeap::kInvalid;
    static constexpr UINT kMaxPdfMips = 16;
    static constexpr UINT kSlots = 3;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_previousStyles;
    std::byte* m_previousStylesMapped = nullptr;
    std::array<UINT, kSlots> m_previousStylesViews = {};
};

}
