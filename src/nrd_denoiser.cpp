#include "nrd_denoiser.h"

#include "log.h"
#include "parallel_jobs.h"

#include <algorithm>
#include <cstring>
#include <functional>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

// Descriptors a dispatch's table may hold (its SRVs, then its UAVs), reserved once for every denoiser.
constexpr UINT kMaxTable = 48;

DXGI_FORMAT DxgiFormat(nrd::Format format)
{
    switch (format) {
    case nrd::Format::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case nrd::Format::R8_SNORM: return DXGI_FORMAT_R8_SNORM;
    case nrd::Format::R8_UINT: return DXGI_FORMAT_R8_UINT;
    case nrd::Format::R8_SINT: return DXGI_FORMAT_R8_SINT;
    case nrd::Format::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case nrd::Format::RG8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
    case nrd::Format::RG8_UINT: return DXGI_FORMAT_R8G8_UINT;
    case nrd::Format::RG8_SINT: return DXGI_FORMAT_R8G8_SINT;
    case nrd::Format::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case nrd::Format::RGBA8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case nrd::Format::RGBA8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
    case nrd::Format::RGBA8_SINT: return DXGI_FORMAT_R8G8B8A8_SINT;
    case nrd::Format::RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case nrd::Format::R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case nrd::Format::R16_SNORM: return DXGI_FORMAT_R16_SNORM;
    case nrd::Format::R16_UINT: return DXGI_FORMAT_R16_UINT;
    case nrd::Format::R16_SINT: return DXGI_FORMAT_R16_SINT;
    case nrd::Format::R16_SFLOAT: return DXGI_FORMAT_R16_FLOAT;
    case nrd::Format::RG16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
    case nrd::Format::RG16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
    case nrd::Format::RG16_UINT: return DXGI_FORMAT_R16G16_UINT;
    case nrd::Format::RG16_SINT: return DXGI_FORMAT_R16G16_SINT;
    case nrd::Format::RG16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case nrd::Format::RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case nrd::Format::RGBA16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case nrd::Format::RGBA16_UINT: return DXGI_FORMAT_R16G16B16A16_UINT;
    case nrd::Format::RGBA16_SINT: return DXGI_FORMAT_R16G16B16A16_SINT;
    case nrd::Format::RGBA16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case nrd::Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
    case nrd::Format::R32_SINT: return DXGI_FORMAT_R32_SINT;
    case nrd::Format::R32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
    case nrd::Format::RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
    case nrd::Format::RG32_SINT: return DXGI_FORMAT_R32G32_SINT;
    case nrd::Format::RG32_SFLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case nrd::Format::RGB32_UINT: return DXGI_FORMAT_R32G32B32_UINT;
    case nrd::Format::RGB32_SINT: return DXGI_FORMAT_R32G32B32_SINT;
    case nrd::Format::RGB32_SFLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
    case nrd::Format::RGBA32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
    case nrd::Format::RGBA32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
    case nrd::Format::RGBA32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case nrd::Format::R10_G10_B10_A2_UINT: return DXGI_FORMAT_R10G10B10A2_UINT;
    case nrd::Format::R11_G11_B10_UFLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case nrd::Format::R9_G9_B9_E5_UFLOAT: return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

const char* MethodName(NrdDenoiser::Method method)
{
    switch (method) {
    case NrdDenoiser::Method::Relax: return "RELAX";
    case NrdDenoiser::Method::Reblur: return "REBLUR";
    case NrdDenoiser::Method::Sigma: return "SIGMA";
    }
    return "?";
}

}

bool NrdDenoiser::Initialize(ID3D12Device* device, DescriptorHeap& descriptors, Method method, UINT width, UINT height)
{
    Release();
    m_device = device;
    m_descriptors = &descriptors;
    m_method = method;
    m_width = width;
    m_height = height;
    const nrd::LibraryDesc& library = *nrd::GetLibraryDesc();
    // shaders/nvidia/nrd_common.hlsli packs normals and roughness in NRD's default encodings.
    if (library.normalEncoding != nrd::NormalEncoding::R10_G10_B10_A2_UNORM || library.roughnessEncoding != nrd::RoughnessEncoding::LINEAR) {
        log::Info("NRD: the library was built with other normal or roughness encodings");
        return false;
    }
    const nrd::Denoiser denoisers[] = {nrd::Denoiser::RELAX_DIFFUSE_SPECULAR, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR, nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY};
    const nrd::DenoiserDesc denoiser = {0, denoisers[static_cast<size_t>(method)]};
    nrd::InstanceCreationDesc creation = {};
    creation.denoisers = &denoiser;
    creation.denoisersNum = 1;
    if (nrd::CreateInstance(creation, m_instance) != nrd::Result::SUCCESS) {
        m_instance = nullptr;
        log::Info("NRD: creating the denoiser failed");
        return false;
    }
    const nrd::InstanceDesc& instance = *nrd::GetInstanceDesc(*m_instance);
    m_tableTextures = instance.descriptorPoolDesc.perSetTexturesMaxNum;
    m_tableStorages = instance.descriptorPoolDesc.perSetStorageTexturesMaxNum;
    m_tableSize = m_tableTextures + m_tableStorages;
    if (m_tableSize > kMaxTable) {
        log::Info("NRD: its pipelines need {} descriptors, more than {}", m_tableSize, kMaxTable);
        Release();
        return false;
    }

    // The root signature of every NRD pipeline.
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {};
    UINT rangeCount = 0;
    const D3D12_DESCRIPTOR_RANGE_FLAGS volatileRange = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    if (m_tableTextures != 0)
        ranges[rangeCount++] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, m_tableTextures, instance.resourcesBaseRegisterIndex, instance.resourcesSpaceIndex, volatileRange, 0};
    if (m_tableStorages != 0)
        ranges[rangeCount++] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, m_tableStorages, instance.resourcesBaseRegisterIndex, instance.resourcesSpaceIndex, volatileRange,
            m_tableTextures};
    D3D12_ROOT_PARAMETER1 parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {rangeCount, ranges};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor = {instance.constantBufferRegisterIndex, instance.constantBufferAndSamplersSpaceIndex, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(instance.samplersNum);
    for (uint32_t i = 0; i < instance.samplersNum; ++i) {
        D3D12_STATIC_SAMPLER_DESC& sampler = samplers[i];
        sampler.Filter = instance.samplers[i] == nrd::Sampler::NEAREST_CLAMP ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = instance.samplersBaseRegisterIndex + i;
        sampler.RegisterSpace = instance.constantBufferAndSamplersSpaceIndex;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc = {D3D_ROOT_SIGNATURE_VERSION_1_1};
    rootDesc.Desc_1_1 = {2, parameters, static_cast<UINT>(samplers.size()), samplers.data(), D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootDesc, &blob, &errors);
    if (SUCCEEDED(hr))
        hr = m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root));
    // Created side by side: the first frame lit with NRD, and every resize, waited for dozens of them one by one.
    if (SUCCEEDED(hr)) {
        m_pipelines.resize(instance.pipelinesNum);
        std::vector<std::function<HRESULT()>> jobs;
        for (uint32_t i = 0; i < instance.pipelinesNum; ++i) {
            jobs.push_back([this, &instance, i] {
                const nrd::ComputeShaderDesc& shader = instance.pipelines[i].computeShaderDXIL;
                D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
                desc.pRootSignature = m_root.Get();
                desc.CS = {shader.bytecode, static_cast<SIZE_T>(shader.size)};
                return m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_pipelines[i]));
            });
        }
        hr = RunInParallel(jobs);
    }

    // Texture pools.
    m_permanentPoolSize = instance.permanentPoolSize;
    const auto addPool = [&](const nrd::TextureDesc& pool) {
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = std::max<UINT>((width + pool.downsampleFactor - 1) / pool.downsampleFactor, 1);
        desc.Height = std::max<UINT>((height + pool.downsampleFactor - 1) / pool.downsampleFactor, 1);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DxgiFormat(pool.format);
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        PoolTexture& texture = m_pool.emplace_back();
        return SUCCEEDED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&texture.texture)));
    };
    for (uint32_t i = 0; i < instance.permanentPoolSize && SUCCEEDED(hr); ++i)
        hr = addPool(instance.permanentPool[i]) ? S_OK : E_OUTOFMEMORY;
    for (uint32_t i = 0; i < instance.transientPoolSize && SUCCEEDED(hr); ++i)
        hr = addPool(instance.transientPool[i]) ? S_OK : E_OUTOFMEMORY;

    // Constants and descriptor tables.
    m_constantStride = (instance.constantBufferMaxDataSize + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1);
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = static_cast<UINT64>(std::max<UINT>(m_constantStride, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)) * kMaxDispatches * kSlots;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constants));
        if (SUCCEEDED(hr))
            hr = m_constants->Map(0, nullptr, reinterpret_cast<void**>(&m_constantsMapped));
    }
    // Reserved descriptors stay with the heap, so the tables are reserved once for every denoiser made after.
    if (SUCCEEDED(hr) && m_tables == DescriptorHeap::kInvalid) {
        m_tables = m_descriptors->Reserve(kSlots * kMaxDispatches * kMaxTable);
        hr = m_tables != DescriptorHeap::kInvalid ? S_OK : E_OUTOFMEMORY;
    }
    if (FAILED(hr)) {
        if (errors)
            log::Info("NRD: {}", static_cast<const char*>(errors->GetBufferPointer()));
        log::Info("NRD: creating its pipelines or textures failed ({:#010x})", static_cast<unsigned>(hr));
        Release();
        return false;
    }
    log::Info("NRD {}.{}.{}: {} for {}x{}, {} pipelines, {} pool textures", library.versionMajor, library.versionMinor, library.versionBuild,
        MethodName(method), width, height, m_pipelines.size(), m_pool.size());
    return true;
}

void NrdDenoiser::Release()
{
    if (m_instance != nullptr)
        nrd::DestroyInstance(*m_instance);
    m_instance = nullptr;
    m_pipelines.clear();
    m_root.Reset();
    m_pool.clear();
    m_constants.Reset();
    m_constantsMapped = nullptr;
    m_width = 0;
    m_height = 0;
}

namespace {

struct HistoryLengths {
    uint32_t frames;
    uint32_t fast;
    uint32_t fix;
};

// NRD's history lengths are frames at 60 fps (30, fast 6), and NRDSettings.h asks for them in seconds instead, so a
// switched light settles in the same time at any frame rate. The fast history stays 5 times shorter, and history
// fix below it.
HistoryLengths Lengths(float seconds, float fps, uint32_t maxFrames, uint32_t fix)
{
    const uint32_t frames = std::clamp(nrd::GetMaxAccumulatedFrameNum(seconds, fps), 1u, maxFrames);
    const uint32_t fast = frames / 5;
    return {frames, fast, std::min(fix, fast > 0 ? fast - 1 : 0u)};
}

}

float NrdDenoiser::SmoothedFps()
{
    LARGE_INTEGER now;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float frameMs = static_cast<float>(static_cast<double>(now.QuadPart - m_lastDenoiseTicks) * 1000.0 / static_cast<double>(frequency.QuadPart));
    // NRDSettings.h's low-pass filter with a time constant of about 0.2 s. Gaps while NRD was not used (another
    // denoiser, a map load) are no frame time.
    if (m_lastDenoiseTicks != 0 && frameMs < 250.0f) {
        const float n = 1000.0f / m_smoothedFrameMs * 0.2f;
        m_smoothedFrameMs += (frameMs - m_smoothedFrameMs) / (1.0f + n);
    }
    m_lastDenoiseTicks = now.QuadPart;
    return 1000.0f / m_smoothedFrameMs;
}

bool NrdDenoiser::Denoise(ID3D12GraphicsCommandList* list, UINT slot, const nrd::CommonSettings& common, const Resources& resources,
    const float (&lightDirection)[3])
{
    if (m_instance == nullptr)
        return false;
    nrd::SetCommonSettings(*m_instance, common);
    const float fps = SmoothedFps();
    if (m_method == Method::Relax) {
        nrd::RelaxSettings settings = {};
        const HistoryLengths lengths = Lengths(nrd::RELAX_DEFAULT_ACCUMULATION_TIME, fps, nrd::RELAX_MAX_HISTORY_FRAME_NUM, settings.historyFixFrameNum);
        settings.diffuseMaxAccumulatedFrameNum = settings.specularMaxAccumulatedFrameNum = lengths.frames;
        settings.diffuseMaxFastAccumulatedFrameNum = settings.specularMaxFastAccumulatedFrameNum = lengths.fast;
        settings.historyFixFrameNum = lengths.fix;
        nrd::SetDenoiserSettings(*m_instance, 0, &settings);
    } else if (m_method == Method::Sigma) {
        nrd::SigmaSettings settings = {};
        std::copy_n(lightDirection, 3, settings.lightDirection);
        settings.maxStabilizedFrameNum = std::min(nrd::GetMaxAccumulatedFrameNum(nrd::SIGMA_DEFAULT_ACCUMULATION_TIME, fps), nrd::SIGMA_MAX_HISTORY_FRAME_NUM);
        nrd::SetDenoiserSettings(*m_instance, 0, &settings);
    } else {
        nrd::ReblurSettings settings = {};
        // NRD's defaults are in meters; Source's units are inches.
        settings.hitDistanceParameters.A *= 39.37f;
        const HistoryLengths lengths = Lengths(nrd::REBLUR_DEFAULT_ACCUMULATION_TIME, fps, nrd::REBLUR_MAX_HISTORY_FRAME_NUM, settings.historyFixFrameNum);
        settings.maxAccumulatedFrameNum = lengths.frames;
        settings.maxFastAccumulatedFrameNum = lengths.fast;
        settings.historyFixFrameNum = lengths.fix;
        nrd::SetDenoiserSettings(*m_instance, 0, &settings);
    }
    const nrd::Identifier identifier = 0;
    const nrd::DispatchDesc* dispatches = nullptr;
    uint32_t dispatchCount = 0;
    if (nrd::GetComputeDispatches(*m_instance, &identifier, 1, dispatches, dispatchCount) != nrd::Result::SUCCESS || dispatchCount > kMaxDispatches)
        return false;
    const nrd::InstanceDesc& instance = *nrd::GetInstanceDesc(*m_instance);
    ID3D12DescriptorHeap* heaps[] = {m_descriptors->Heap()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_root.Get());
    const UINT slotTables = m_tables + (slot % kSlots) * kMaxDispatches * kMaxTable;
    const D3D12_GPU_VIRTUAL_ADDRESS slotConstants = m_constants->GetGPUVirtualAddress() + static_cast<UINT64>(slot % kSlots) * kMaxDispatches * m_constantStride;
    std::byte* slotConstantsMapped = m_constantsMapped + static_cast<size_t>(slot % kSlots) * kMaxDispatches * m_constantStride;
    D3D12_GPU_VIRTUAL_ADDRESS constants = slotConstants;
    for (uint32_t d = 0; d < dispatchCount; ++d) {
        const nrd::DispatchDesc& dispatch = dispatches[d];
        const nrd::PipelineDesc& pipeline = instance.pipelines[dispatch.pipelineIndex];
        const UINT table = slotTables + d * kMaxTable;
        // Every resource the dispatch names must exist before any state changes.
        for (uint32_t i = 0; i < dispatch.resourcesNum; ++i) {
            const nrd::ResourceDesc& resource = dispatch.resources[i];
            const bool pooled = resource.type == nrd::ResourceType::PERMANENT_POOL || resource.type == nrd::ResourceType::TRANSIENT_POOL;
            const Resource& provided = resources[static_cast<size_t>(resource.type)];
            if (!pooled && (provided.texture == nullptr || provided.state == nullptr)) {
                log::Info("NRD: dispatch '{}' needs {}, which is missing", dispatch.name != nullptr ? dispatch.name : "", nrd::GetResourceTypeString(resource.type));
                return false;
            }
        }
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        uint32_t n = 0;
        for (uint32_t r = 0; r < pipeline.resourceRangesNum; ++r) {
            const nrd::ResourceRangeDesc& range = pipeline.resourceRanges[r];
            const bool storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            for (uint32_t j = 0; j < range.descriptorsNum; ++j, ++n) {
                const nrd::ResourceDesc& resource = dispatch.resources[n];
                ID3D12Resource* texture = nullptr;
                D3D12_RESOURCE_STATES* state = nullptr;
                if (resource.type == nrd::ResourceType::PERMANENT_POOL || resource.type == nrd::ResourceType::TRANSIENT_POOL) {
                    PoolTexture& pool = m_pool[resource.indexInPool + (resource.type == nrd::ResourceType::TRANSIENT_POOL ? m_permanentPoolSize : 0)];
                    texture = pool.texture.Get();
                    state = &pool.state;
                } else {
                    const Resource& provided = resources[static_cast<size_t>(resource.type)];
                    texture = provided.texture;
                    state = provided.state;
                }
                const D3D12_RESOURCE_STATES wanted = storage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                if (*state != wanted) {
                    barriers.push_back(Transition(texture, *state, wanted));
                    *state = wanted;
                } else if (storage) {
                    D3D12_RESOURCE_BARRIER barrier = {};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    barrier.UAV.pResource = texture;
                    barriers.push_back(barrier);
                }
                const D3D12_RESOURCE_DESC desc = texture->GetDesc();
                if (storage) {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
                    uav.Format = desc.Format;
                    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    m_device->CreateUnorderedAccessView(texture, nullptr, &uav, m_descriptors->Cpu(table + m_tableTextures + j));
                } else {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                    srv.Format = desc.Format;
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.Texture2D.MipLevels = 1;
                    m_device->CreateShaderResourceView(texture, &srv, m_descriptors->Cpu(table + j));
                }
            }
        }
        if (!barriers.empty())
            list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        if (dispatch.constantBufferDataSize != 0 && !dispatch.constantBufferDataMatchesPreviousDispatch) {
            std::memcpy(slotConstantsMapped + static_cast<size_t>(d) * m_constantStride, dispatch.constantBufferData, dispatch.constantBufferDataSize);
            constants = slotConstants + static_cast<UINT64>(d) * m_constantStride;
        }
        list->SetPipelineState(m_pipelines[dispatch.pipelineIndex].Get());
        list->SetComputeRootDescriptorTable(0, m_descriptors->Gpu(table));
        list->SetComputeRootConstantBufferView(1, constants);
        list->Dispatch(dispatch.gridWidth, dispatch.gridHeight, 1);
    }
    return true;
}

}
