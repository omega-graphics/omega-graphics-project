#include "GED3D12.h"
#include "GED3D12CommandQueue.h"
#include "GED3D12Texture.h"
#include "GED3D12RenderTarget.h"
#include "GED3D12Pipeline.h"

#include "../BufferIO.h"

#include <atlstr.h>
#include <cassert>
#include <cstdio>
#include <d3d12.h>
#include <limits>
#include <memory>

#include <Windows.h>

#include "omegaGTE/GTEDevice.h"
#include "omegaGTE/GECommandQueue.h"
#include "omegaGTE/GEPipeline.h"
#include "omegaGTE/GERenderTarget.h"
#include "omegaGTE/GTEMath.h"


_NAMESPACE_BEGIN_

    inline DXGI_FORMAT pixelFormatToDxgiFormat(PixelFormat fmt){
        switch(fmt){
            case PixelFormat::RGBA8Unorm:      return DXGI_FORMAT_R8G8B8A8_UNORM;
            case PixelFormat::RGBA16Unorm:     return DXGI_FORMAT_R16G16B16A16_UNORM;
            case PixelFormat::RGBA8Unorm_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case PixelFormat::BGRA8Unorm:      return DXGI_FORMAT_B8G8R8A8_UNORM;
            case PixelFormat::BGRA8Unorm_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            default:                           return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }

struct GTED3D12Device : public GTEDevice {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    bool isUMA = false;
    const void * native() override {
        return (const void *)adapter.Get();
    }
    GTED3D12Device(GTEDevice::Type type,const char *name,GTEDeviceFeatures & features,IDXGIAdapter1 *adapter,bool isUMA_)
        : GTEDevice(type,name,features),adapter(adapter),isUMA(isUMA_) {

    };
    GTEDeviceMemoryBudget queryMemoryBudget() override {
        GTEDeviceMemoryBudget out;
        out.unifiedMemory = isUMA;

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        out.dedicatedVideoMemory = isUMA ? 0ULL : (uint64_t)desc.DedicatedVideoMemory;

        ComPtr<IDXGIAdapter3> adapter3;
        if(SUCCEEDED(adapter.As(&adapter3))){
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            DXGI_MEMORY_SEGMENT_GROUP segment = isUMA
                ? DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL
                : DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
            if(SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, segment, &info))){
                uint64_t used = info.CurrentUsage;
                uint64_t budget = info.Budget;
                out.availableVideoMemory = (budget > used) ? (budget - used) : 0;
            }
        }
        return out;
    }
    ~GTED3D12Device() = default;
};

static GTEDeviceFeatures queryDeviceFeatures(IDXGIAdapter1 *adapter, bool *outIsUMA){
    GTEDeviceFeatures features {};
    if(outIsUMA) *outIsUMA = false;

    ComPtr<ID3D12Device5> tmpDevice;
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&tmpDevice));
    if(FAILED(hr) || !tmpDevice){
        return features;
    }

    // ── OPTIONS ─────────────────────────────────────────────
    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    if(SUCCEEDED(tmpDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts)))){
        if(opts.DoublePrecisionFloatShaderOps)
            features.flags |= GTEDEVICE_FEATURE_SHADER_FLOAT64;
        if(opts.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3)
            features.flags |= GTEDEVICE_FEATURE_DESCRIPTOR_INDEXING;
        if(opts.ConservativeRasterizationTier >= D3D12_CONSERVATIVE_RASTERIZATION_TIER_1)
            features.flags |= GTEDEVICE_FEATURE_CONSERVATIVE_RASTERIZATION;
    }

    // ── OPTIONS1 (Int64) ────────────────────────────────────
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 opts1{};
    if(SUCCEEDED(tmpDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opts1, sizeof(opts1)))){
        if(opts1.Int64ShaderOps)
            features.flags |= GTEDEVICE_FEATURE_SHADER_INT64;
    }

    // ── OPTIONS3 (Barycentrics) ─────────────────────────────
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 opts3{};
    if(SUCCEEDED(tmpDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &opts3, sizeof(opts3)))){
        if(opts3.BarycentricsSupported)
            features.flags |= GTEDEVICE_FEATURE_SHADER_BARYCENTRIC;
    }

    // ── OPTIONS4 (Native 16-bit shader ops) ─────────────────
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 opts4{};
    if(SUCCEEDED(tmpDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &opts4, sizeof(opts4)))){
        if(opts4.Native16BitShaderOpsSupported){
            features.flags |= GTEDEVICE_FEATURE_SHADER_FLOAT16;
            features.flags |= GTEDEVICE_FEATURE_SHADER_INT16;
        }
    }

    // ── OPTIONS5 (Raytracing) ───────────────────────────────
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
    if(SUCCEEDED(tmpDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5)))
       && opts5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0){
        features.flags |= GTEDEVICE_FEATURE_RAYTRACING;
    }

    // ── OPTIONS6 (Variable Rate Shading) ────────────────────
    // The previous `#if defined(D3D12_FEATURE_D3D12_OPTIONS6)` guard was
    // a no-op: `D3D12_FEATURE_D3D12_OPTIONS6` is an enum value (= 30) in
    // d3d12.h, not a preprocessor macro, so `defined(...)` is always
    // false and the body never compiled — VRS detection was silently
    // dead on every SDK. The hard floor for these probes is Windows 10
    // SDK 10.0.19041.0 (May 2020), documented in gte/docs/Building.rst;
    // an older SDK now fails the build with a clean
    // "undefined identifier" instead of silently losing detection.
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 opts6{};
    if(SUCCEEDED(tmpDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opts6, sizeof(opts6)))){
        if(opts6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1)
            features.flags |= GTEDEVICE_FEATURE_VARIABLE_RATE_SHADING;
    }

    // ── OPTIONS7 (Mesh shaders) ─────────────────────────────
    // Same broken-guard story as OPTIONS6 above. Same SDK floor.
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
    if(SUCCEEDED(tmpDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7)))){
        if(opts7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1)
            features.flags |= GTEDEVICE_FEATURE_MESH_SHADER;
    }

    // ── ARCHITECTURE (UMA) ──────────────────────────────────
    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    arch.NodeIndex = 0;
    if(SUCCEEDED(tmpDevice->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch)))){
        if(outIsUMA) *outIsUMA = arch.UMA;
    }

    // ── SHADER_MODEL ────────────────────────────────────────
    D3D12_FEATURE_DATA_SHADER_MODEL smQuery{};
    smQuery.HighestShaderModel = D3D_SHADER_MODEL_6_7;
    if(SUCCEEDED(tmpDevice->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &smQuery, sizeof(smQuery)))){
        switch(smQuery.HighestShaderModel){
            case D3D_SHADER_MODEL_6_7: features.shaderModel = GTEDeviceFeatures::ShaderModel::SM_6_7; break;
            case D3D_SHADER_MODEL_6_6: features.shaderModel = GTEDeviceFeatures::ShaderModel::SM_6_6; break;
            case D3D_SHADER_MODEL_6_5: features.shaderModel = GTEDeviceFeatures::ShaderModel::SM_6_5; break;
            case D3D_SHADER_MODEL_6_4: features.shaderModel = GTEDeviceFeatures::ShaderModel::SM_6_4; break;
            case D3D_SHADER_MODEL_6_0: features.shaderModel = GTEDeviceFeatures::ShaderModel::SM_6_0; break;
            default:                    features.shaderModel = GTEDeviceFeatures::ShaderModel::SM_5_1; break;
        }
    }

    // ── Always-true flags per D3D12 feature level 11_0+ ─────
    features.flags |= GTEDEVICE_FEATURE_INDEPENDENT_BLEND;
    features.flags |= GTEDEVICE_FEATURE_DUAL_SOURCE_BLENDING;
    features.flags |= GTEDEVICE_FEATURE_DEPTH_CLAMP;
    features.flags |= GTEDEVICE_FEATURE_DEPTH_BIAS_CLAMP;
    features.flags |= GTEDEVICE_FEATURE_FILL_MODE_NON_SOLID;
    features.flags |= GTEDEVICE_FEATURE_SAMPLER_ANISOTROPY;
    features.flags |= GTEDEVICE_FEATURE_MULTI_DRAW_INDIRECT;
    features.flags |= GTEDEVICE_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE;
    features.flags |= GTEDEVICE_FEATURE_GEOMETRY_SHADER;
    features.flags |= GTEDEVICE_FEATURE_TESSELLATION_SHADER;
    features.flags |= GTEDEVICE_FEATURE_TIMESTAMP_QUERIES;
    features.flags |= GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_BC;
    // WIDE_LINES, ETC2, ASTC: unsupported on D3D12.

    // ── MSAA ────────────────────────────────────────────────
    uint8_t maxSamples = 1;
    for(UINT n : {32u, 16u, 8u, 4u, 2u}){
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ms {};
        ms.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ms.SampleCount = n;
        ms.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        if(SUCCEEDED(tmpDevice->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &ms, sizeof(ms)))
           && ms.NumQualityLevels > 0){
            maxSamples = (uint8_t)n;
            break;
        }
    }
    features.maxMSAASamples = maxSamples;

    // ── Limits (D3D12 spec constants) ───────────────────────
    features.maxTextureDimension2D   = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    features.maxTextureDimension3D   = D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    features.maxTextureDimensionCube = D3D12_REQ_TEXTURECUBE_DIMENSION;

    features.maxComputeWorkGroupSizeX       = D3D12_CS_THREAD_GROUP_MAX_X;
    features.maxComputeWorkGroupSizeY       = D3D12_CS_THREAD_GROUP_MAX_Y;
    features.maxComputeWorkGroupSizeZ       = D3D12_CS_THREAD_GROUP_MAX_Z;
    features.maxComputeWorkGroupInvocations = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
    // TGSM: 8192 32-bit scalar registers == 32 KB per threadgroup.
    features.maxComputeSharedMemorySize     = D3D12_CS_TGSM_REGISTER_COUNT * 4u;

    features.maxSamplerAnisotropy = 16;  // D3D12 requires exactly 16 for anisotropic sampling.
    // D3D12 has no hard per-buffer cap — bounded by VRAM / process address space.
    features.maxBufferSize = (std::numeric_limits<uint64_t>::max)();

    // ── Timestamp period ────────────────────────────────────
    D3D12_COMMAND_QUEUE_DESC cqDesc{};
    cqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> cq;
    if(SUCCEEDED(tmpDevice->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&cq)))){
        UINT64 freq = 0;
        if(SUCCEEDED(cq->GetTimestampFrequency(&freq)) && freq > 0){
            features.timestampPeriod = 1.0e9f / (float)freq;
        }
    }

    return features;
}

OmegaCommon::Vector<SharedHandle<GTEDevice>> enumerateDevices(){
    OmegaCommon::Vector<SharedHandle<GTEDevice>> devs;
    ComPtr<IDXGIFactory6> dxgi_factory;
    CreateDXGIFactory(IID_PPV_ARGS(&dxgi_factory));

    IDXGIAdapter1 *adapter;
    DXGI_ADAPTER_DESC1 desc;
    for(unsigned i = 0;SUCCEEDED(dxgi_factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                          IID_PPV_ARGS(&adapter)));i++){
        adapter->GetDesc1(&desc);
        CAtlString atlString(desc.Description);
        bool isUMA = false;
        GTEDeviceFeatures features = queryDeviceFeatures(adapter, &isUMA);
        devs.emplace_back(SharedHandle<GTEDevice>(new GTED3D12Device {GTEDevice::Discrete,atlString.GetBuffer(),features,adapter,isUMA}));
    }

    for(unsigned i = 0;SUCCEEDED(dxgi_factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_MINIMUM_POWER,
                                                                          IID_PPV_ARGS(&adapter)));i++){
        adapter->GetDesc1(&desc);
        CAtlString atlString(desc.Description);
        bool isUMA = false;
        GTEDeviceFeatures features = queryDeviceFeatures(adapter, &isUMA);
        devs.emplace_back(SharedHandle<GTEDevice>(new GTED3D12Device {GTEDevice::Integrated,atlString.GetBuffer(),features,adapter,isUMA}));
    }

    return devs;
}


SharedHandle<GEBuffer> GED3D12Heap::makeBuffer(const BufferDescriptor &desc){
    HRESULT hr;

    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_GENERIC_READ;

    switch(desc.usage){
        case BufferDescriptor::Upload:
            state = D3D12_RESOURCE_STATE_GENERIC_READ;
            break;
        case BufferDescriptor::Readback:
            flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            state = D3D12_RESOURCE_STATE_COPY_DEST;
            break;
        case BufferDescriptor::GPUOnly:
            flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            state = D3D12_RESOURCE_STATE_COPY_DEST;
            break;
    }

    // §2.4 — round constant buffers up to the 256-byte CBV placement
    // requirement (see GED3D12Engine::makeBuffer).
    UINT64 bufferLen = desc.len;
    if(desc.role == BufferDescriptor::Uniform){
        bufferLen = (bufferLen + 255ull) & ~255ull;
    }
    D3D12_RESOURCE_DESC d3d12_desc = CD3DX12_RESOURCE_DESC::Buffer(bufferLen, flags);

    // Suballocate within this heap's pool. D3D12MA handles alignment,
    // offsets, and free-list reclamation; HeapType is implicit in the pool.
    D3D12MA::ALLOCATION_DESC allocDesc {};
    allocDesc.CustomPool = pool;

    ID3D12Resource *buffer = nullptr;
    D3D12MA::Allocation *allocation = nullptr;
    hr = engine->memAllocator->CreateResource(
        &allocDesc, &d3d12_desc, state, nullptr,
        &allocation, IID_PPV_ARGS(&buffer));
    if(FAILED(hr)){
        DEBUG_STREAM("GED3D12Heap::makeBuffer: D3D12MA CreateResource failed");
        if (allocation) allocation->Release();
        return nullptr;
    }

    D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc {};
    descHeapDesc.NumDescriptors = 1;
    descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    descHeapDesc.NodeMask = engine->d3d12_device->GetNodeCount();
    descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ID3D12DescriptorHeap *descHeap;
    hr = engine->d3d12_device->CreateDescriptorHeap(&descHeapDesc,IID_PPV_ARGS(&descHeap));
    if(FAILED(hr)){
        return nullptr;
    }

    if(desc.role == BufferDescriptor::Uniform){
        // §2.4 — uniform buffers bind via a root CBV; no SRV/UAV needed.
    } else if(desc.usage == BufferDescriptor::Upload && desc.objectStride > 0){
        D3D12_SHADER_RESOURCE_VIEW_DESC res_view_desc {};
        res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        res_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        res_view_desc.Format = DXGI_FORMAT_UNKNOWN;
        res_view_desc.Buffer.StructureByteStride = desc.objectStride;
        res_view_desc.Buffer.FirstElement = 0;
        res_view_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        res_view_desc.Buffer.NumElements = desc.len / desc.objectStride;
        engine->d3d12_device->CreateShaderResourceView(buffer,&res_view_desc,descHeap->GetCPUDescriptorHandleForHeapStart());
    } else if(desc.objectStride > 0) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc {};
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_desc.Format = DXGI_FORMAT_UNKNOWN;
        uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        uav_desc.Buffer.NumElements = desc.len / desc.objectStride;
        uav_desc.Buffer.StructureByteStride = desc.objectStride;
        uav_desc.Buffer.CounterOffsetInBytes = 0;
        uav_desc.Buffer.FirstElement = 0;
        engine->d3d12_device->CreateUnorderedAccessView(buffer,nullptr,&uav_desc,descHeap->GetCPUDescriptorHandleForHeapStart());
    }

    auto *d3d12_buffer = new GED3D12Buffer(desc.usage,buffer,descHeap,state,allocation);
    d3d12_buffer->role = desc.role;
    return SharedHandle<GEBuffer>(d3d12_buffer);
}

SharedHandle<GETexture> GED3D12Heap::makeTexture(const TextureDescriptor &desc){
    HRESULT hr;
    D3D12_RESOURCE_DESC d3d12_desc {};
    D3D12_RESOURCE_STATES res_states = D3D12_RESOURCE_STATE_COMMON;

    DXGI_FORMAT dxgiFormat = pixelFormatToDxgiFormat(desc.pixelFormat);

    D3D12_RESOURCE_FLAGS resFlags = D3D12_RESOURCE_FLAG_NONE;
    if(desc.usage == GETexture::RenderTarget || desc.usage == GETexture::RenderTargetAndDepthStencil){
        res_states = D3D12_RESOURCE_STATE_RENDER_TARGET;
        resFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    } else if(desc.usage == GETexture::ToGPU){
        res_states = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else if(desc.usage == GETexture::FromGPU){
        res_states = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        resFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    // §6.2 — the heap path mirrors the engine path's kind dispatch. Cube /
    // array / MS textures place the same way as their underlying 2D-array
    // resource on D3D12; only the SRV view dimension differs.
    const TextureKind kind = desc.kind == TextureKind::Auto ? TextureKind::Tex2D : desc.kind;
    const unsigned arrayLayers = desc.arrayLayers > 0 ? desc.arrayLayers : 1;
    const bool isMS = (kind == TextureKind::Tex2DMS || kind == TextureKind::Tex2DMSArray);
    const unsigned effectiveSampleCount = isMS ? (desc.sampleCount > 1 ? desc.sampleCount : 1u) : 1u;
    const unsigned effectiveMips = isMS ? 1u : desc.mipLevels;

    switch(kind){
        case TextureKind::Tex1D:
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex1D(dxgiFormat,desc.width,1,effectiveMips);
            break;
        case TextureKind::Tex1DArray:
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex1D(dxgiFormat,desc.width,arrayLayers,effectiveMips);
            break;
        case TextureKind::Tex2D:
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,1,effectiveMips,1);
            break;
        case TextureKind::Tex2DArray:
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,arrayLayers,effectiveMips,1);
            break;
        case TextureKind::TexCube:
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,6,effectiveMips,1);
            break;
        case TextureKind::TexCubeArray: {
            const unsigned layers = arrayLayers >= 6 ? arrayLayers : 6;
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,layers,effectiveMips,1);
            break;
        }
        case TextureKind::Tex2DMS:
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,1,1,effectiveSampleCount);
            break;
        case TextureKind::Tex2DMSArray:
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,arrayLayers,1,effectiveSampleCount);
            break;
        case TextureKind::Tex3D:
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex3D(dxgiFormat,desc.width,desc.height,desc.depth,effectiveMips);
            break;
        case TextureKind::Auto:
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,1,effectiveMips,1);
            break;
    }
    d3d12_desc.Flags = resFlags;

    D3D12MA::ALLOCATION_DESC allocDesc {};
    allocDesc.CustomPool = pool;

    ID3D12Resource *texture = nullptr;
    D3D12MA::Allocation *allocation = nullptr;
    hr = engine->memAllocator->CreateResource(
        &allocDesc, &d3d12_desc, res_states, nullptr,
        &allocation, IID_PPV_ARGS(&texture));
    if(FAILED(hr)){
        DEBUG_STREAM("GED3D12Heap::makeTexture: D3D12MA CreateResource failed");
        if (allocation) allocation->Release();
        return nullptr;
    }

    D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc {};
    descHeapDesc.NumDescriptors = 1;
    descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    descHeapDesc.NodeMask = engine->d3d12_device->GetNodeCount();
    descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ID3D12DescriptorHeap *descHeap;
    hr = engine->d3d12_device->CreateDescriptorHeap(&descHeapDesc,IID_PPV_ARGS(&descHeap));
    if(FAILED(hr)){
        return nullptr;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC res_view_desc {};
    res_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    res_view_desc.Format = dxgiFormat;
    switch(kind){
        case TextureKind::Tex1D:
            res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            res_view_desc.Texture1D.MipLevels = effectiveMips;
            break;
        case TextureKind::Tex1DArray:
            res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            res_view_desc.Texture1DArray.MipLevels = effectiveMips;
            res_view_desc.Texture1DArray.ArraySize = arrayLayers;
            break;
        case TextureKind::Tex2D:
            res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            res_view_desc.Texture2D.MipLevels = effectiveMips;
            break;
        case TextureKind::Tex2DArray:
            res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            res_view_desc.Texture2DArray.MipLevels = effectiveMips;
            res_view_desc.Texture2DArray.ArraySize = arrayLayers;
            break;
        case TextureKind::TexCube:
            res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            res_view_desc.TextureCube.MipLevels = effectiveMips;
            break;
        case TextureKind::TexCubeArray:
            res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            res_view_desc.TextureCubeArray.MipLevels = effectiveMips;
            res_view_desc.TextureCubeArray.NumCubes =
                (arrayLayers >= 6 ? arrayLayers : 6) / 6;
            break;
        case TextureKind::Tex2DMS:
            res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            break;
        case TextureKind::Tex2DMSArray:
            res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            res_view_desc.Texture2DMSArray.ArraySize = arrayLayers;
            break;
        case TextureKind::Tex3D:
            res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            res_view_desc.Texture3D.MipLevels = effectiveMips;
            break;
        case TextureKind::Auto:
            res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            res_view_desc.Texture2D.MipLevels = effectiveMips;
            break;
    }
    engine->d3d12_device->CreateShaderResourceView(texture,&res_view_desc,descHeap->GetCPUDescriptorHandleForHeapStart());

    ID3D12DescriptorHeap *rtvDescHeap = nullptr;
    if(desc.usage == GETexture::RenderTarget || desc.usage == GETexture::RenderTargetAndDepthStencil){
        descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = engine->d3d12_device->CreateDescriptorHeap(&descHeapDesc,IID_PPV_ARGS(&rtvDescHeap));
        if(FAILED(hr)){
            rtvDescHeap = nullptr;
        } else {
            D3D12_RENDER_TARGET_VIEW_DESC rtv_view_desc {};
            rtv_view_desc.Format = dxgiFormat;
            switch(kind){
                case TextureKind::Tex2D:
                    rtv_view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; break;
                case TextureKind::Tex2DArray:
                case TextureKind::TexCube:
                case TextureKind::TexCubeArray:
                    rtv_view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtv_view_desc.Texture2DArray.ArraySize =
                        kind == TextureKind::TexCube ? 6
                            : (kind == TextureKind::TexCubeArray
                                   ? (arrayLayers >= 6 ? arrayLayers : 6)
                                   : arrayLayers);
                    break;
                case TextureKind::Tex2DMS:
                    rtv_view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS; break;
                case TextureKind::Tex2DMSArray:
                    rtv_view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    rtv_view_desc.Texture2DMSArray.ArraySize = arrayLayers;
                    break;
                case TextureKind::Tex3D:
                    rtv_view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                    rtv_view_desc.Texture3D.WSize = desc.depth;
                    break;
                default:
                    rtv_view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; break;
            }
            engine->d3d12_device->CreateRenderTargetView(texture,&rtv_view_desc,rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
        }
    }

    auto result = SharedHandle<GETexture>(new GED3D12Texture(kind, desc.usage, desc.pixelFormat, texture, nullptr, descHeap, nullptr, rtvDescHeap, nullptr, res_states, allocation));
    result->setShape(kind, arrayLayers, effectiveSampleCount);
    return result;
}

    // void GED3D12Engine::getHardwareAdapter(__in IDXGIFactory4 * dxgi_factory,
    //                                        __out IDXGIAdapter1 **adapter){
    //     ComPtr<IDXGIAdapter1> _out;

    //     ComPtr<IDXGIFactory6> factory6;

    //     BOOL hasDxgiFactory6 = SUCCEEDED(dxgi_factory->QueryInterface(IID_PPV_ARGS(&factory6)));
      
    //     HRESULT hr = S_OK;
    //     UINT adapterIdx = 0;
    //     while(hr != DXGI_ERROR_NOT_FOUND){
            
    //         if(hasDxgiFactory6){
    //             hr = factory6->EnumAdapterByGpuPreference(adapterIdx,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&_out));
    //         }
    //         else {
    //             hr = dxgi_factory->EnumAdapters1(adapterIdx,&_out);
    //         }

    //         if(hr == DXGI_ERROR_NOT_FOUND)
    //             break;
            
    //         DXGI_ADAPTER_DESC1 adapterDesc;
    //         _out->GetDesc1(&adapterDesc);

    //         if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE){
    //             /// Skip DXGI Software Warp Adapter.
    //             continue;
    //         }
    //         else {
    //             break;
    //         }
    //         ++adapterIdx;
    //     };

    //     *adapter = _out.Detach();
    // };


    GED3D12Engine::GED3D12Engine(SharedHandle<GTED3D12Device> device){
        HRESULT hr;

        const bool debugLayer = isDebugLayerEnabled();

        UINT dxgiFactoryFlags = 0;
        if(debugLayer){
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
        CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&dxgi_factory));

        if(debugLayer){
            if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_interface)))){
                debug_interface->EnableDebugLayer();
                debug_interface->SetEnableGPUBasedValidation(
                    isGpuBasedValidationEnabled() ? TRUE : FALSE);
            }
        }

        hr = D3D12CreateDevice(device->adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d3d12_device));
        if(FAILED(hr)){
            exit(1);
        };

        // Funnel D3D12 validation messages into DEBUG_STREAM. Requires
        // ID3D12InfoQueue1 (Windows 10 21H1+ SDK); older runtimes silently
        // skip — messages still go to the debugger output stream via the
        // debug layer's default sink.
        if(debugLayer){
#ifdef __ID3D12InfoQueue1_INTERFACE_DEFINED__
            ComPtr<ID3D12InfoQueue1> infoQueue;
            if(SUCCEEDED(d3d12_device.As(&infoQueue))){
                DWORD cookie = 0;
                infoQueue->RegisterMessageCallback(
                    [](D3D12_MESSAGE_CATEGORY,
                       D3D12_MESSAGE_SEVERITY severity,
                       D3D12_MESSAGE_ID id,
                       LPCSTR description,
                       void *){
                        const char *sev = "INFO";
                        switch(severity){
                            case D3D12_MESSAGE_SEVERITY_CORRUPTION: sev = "CORRUPTION"; break;
                            case D3D12_MESSAGE_SEVERITY_ERROR:      sev = "ERROR";      break;
                            case D3D12_MESSAGE_SEVERITY_WARNING:    sev = "WARN";       break;
                            case D3D12_MESSAGE_SEVERITY_INFO:       sev = "INFO";       break;
                            case D3D12_MESSAGE_SEVERITY_MESSAGE:    sev = "MSG";        break;
                        }
                        DEBUG_STREAM("D3D12 [" << sev << " id=" << static_cast<int>(id)
                                     << "] " << (description ? description : "(null)"));
                    },
                    D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                    nullptr,
                    &cookie);
                debug_message_cookie = cookie;
                debug_info_queue = infoQueue;
            }
#endif
        }

        D3D12MA::ALLOCATOR_DESC allocatorDesc = {};
        allocatorDesc.pDevice = d3d12_device.Get();
        allocatorDesc.pAdapter = device->adapter.Get();
        allocatorDesc.Flags = D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED;
        hr = D3D12MA::CreateAllocator(&allocatorDesc, &memAllocator);
        if(FAILED(hr)){
            DEBUG_STREAM("D3D12MA::CreateAllocator failed");
            exit(1);
        };

        gteDevice = std::static_pointer_cast<GTEDevice>(device);
        _deviceFeatures = gteDevice->features.featuresAsBitmask();

        DEBUG_STREAM("GED3D12Engine Intialized!");

    };

    void GED3D12Engine::waitForGPUIdle(){
        // Per-queue Signal+wait. liveCommandQueues holds weak refs to
        // every command queue the engine has handed out; locking each
        // and calling commitToGPUAndWait flushes any queued command
        // lists and blocks on the queue's binary fence. Any queue
        // whose owner has already dropped (lock returns null) has by
        // definition already drained on its own destructor path, so
        // there is nothing to wait on there. Safe to call repeatedly
        // and from any callsite that needs a GPU drain before
        // releasing GPU-referenced resources.
        for(auto & weakQ : liveCommandQueues){
            if(auto q = weakQ.lock()){
                q->commitToGPUAndWait();
            }
        }
    }

    GED3D12Engine::~GED3D12Engine(){
        // Block on every still-live command queue so any pending GPU
        // work finishes before we drain the retention queue. Shared
        // with the public waitForGPUIdle() override so callers that
        // reach into Close()/Compositor teardown get the same GPU
        // drain semantics the destructor relies on.
        waitForGPUIdle();
        liveCommandQueues.clear();

        // GPU is provably idle now — every retention gate would report
        // signaled. drainAll runs releases unconditionally, which avoids
        // the cost of querying every gate one more time.
        retentionQueue.drainAll();

        if(memAllocator){
            memAllocator->Release();
            memAllocator = nullptr;
        }

#ifdef __ID3D12InfoQueue1_INTERFACE_DEFINED__
        if(debug_info_queue && debug_message_cookie != 0){
            debug_info_queue->UnregisterMessageCallback(debug_message_cookie);
            debug_message_cookie = 0;
            debug_info_queue.Reset();
        }
#endif
    };

    SharedHandle<GTEShader> GED3D12Engine::_loadShaderFromDesc(omegasl_shader *shaderDesc,bool runtime) {
        auto shader = new GED3D12Shader();
        shader->internal = *shaderDesc;
        shader->shaderBytecode.pShaderBytecode = shaderDesc->data;
        shader->shaderBytecode.BytecodeLength = shaderDesc->dataSize;
        return SharedHandle<GTEShader>(shader);
    }

    // OmegaSL source for the box-filter mipmap downsample. Mirrors
    // gte/src/shaders/mipmap_gen_2d.omegasl; embedded so the engine has no
    // filesystem dependency at runtime.
    static const char *kMipmapGen2DOmegaSL = R"(
texture2d srcMip : 0;
texture2d dstMip : 1;

[in srcMip, out dstMip]
compute(x=8, y=8, z=1)
void mipmap_gen_2d_kernel(uint3 tid : GlobalThreadID){
    int2 dstCoord = make_int2((int)tid[0], (int)tid[1]);
    int2 srcCoord = make_int2(dstCoord[0] * 2, dstCoord[1] * 2);

    float4 c0 = read(srcMip, srcCoord);
    float4 c1 = read(srcMip, make_int2(srcCoord[0] + 1, srcCoord[1]));
    float4 c2 = read(srcMip, make_int2(srcCoord[0],     srcCoord[1] + 1));
    float4 c3 = read(srcMip, make_int2(srcCoord[0] + 1, srcCoord[1] + 1));

    write(dstMip, dstCoord, (c0 + c1 + c2 + c3) * 0.25);
}
)";

    // OmegaSL source for the full-screen-triangle vertex shader used by every
    // Extension 3 blit pipeline. The rasterizer output struct
    // `OmegaGTEBlitVertexData` is the contract the user-supplied fragment
    // shader must consume (see BlitPipelineDescriptor doxygen in GEPipeline.h).
    static const char *kBlitFullscreenVsOmegaSL = R"(
struct OmegaGTEBlitVertexData internal {
    float4 pos : Position;
    float2 uv  : TexCoord;
};

vertex OmegaGTEBlitVertexData omega_gte_blit_fullscreen_vs(uint vid : VertexID){
    OmegaGTEBlitVertexData r;
    float u = (float)((vid << 1) & 2);
    float v = (float)(vid & 2);
    r.pos = make_float4(u * 2.0 - 1.0, 1.0 - v * 2.0, 0.0, 1.0);
    r.uv  = make_float2(u, v);
    return r;
}
)";

    bool GED3D12Engine::ensureBlitFullscreenVs() {
        if (blitFullscreenVs) return true;
        try {
            auto compiler = OmegaSLCompiler::Create(gteDevice);
            if (!compiler) {
                DEBUG_STREAM("ensureBlitFullscreenVs: OmegaSLCompiler::Create returned null");
                return false;
            }
            OmegaCommon::String src(kBlitFullscreenVsOmegaSL);
            auto source = OmegaSLCompiler::Source::fromString(src);
            blitFullscreenVsLib = compiler->compile({source});
            if (!blitFullscreenVsLib || blitFullscreenVsLib->header.entry_count == 0) {
                DEBUG_STREAM("ensureBlitFullscreenVs: OmegaSL compile produced no shaders");
                blitFullscreenVsLib.reset();
                return false;
            }
            omegasl_shader *shaderDesc = &blitFullscreenVsLib->shaders[0];
            auto shader = _loadShaderFromDesc(shaderDesc, true);
            if (!shader) {
                DEBUG_STREAM("ensureBlitFullscreenVs: _loadShaderFromDesc failed");
                blitFullscreenVsLib.reset();
                return false;
            }
            blitFullscreenVs = shader;
            return true;
        } catch (const std::exception &e) {
            DEBUG_STREAM("ensureBlitFullscreenVs: exception: " << e.what());
            blitFullscreenVs.reset();
            blitFullscreenVsLib.reset();
            return false;
        }
    }

    bool GED3D12Engine::ensureMipmapGenPipeline() {
        if (mipmapGenPipeline) return true;

        try {
            auto compiler = OmegaSLCompiler::Create(gteDevice);
            if (!compiler) {
                DEBUG_STREAM("ensureMipmapGenPipeline: OmegaSLCompiler::Create returned null");
                return false;
            }

            OmegaCommon::String src(kMipmapGen2DOmegaSL);
            auto source = OmegaSLCompiler::Source::fromString(src);

            mipmapGenShaderLib = compiler->compile({source});
            if (!mipmapGenShaderLib || mipmapGenShaderLib->header.entry_count == 0) {
                DEBUG_STREAM("ensureMipmapGenPipeline: OmegaSL compile produced no shaders");
                mipmapGenShaderLib.reset();
                return false;
            }

            // The kernel is the only entry point in the library.
            omegasl_shader *shaderDesc = &mipmapGenShaderLib->shaders[0];
            auto shader = _loadShaderFromDesc(shaderDesc, true);
            if (!shader) {
                DEBUG_STREAM("ensureMipmapGenPipeline: _loadShaderFromDesc failed");
                mipmapGenShaderLib.reset();
                return false;
            }

            ComputePipelineDescriptor desc{};
            desc.name = "OmegaGTE.Internal.MipmapGen2D";
            desc.computeFunc = shader;

            mipmapGenPipeline = makeComputePipelineState(desc);
            if (!mipmapGenPipeline) {
                DEBUG_STREAM("ensureMipmapGenPipeline: makeComputePipelineState returned null");
                mipmapGenShaderLib.reset();
                return false;
            }
            return true;
        } catch (const std::exception &e) {
            DEBUG_STREAM("ensureMipmapGenPipeline: exception: " << e.what());
            mipmapGenPipeline.reset();
            mipmapGenShaderLib.reset();
            return false;
        }
    }

    ID3D12CommandSignature * GED3D12Engine::getDrawIndirectSignature() {
        if (drawIndirectSignature) return drawIndirectSignature.Get();
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
        sigDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        sigDesc.NumArgumentDescs = 1;
        sigDesc.pArgumentDescs = &arg;
        sigDesc.NodeMask = 0;
        HRESULT hr = d3d12_device->CreateCommandSignature(&sigDesc, nullptr,
                                                          IID_PPV_ARGS(&drawIndirectSignature));
        if (FAILED(hr)) {
            DEBUG_STREAM("getDrawIndirectSignature: CreateCommandSignature failed hr=" << std::hex << hr);
            return nullptr;
        }
        return drawIndirectSignature.Get();
    }

    ID3D12CommandSignature * GED3D12Engine::getDrawIndexedIndirectSignature() {
        if (drawIndexedIndirectSignature) return drawIndexedIndirectSignature.Get();
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
        sigDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        sigDesc.NumArgumentDescs = 1;
        sigDesc.pArgumentDescs = &arg;
        sigDesc.NodeMask = 0;
        HRESULT hr = d3d12_device->CreateCommandSignature(&sigDesc, nullptr,
                                                          IID_PPV_ARGS(&drawIndexedIndirectSignature));
        if (FAILED(hr)) {
            DEBUG_STREAM("getDrawIndexedIndirectSignature: CreateCommandSignature failed hr=" << std::hex << hr);
            return nullptr;
        }
        return drawIndexedIndirectSignature.Get();
    }

    ID3D12CommandSignature * GED3D12Engine::getDispatchIndirectSignature() {
        if (dispatchIndirectSignature) return dispatchIndirectSignature.Get();
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
        sigDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        sigDesc.NumArgumentDescs = 1;
        sigDesc.pArgumentDescs = &arg;
        sigDesc.NodeMask = 0;
        HRESULT hr = d3d12_device->CreateCommandSignature(&sigDesc, nullptr,
                                                          IID_PPV_ARGS(&dispatchIndirectSignature));
        if (FAILED(hr)) {
            DEBUG_STREAM("getDispatchIndirectSignature: CreateCommandSignature failed hr=" << std::hex << hr);
            return nullptr;
        }
        return dispatchIndirectSignature.Get();
    }

    using D3DByte = unsigned char;

    class GED3D12BufferWriter : public GEBufferWriter {
        GED3D12Buffer * _buffer = nullptr;
        D3DByte *_data_buffer = nullptr;

        bool inStruct=false;
        OmegaCommon::Vector<DataBlock> blocks;
        size_t currentOffset = 0;
        /// §2.4 — std140 (HLSL `cbuffer`, column-major) when the bound buffer
        /// is a uniform/constant buffer; derived from its role.
        BufferLayoutStd layoutStd = BufferLayoutStd::Std430;
    public:
        void setOutputBuffer(SharedHandle<GEBuffer> &buffer) override {
            _buffer = (GED3D12Buffer *)buffer.get();
            layoutStd = (_buffer->role == BufferDescriptor::Uniform)
                            ? BufferLayoutStd::Std140 : BufferLayoutStd::Std430;
            HRESULT hr = _buffer->buffer->Map(0, nullptr, (void **)&_data_buffer);
            {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                    "[GED3D12 BufferWriter] Map(0,nullptr): hr=0x%08X _data_buffer=%p\n",
                    (unsigned)hr, (void *)_data_buffer);
                OutputDebugStringA(msg);
            }
            currentOffset = 0;
        }
        void structBegin() override {
            if(!blocks.empty()){
                blocks.clear();
            }
            inStruct = true;
        }
        void writeFloat(float &v) override {
            DataBlock block{
                OMEGASL_FLOAT,
                new float(v)};
            blocks.push_back(block);
        }
        void writeFloat2(FVec<2> &v) override {
           DataBlock block{
               OMEGASL_FLOAT2,
               new DirectX::XMFLOAT2{v[0][0],v[1][0]}
           };
           blocks.push_back(block);
        }
        void writeFloat3(FVec<3> &v) override {
            DataBlock block {
                OMEGASL_FLOAT3,
                new DirectX::XMFLOAT3 {v[0][0],v[1][0],v[2][0]}
            };
            blocks.push_back(block);
        }
        void writeFloat4(FVec<4> &v) override {
            DataBlock block {
                OMEGASL_FLOAT4,
                new DirectX::XMFLOAT4 {v[0][0],v[1][0],v[2][0],v[3][0]}
            };
            blocks.push_back(block);
        }
        void writeInt(int &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT,new int(v)});
        }
        void writeInt2(IVec<2> &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT2,new DirectX::XMINT2{v[0][0],v[1][0]}});
        }
        void writeInt3(IVec<3> &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT3,new DirectX::XMINT3{v[0][0],v[1][0],v[2][0]}});
        }
        void writeInt4(IVec<4> &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT4,new DirectX::XMINT4{v[0][0],v[1][0],v[2][0],v[3][0]}});
        }
        void writeUint(unsigned &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT,new unsigned(v)});
        }
        void writeUint2(UVec<2> &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT2,new DirectX::XMUINT2{v[0][0],v[1][0]}});
        }
        void writeUint3(UVec<3> &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT3,new DirectX::XMUINT3{v[0][0],v[1][0],v[2][0]}});
        }
        void writeUint4(UVec<4> &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT4,new DirectX::XMUINT4{v[0][0],v[1][0],v[2][0],v[3][0]}});
        }
        /// Matrix uploads — column-major std430-style bytes. With the
        /// HLSL packing lock (see HLSLTarget runtime + offline), HLSL
        /// loads these column-by-column with the Cx3 vec3 padding,
        /// matching Vulkan/Metal layouts. See §12.2.
        void writeFloat2x2(FMatrix<2,2> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<2,2>(), encodeFMatrix<2,2>(m, layoutStd)});
        }
        void writeFloat3x3(FMatrix<3,3> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<3,3>(), encodeFMatrix<3,3>(m, layoutStd)});
        }
        void writeFloat4x4(FMatrix<4,4> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<4,4>(), encodeFMatrix<4,4>(m, layoutStd)});
        }
        void writeFloat2x3(FMatrix<2,3> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<2,3>(), encodeFMatrix<2,3>(m, layoutStd)});
        }
        void writeFloat2x4(FMatrix<2,4> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<2,4>(), encodeFMatrix<2,4>(m, layoutStd)});
        }
        void writeFloat3x2(FMatrix<3,2> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<3,2>(), encodeFMatrix<3,2>(m, layoutStd)});
        }
        void writeFloat3x4(FMatrix<3,4> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<3,4>(), encodeFMatrix<3,4>(m, layoutStd)});
        }
        void writeFloat4x2(FMatrix<4,2> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<4,2>(), encodeFMatrix<4,2>(m, layoutStd)});
        }
        void writeFloat4x3(FMatrix<4,3> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<4,3>(), encodeFMatrix<4,3>(m, layoutStd)});
        }
        /// §12.2 follow-up — integer / unsigned matrix writers. Same
        /// column-major std430/std140 packing as the float writers; the HLSL
        /// shader consumes them as `int4 m[C]` arrays (column-major natural).
        void writeInt2x2(IMatrix<2,2> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<2,2>(), encodeMatrix<int,2,2>(m, layoutStd)}); }
        void writeInt3x3(IMatrix<3,3> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<3,3>(), encodeMatrix<int,3,3>(m, layoutStd)}); }
        void writeInt4x4(IMatrix<4,4> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<4,4>(), encodeMatrix<int,4,4>(m, layoutStd)}); }
        void writeInt2x3(IMatrix<2,3> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<2,3>(), encodeMatrix<int,2,3>(m, layoutStd)}); }
        void writeInt2x4(IMatrix<2,4> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<2,4>(), encodeMatrix<int,2,4>(m, layoutStd)}); }
        void writeInt3x2(IMatrix<3,2> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<3,2>(), encodeMatrix<int,3,2>(m, layoutStd)}); }
        void writeInt3x4(IMatrix<3,4> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<3,4>(), encodeMatrix<int,3,4>(m, layoutStd)}); }
        void writeInt4x2(IMatrix<4,2> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<4,2>(), encodeMatrix<int,4,2>(m, layoutStd)}); }
        void writeInt4x3(IMatrix<4,3> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<4,3>(), encodeMatrix<int,4,3>(m, layoutStd)}); }
        void writeUint2x2(UMatrix<2,2> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<2,2>(), encodeMatrix<unsigned,2,2>(m, layoutStd)}); }
        void writeUint3x3(UMatrix<3,3> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<3,3>(), encodeMatrix<unsigned,3,3>(m, layoutStd)}); }
        void writeUint4x4(UMatrix<4,4> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<4,4>(), encodeMatrix<unsigned,4,4>(m, layoutStd)}); }
        void writeUint2x3(UMatrix<2,3> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<2,3>(), encodeMatrix<unsigned,2,3>(m, layoutStd)}); }
        void writeUint2x4(UMatrix<2,4> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<2,4>(), encodeMatrix<unsigned,2,4>(m, layoutStd)}); }
        void writeUint3x2(UMatrix<3,2> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<3,2>(), encodeMatrix<unsigned,3,2>(m, layoutStd)}); }
        void writeUint3x4(UMatrix<3,4> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<3,4>(), encodeMatrix<unsigned,3,4>(m, layoutStd)}); }
        void writeUint4x2(UMatrix<4,2> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<4,2>(), encodeMatrix<unsigned,4,2>(m, layoutStd)}); }
        void writeUint4x3(UMatrix<4,3> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<4,3>(), encodeMatrix<unsigned,4,3>(m, layoutStd)}); }
        void structEnd() override {
            inStruct = false;
        }

        void sendToBuffer() override {
            assert(!inStruct && "");
            if (_data_buffer == nullptr)
                return;
            /// §2.4-1 — align-then-place: pad the cursor up to each member's
            /// base alignment (zero-filling the gap) before the copy, so a
            /// mixed field order such as `{float, float4}` (or std140's
            /// 16-strided matrix columns for a uniform buffer) lands every
            /// member on the offset the HLSL shader reads it from.
            for(auto & block : blocks){
                size_t dataSize = 0;
                if(isMatrixDataType(block.type)){
                    auto [cols, rows] = matrixDims(block.type);
                    dataSize = matrixSize(cols, rows, layoutStd);
                }
                else if(block.type == OMEGASL_FLOAT){
                    dataSize = sizeof(float);
                }
                else if(block.type == OMEGASL_FLOAT2){
                    dataSize = sizeof(DirectX::XMFLOAT2);
                }
                else if(block.type == OMEGASL_FLOAT3){
                    dataSize = sizeof(DirectX::XMFLOAT3);
                }
                else if(block.type == OMEGASL_FLOAT4){
                    dataSize = sizeof(DirectX::XMFLOAT4);
                }
                else if(block.type == OMEGASL_INT || block.type == OMEGASL_UINT){
                    dataSize = sizeof(int);
                }
                else if(block.type == OMEGASL_INT2 || block.type == OMEGASL_UINT2){
                    dataSize = sizeof(DirectX::XMINT2);
                }
                else if(block.type == OMEGASL_INT3 || block.type == OMEGASL_UINT3){
                    dataSize = sizeof(DirectX::XMINT3);
                }
                else if(block.type == OMEGASL_INT4 || block.type == OMEGASL_UINT4){
                    dataSize = sizeof(DirectX::XMINT4);
                }
                size_t a = memberBaseAlignment(block.type, layoutStd);
                size_t aligned = alignOffset(currentOffset, a);
                if(aligned > currentOffset){
                    memset(_data_buffer + currentOffset, 0, aligned - currentOffset);
                }
                currentOffset = aligned;
                memcpy(_data_buffer + currentOffset,block.data,dataSize);
                currentOffset += dataSize;
            }
            if (currentOffset == 32u) {
                const float *f = reinterpret_cast<const float *>(_data_buffer);
                char msg[320];
                std::snprintf(msg, sizeof(msg),
                    "[GED3D12 BufferWriter] first vertex: pos=(%.3f,%.3f,%.3f,%.3f) color=(%.3f,%.3f,%.3f,%.3f)\n",
                    f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
                OutputDebugStringA(msg);
            }
        }

        void flush() override {
            _buffer->buffer->Unmap(0,nullptr);
            _buffer = nullptr;
            _data_buffer = nullptr;
            // std::cout << "LastOffset:" << currentOffset << std::endl;
            currentOffset = 0;
        }
        ~GED3D12BufferWriter() override = default;
    };

    SharedHandle<GEBufferWriter> GEBufferWriter::Create() {
        return SharedHandle<GEBufferWriter>(new GED3D12BufferWriter());
    }

    class GED3D12BufferReader : public GEBufferReader {
        GED3D12Buffer * _buffer = nullptr;
        D3DByte *_data_buffer = nullptr;
        size_t currentOffset = 0;
        /// §2.4 — matches the writer: std140 for uniform buffers.
        BufferLayoutStd layoutStd = BufferLayoutStd::Std430;
        /// §2.4-1 — align-then-read: advance the cursor to the field's base
        /// alignment before each read, mirroring the writer's inter-member
        /// padding.
        inline void alignRead(omegasl_data_type t){
            currentOffset = alignOffset(currentOffset, memberBaseAlignment(t, layoutStd));
        }
    public:
        void setInputBuffer(SharedHandle<GEBuffer> &buffer) override {
            currentOffset = 0;
            _buffer = (GED3D12Buffer *)buffer.get();
            layoutStd = (_buffer->role == BufferDescriptor::Uniform)
                            ? BufferLayoutStd::Std140 : BufferLayoutStd::Std430;
            CD3DX12_RANGE range(0,0);

            _buffer->buffer->Map(0,&range,(void **)&_data_buffer);
        }
        void setStructLayout(OmegaCommon::Vector<omegasl_data_type> fields) override {

        }
        void structBegin() override {

        }
        void getFloat(float &v) override {
            alignRead(OMEGASL_FLOAT);
            memcpy(&v,_data_buffer + currentOffset,sizeof(v));
            currentOffset += sizeof(v);
        }
        void getFloat2(FVec<2> &v) override {
            alignRead(OMEGASL_FLOAT2);
            DirectX::XMFLOAT2 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
        }
        void getFloat3(FVec<3> &v) override {
            alignRead(OMEGASL_FLOAT3);
            DirectX::XMFLOAT3 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
            v[2][0] = _v.z;
        }
        void getFloat4(FVec<4> &v) override {
            alignRead(OMEGASL_FLOAT4);
            DirectX::XMFLOAT4 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
            v[2][0] = _v.z;
            v[3][0] = _v.w;
        }
        /// Integer / unsigned scalar + vector downloads — symmetric with
        /// `writeInt*` / `writeUint*`. int/uint share the byte layout; the
        /// unsigned readers use the `XMUINT*` types but otherwise mirror int.
        void getInt(int &v) override {
            alignRead(OMEGASL_INT);
            memcpy(&v,_data_buffer + currentOffset,sizeof(v));
            currentOffset += sizeof(v);
        }
        void getInt2(IVec<2> &v) override {
            alignRead(OMEGASL_INT2);
            DirectX::XMINT2 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
        }
        void getInt3(IVec<3> &v) override {
            alignRead(OMEGASL_INT3);
            DirectX::XMINT3 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
            v[2][0] = _v.z;
        }
        void getInt4(IVec<4> &v) override {
            alignRead(OMEGASL_INT4);
            DirectX::XMINT4 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
            v[2][0] = _v.z;
            v[3][0] = _v.w;
        }
        void getUint(unsigned &v) override {
            alignRead(OMEGASL_UINT);
            memcpy(&v,_data_buffer + currentOffset,sizeof(v));
            currentOffset += sizeof(v);
        }
        void getUint2(UVec<2> &v) override {
            alignRead(OMEGASL_UINT2);
            DirectX::XMUINT2 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
        }
        void getUint3(UVec<3> &v) override {
            alignRead(OMEGASL_UINT3);
            DirectX::XMUINT3 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
            v[2][0] = _v.z;
        }
        void getUint4(UVec<4> &v) override {
            alignRead(OMEGASL_UINT4);
            DirectX::XMUINT4 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
            v[2][0] = _v.z;
            v[3][0] = _v.w;
        }
        /// Matrix downloads. With the HLSL packing lock (column-major)
        /// these bytes are laid out the same way Vulkan/Metal write
        /// them, so the shared decoder works directly.
        template<class T, unsigned C, unsigned R>
        void getMatrixImpl(Matrix<T, C, R> &m, omegasl_data_type tag){
            alignRead(tag);
            decodeMatrix<T, C, R>(_data_buffer + currentOffset, m, layoutStd);
            currentOffset += matrixSize(C, R, layoutStd);
        }
        void getFloat2x2(FMatrix<2,2> &m) override { getMatrixImpl<float,2,2>(m, OMEGASL_FLOAT2x2); }
        void getFloat3x3(FMatrix<3,3> &m) override { getMatrixImpl<float,3,3>(m, OMEGASL_FLOAT3x3); }
        void getFloat4x4(FMatrix<4,4> &m) override { getMatrixImpl<float,4,4>(m, OMEGASL_FLOAT4x4); }
        void getFloat2x3(FMatrix<2,3> &m) override { getMatrixImpl<float,2,3>(m, OMEGASL_FLOAT2x3); }
        void getFloat2x4(FMatrix<2,4> &m) override { getMatrixImpl<float,2,4>(m, OMEGASL_FLOAT2x4); }
        void getFloat3x2(FMatrix<3,2> &m) override { getMatrixImpl<float,3,2>(m, OMEGASL_FLOAT3x2); }
        void getFloat3x4(FMatrix<3,4> &m) override { getMatrixImpl<float,3,4>(m, OMEGASL_FLOAT3x4); }
        void getFloat4x2(FMatrix<4,2> &m) override { getMatrixImpl<float,4,2>(m, OMEGASL_FLOAT4x2); }
        void getFloat4x3(FMatrix<4,3> &m) override { getMatrixImpl<float,4,3>(m, OMEGASL_FLOAT4x3); }
        /// §12.2 follow-up — integer / unsigned matrix readers.
        void getInt2x2(IMatrix<2,2> &m) override { getMatrixImpl<int,2,2>(m, OMEGASL_INT2x2); }
        void getInt3x3(IMatrix<3,3> &m) override { getMatrixImpl<int,3,3>(m, OMEGASL_INT3x3); }
        void getInt4x4(IMatrix<4,4> &m) override { getMatrixImpl<int,4,4>(m, OMEGASL_INT4x4); }
        void getInt2x3(IMatrix<2,3> &m) override { getMatrixImpl<int,2,3>(m, OMEGASL_INT2x3); }
        void getInt2x4(IMatrix<2,4> &m) override { getMatrixImpl<int,2,4>(m, OMEGASL_INT2x4); }
        void getInt3x2(IMatrix<3,2> &m) override { getMatrixImpl<int,3,2>(m, OMEGASL_INT3x2); }
        void getInt3x4(IMatrix<3,4> &m) override { getMatrixImpl<int,3,4>(m, OMEGASL_INT3x4); }
        void getInt4x2(IMatrix<4,2> &m) override { getMatrixImpl<int,4,2>(m, OMEGASL_INT4x2); }
        void getInt4x3(IMatrix<4,3> &m) override { getMatrixImpl<int,4,3>(m, OMEGASL_INT4x3); }
        void getUint2x2(UMatrix<2,2> &m) override { getMatrixImpl<unsigned,2,2>(m, OMEGASL_UINT2x2); }
        void getUint3x3(UMatrix<3,3> &m) override { getMatrixImpl<unsigned,3,3>(m, OMEGASL_UINT3x3); }
        void getUint4x4(UMatrix<4,4> &m) override { getMatrixImpl<unsigned,4,4>(m, OMEGASL_UINT4x4); }
        void getUint2x3(UMatrix<2,3> &m) override { getMatrixImpl<unsigned,2,3>(m, OMEGASL_UINT2x3); }
        void getUint2x4(UMatrix<2,4> &m) override { getMatrixImpl<unsigned,2,4>(m, OMEGASL_UINT2x4); }
        void getUint3x2(UMatrix<3,2> &m) override { getMatrixImpl<unsigned,3,2>(m, OMEGASL_UINT3x2); }
        void getUint3x4(UMatrix<3,4> &m) override { getMatrixImpl<unsigned,3,4>(m, OMEGASL_UINT3x4); }
        void getUint4x2(UMatrix<4,2> &m) override { getMatrixImpl<unsigned,4,2>(m, OMEGASL_UINT4x2); }
        void getUint4x3(UMatrix<4,3> &m) override { getMatrixImpl<unsigned,4,3>(m, OMEGASL_UINT4x3); }
        void structEnd() override {

        }
        void reset() override {
            _data_buffer = nullptr;
            currentOffset = 0;
            _buffer->buffer->Unmap(0,nullptr);
            _buffer = nullptr;
        }
        ~GED3D12BufferReader() override = default;
    };

    SharedHandle<GEBufferReader> GEBufferReader::Create() {
        return SharedHandle<GEBufferReader>(new GED3D12BufferReader());
    }


    IDXGISwapChain3 *GED3D12Engine::createSwapChainForComposition(DXGI_SWAP_CHAIN_DESC1 *desc,SharedHandle<GECommandQueue> & commandQueue){
        auto *d3d12_queue = (GED3D12CommandQueue *)commandQueue.get();
        ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT hr = dxgi_factory->CreateSwapChainForComposition(d3d12_queue->commandQueue.Get(),desc,nullptr,&swapChain1);
        if(FAILED(hr)){
            DEBUG_STREAM("CreateSwapChainForComposition failed hr=0x"
                         << std::hex << static_cast<unsigned long>(hr) << std::dec);
            return nullptr;
        }
        ComPtr<IDXGISwapChain3> swapChain3;
        hr = swapChain1.As(&swapChain3);
        if(FAILED(hr)){
            DEBUG_STREAM("QueryInterface(IDXGISwapChain3) failed hr=0x"
                         << std::hex << static_cast<unsigned long>(hr) << std::dec);
            return nullptr;
        }
        return swapChain3.Detach();
    }

    IDXGISwapChain3 *GED3D12Engine::createSwapChainFromHWND(HWND hwnd,DXGI_SWAP_CHAIN_DESC1 *desc,SharedHandle<GECommandQueue> & commandQueue){
        auto *d3d12_queue = (GED3D12CommandQueue *)commandQueue.get();
        ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT hr = dxgi_factory->CreateSwapChainForHwnd(d3d12_queue->commandQueue.Get(),hwnd,desc,nullptr,nullptr,&swapChain1);
        if(FAILED(hr)){
            DEBUG_STREAM("CreateSwapChainForHwnd failed hr=0x"
                         << std::hex << static_cast<unsigned long>(hr) << std::dec);
            return nullptr;
        }

        // Block DXGI's Alt+Enter borderless-fullscreen hijack on the parent
        // window. The application owns its presentation surface; we never
        // want DXGI silently switching modes behind the toolkit's back.
        dxgi_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

        ComPtr<IDXGISwapChain3> swapChain3;
        hr = swapChain1.As(&swapChain3);
        if(FAILED(hr)){
            DEBUG_STREAM("QueryInterface(IDXGISwapChain3) failed hr=0x"
                         << std::hex << static_cast<unsigned long>(hr) << std::dec);
            return nullptr;
        }
        return swapChain3.Detach();
    }

    SharedHandle<OmegaGraphicsEngine> GED3D12Engine::Create(SharedHandle<GTEDevice> & device){
        return SharedHandle<OmegaGraphicsEngine>(new GED3D12Engine(std::dynamic_pointer_cast<GTED3D12Device>(device)));
    }

    SharedHandle<GEFence> GED3D12Engine::makeFence(){
        ID3D12Fence *f;
        d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&f));
        return SharedHandle<GEFence>(new GED3D12Fence(f));
    }

    SharedHandle<GEHeap> GED3D12Engine::makeHeap(const HeapDescriptor &desc){
        // One pool, one block of the requested size — matches the previous
        // CreateHeap behavior (single contiguous heap, no growth). D3D12MA
        // takes over alignment, offset tracking, and (within the block)
        // free-list reclamation across resource lifetimes.
        D3D12MA::POOL_DESC poolDesc {};
        poolDesc.HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        poolDesc.HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        poolDesc.HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        poolDesc.HeapFlags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
        poolDesc.BlockSize = desc.len;
        poolDesc.MinBlockCount = 1;
        poolDesc.MaxBlockCount = 1;

        D3D12MA::Pool *pool = nullptr;
        HRESULT hr = memAllocator->CreatePool(&poolDesc, &pool);
        if(FAILED(hr)){
            DEBUG_STREAM("Failed to create D3D12MA Pool");
            if (pool) pool->Release();
            return nullptr;
        }
        return SharedHandle<GEHeap>(new GED3D12Heap(this, pool, desc.len));
    }

    GED3D12AccelerationStruct::GED3D12AccelerationStruct(SharedHandle<GED3D12Buffer> & structBuffer,
        SharedHandle<GED3D12Buffer> & scratchBuffer):structBuffer(structBuffer),scratchBuffer(scratchBuffer){

    }

    SharedHandle<GEBuffer> GED3D12Engine::createBoundingBoxesBuffer(OmegaCommon::ArrayRef<GERaytracingBoundingBox> boxes){
        if(!gteDevice->features.hasFeature(GTEDEVICE_FEATURE_RAYTRACING)){
            DEBUG_STREAM("Raytracing not supported on this device");
            return nullptr;
        }
        std::vector<D3D12_RAYTRACING_AABB> aabbs;
        auto d3d12_buffer = std::dynamic_pointer_cast<GED3D12Buffer>(makeBuffer({BufferDescriptor::Upload,sizeof(D3D12_RAYTRACING_AABB) * boxes.size(),sizeof(D3D12_RAYTRACING_AABB)}));
        for(auto & b : boxes){
            D3D12_RAYTRACING_AABB bb {};
            bb.MinX = b.minX;
            bb.MinY = b.minY;
            bb.MinZ = b.minZ;

            bb.MaxX = b.maxX;
            bb.MaxY = b.maxY;
            bb.MaxZ = b.maxZ;
            aabbs.push_back(bb);
        }
        CD3DX12_RANGE r(0,0);
        void *dataPtr;
        d3d12_buffer->buffer->Map(0,&r,&dataPtr);
        memmove(dataPtr,aabbs.data(),sizeof(D3D12_RAYTRACING_AABB) * aabbs.size());
        d3d12_buffer->buffer->Unmap(0,nullptr);
        return std::static_pointer_cast<GEBuffer>(d3d12_buffer);
    }

    SharedHandle<GEAccelerationStruct> GED3D12Engine::allocateAccelerationStructure(const GEAccelerationStructDescriptor &desc){
        if(!gteDevice->features.hasFeature(GTEDEVICE_FEATURE_RAYTRACING)){
            DEBUG_STREAM("Raytracing not supported on this device");
            return nullptr;
        }
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
        for(auto & g : desc.data){
            D3D12_RAYTRACING_GEOMETRY_DESC gd {};
            gd.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            if(g.type == GEAccelerationStructDescriptor::Geometry::TRIANGLES){
                gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                auto d3dBuf = std::dynamic_pointer_cast<GED3D12Buffer>(g.getTriangleList().buffer);
                if(d3dBuf){
                    gd.Triangles.VertexBuffer.StartAddress = d3dBuf->buffer->GetGPUVirtualAddress();
                    gd.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
                    gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                    gd.Triangles.VertexCount = static_cast<UINT>(d3dBuf->size() / (sizeof(float) * 3));
                }
            } else {
                gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
                auto d3dBuf = std::dynamic_pointer_cast<GED3D12Buffer>(g.getAabb().buffer);
                if(d3dBuf){
                    gd.AABBs.AABBs.StartAddress = d3dBuf->buffer->GetGPUVirtualAddress();
                    gd.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
                    gd.AABBs.AABBCount = d3dBuf->size() / sizeof(D3D12_RAYTRACING_AABB);
                }
            }
            geometryDescs.push_back(gd);
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs {};
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        if(geometryDescs.empty()){
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            inputs.NumDescs = 0;
            inputs.pGeometryDescs = nullptr;
        } else {
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.NumDescs = static_cast<UINT>(geometryDescs.size());
            inputs.pGeometryDescs = geometryDescs.data();
        }

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo {};
        d3d12_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs,&prebuildInfo);

        size_t resultSize = prebuildInfo.ResultDataMaxSizeInBytes > 0
                            ? prebuildInfo.ResultDataMaxSizeInBytes : 256;
        size_t scratchSize = prebuildInfo.ScratchDataSizeInBytes > 0
                             ? prebuildInfo.ScratchDataSizeInBytes : 256;

        auto dataBuffer = std::dynamic_pointer_cast<GED3D12Buffer>(
            makeBuffer({BufferDescriptor::GPUOnly, resultSize}));
        auto scratchBuffer = std::dynamic_pointer_cast<GED3D12Buffer>(
            makeBuffer({BufferDescriptor::GPUOnly, scratchSize}));

        return (SharedHandle<GEAccelerationStruct>)new GED3D12AccelerationStruct(dataBuffer,scratchBuffer);
    }

    inline D3D12_COMPARISON_FUNC convertCompareFunc(CompareFunc & func){
        D3D12_COMPARISON_FUNC res;
        switch (func) {
            case CompareFunc::Greater : {
                res = D3D12_COMPARISON_FUNC_GREATER;
                break;
            }
            case CompareFunc::GreaterEqual : {
                res = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
                break;
            }
            case CompareFunc::Less : {
                res = D3D12_COMPARISON_FUNC_LESS;
                break;
            }
            case CompareFunc::LessEqual : {
                res = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                break;
            }
        }
        return res;
    }

    inline D3D12_STENCIL_OP convertStencilOperation(StencilOperation & op){
        D3D12_STENCIL_OP res;
        switch (op) {
            case StencilOperation::Retain : {
                res = D3D12_STENCIL_OP_KEEP;
                break;
            }
            case StencilOperation::Replace : {
                res = D3D12_STENCIL_OP_REPLACE;
                break;
            }
            case StencilOperation::IncrementWrap : {
                res = D3D12_STENCIL_OP_INCR;
                break;
            }
            case StencilOperation::DecrementWrap : {
                res = D3D12_STENCIL_OP_DECR;
                break;
            }
            case StencilOperation::Zero : {
                res = D3D12_STENCIL_OP_ZERO;
                break;
            }
        }
        return res;
    }

    inline DXGI_FORMAT convertVertexFormatToDxgi(VertexFormat fmt){
        switch(fmt){
            case VertexFormat::Float:   return DXGI_FORMAT_R32_FLOAT;
            case VertexFormat::Float2:  return DXGI_FORMAT_R32G32_FLOAT;
            case VertexFormat::Float3:  return DXGI_FORMAT_R32G32B32_FLOAT;
            case VertexFormat::Float4:  return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case VertexFormat::Int:     return DXGI_FORMAT_R32_SINT;
            case VertexFormat::Int2:    return DXGI_FORMAT_R32G32_SINT;
            case VertexFormat::Int3:    return DXGI_FORMAT_R32G32B32_SINT;
            case VertexFormat::Int4:    return DXGI_FORMAT_R32G32B32A32_SINT;
            case VertexFormat::UInt:    return DXGI_FORMAT_R32_UINT;
            case VertexFormat::UInt2:   return DXGI_FORMAT_R32G32_UINT;
            case VertexFormat::UInt3:   return DXGI_FORMAT_R32G32B32_UINT;
            case VertexFormat::UInt4:   return DXGI_FORMAT_R32G32B32A32_UINT;
            case VertexFormat::UNorm8x4: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case VertexFormat::SNorm8x4: return DXGI_FORMAT_R8G8B8A8_SNORM;
            case VertexFormat::UShort2: return DXGI_FORMAT_R16G16_UINT;
            case VertexFormat::UShort4: return DXGI_FORMAT_R16G16B16A16_UINT;
            case VertexFormat::Half2:   return DXGI_FORMAT_R16G16_FLOAT;
            case VertexFormat::Half4:   return DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
        return DXGI_FORMAT_UNKNOWN;
    }

    inline D3D12_BLEND convertBlendFactor(BlendFactor f){
        switch(f){
            case BlendFactor::Zero:             return D3D12_BLEND_ZERO;
            case BlendFactor::One:              return D3D12_BLEND_ONE;
            case BlendFactor::SrcColor:         return D3D12_BLEND_SRC_COLOR;
            case BlendFactor::InvSrcColor:      return D3D12_BLEND_INV_SRC_COLOR;
            case BlendFactor::SrcAlpha:         return D3D12_BLEND_SRC_ALPHA;
            case BlendFactor::InvSrcAlpha:      return D3D12_BLEND_INV_SRC_ALPHA;
            case BlendFactor::DestColor:        return D3D12_BLEND_DEST_COLOR;
            case BlendFactor::InvDestColor:     return D3D12_BLEND_INV_DEST_COLOR;
            case BlendFactor::DestAlpha:        return D3D12_BLEND_DEST_ALPHA;
            case BlendFactor::InvDestAlpha:     return D3D12_BLEND_INV_DEST_ALPHA;
            case BlendFactor::SrcAlphaSaturated:return D3D12_BLEND_SRC_ALPHA_SAT;
            case BlendFactor::Src1Color:        return D3D12_BLEND_SRC1_COLOR;
            case BlendFactor::InvSrc1Color:     return D3D12_BLEND_INV_SRC1_COLOR;
            case BlendFactor::Src1Alpha:        return D3D12_BLEND_SRC1_ALPHA;
            case BlendFactor::InvSrc1Alpha:     return D3D12_BLEND_INV_SRC1_ALPHA;
        }
        return D3D12_BLEND_ONE;
    }

    inline D3D12_BLEND_OP convertBlendOperation(BlendOperation op){
        switch(op){
            case BlendOperation::Add:             return D3D12_BLEND_OP_ADD;
            case BlendOperation::Subtract:        return D3D12_BLEND_OP_SUBTRACT;
            case BlendOperation::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
            case BlendOperation::Min:             return D3D12_BLEND_OP_MIN;
            case BlendOperation::Max:             return D3D12_BLEND_OP_MAX;
        }
        return D3D12_BLEND_OP_ADD;
    }

    inline UINT8 convertColorWriteMask(uint8_t mask){
        UINT8 res = 0;
        if(mask & ColorWriteRed)   res |= D3D12_COLOR_WRITE_ENABLE_RED;
        if(mask & ColorWriteGreen) res |= D3D12_COLOR_WRITE_ENABLE_GREEN;
        if(mask & ColorWriteBlue)  res |= D3D12_COLOR_WRITE_ENABLE_BLUE;
        if(mask & ColorWriteAlpha) res |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
        return res;
    }

    SharedHandle<GERenderPipelineState> GED3D12Engine::makeRenderPipelineState(RenderPipelineDescriptor &desc){
        if(!_checkPipelineShader(desc.vertexFunc,"vertex",desc.name) ||
           !_checkPipelineShader(desc.fragmentFunc,"fragment",desc.name)){
            return nullptr;
        }
        auto & vertexFunc = desc.vertexFunc->internal;
        auto & fragmentFunc = desc.fragmentFunc->internal;

        DEBUG_STREAM("Making D3D12RenderPipelineState");

        std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;

        assert(desc.vertexFunc && "Vertex Function is not provided");
        assert(desc.fragmentFunc && "Fragment Function is not provided");

        assert(vertexFunc.type == OMEGASL_SHADER_VERTEX && "Function is not a vertex function");
        assert(fragmentFunc.type == OMEGASL_SHADER_FRAGMENT && "Function is not a fragment function");

        D3D12_INPUT_LAYOUT_DESC inputLayoutDesc {};
        if(vertexFunc.vertexShaderInputDesc.useVertexID){
            // Vertex-ID driven shaders consume system values and structured buffers,
            // so no IA vertex layout is required.
            inputLayoutDesc.NumElements = 0;
            inputLayoutDesc.pInputElementDescs = nullptr;
        }
        else if(!desc.vertexInputDescriptor.attributes.empty()){
            // Descriptor-driven vertex input layout.
            auto & vid = desc.vertexInputDescriptor;
            for(auto & attr : vid.attributes){
                D3D12_INPUT_ELEMENT_DESC el {};
                el.SemanticName = "TEXCOORD";
                el.SemanticIndex = attr.shaderLocation;
                el.Format = convertVertexFormatToDxgi(attr.format);
                el.InputSlot = attr.bufferIndex;
                el.AlignedByteOffset = attr.offset;
                VertexStepFunction step = VertexStepFunction::PerVertex;
                unsigned stepRate = 0;
                if(attr.bufferIndex < vid.bufferLayouts.size()){
                    step = vid.bufferLayouts[attr.bufferIndex].stepFunction;
                    stepRate = vid.bufferLayouts[attr.bufferIndex].stepRate;
                }
                if(step == VertexStepFunction::PerInstance){
                    el.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                    el.InstanceDataStepRate = stepRate;
                }
                else {
                    el.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                    el.InstanceDataStepRate = 0;
                }
                inputs.push_back(el);
            }
            inputLayoutDesc.pInputElementDescs = inputs.data();
            inputLayoutDesc.NumElements = (UINT)inputs.size();
        }
        else {
            ArrayRef<omegasl_vertex_shader_param_desc> inputDesc{vertexFunc.vertexShaderInputDesc.pParams,
                                                                 vertexFunc.vertexShaderInputDesc.pParams +
                                                                 vertexFunc.vertexShaderInputDesc.nParam};
            for (auto &attr: inputDesc) {
                D3D12_INPUT_ELEMENT_DESC inputEl {};
                switch (attr.type) {
                    case OMEGASL_FLOAT : {
                        inputEl.Format = DXGI_FORMAT_R32_FLOAT;
                        break;
                    }
                    case OMEGASL_FLOAT2 : {
                        inputEl.Format = DXGI_FORMAT_R32G32_FLOAT;
                        break;
                    }
                    case OMEGASL_FLOAT3 : {
                        inputEl.Format = DXGI_FORMAT_R32G32B32_FLOAT;
                        break;
                    }
                    case OMEGASL_FLOAT4 : {
                        inputEl.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                        break;
                    }
                    case OMEGASL_INT : {
                        inputEl.Format = DXGI_FORMAT_R32_SINT;
                        break;
                    }
                    case OMEGASL_INT2 : {
                        inputEl.Format = DXGI_FORMAT_R32G32_SINT;
                        break;
                    }
                    case OMEGASL_INT3 : {
                        inputEl.Format = DXGI_FORMAT_R32G32B32_SINT;
                        break;
                    }
                    case OMEGASL_INT4 : {
                        inputEl.Format = DXGI_FORMAT_R32G32B32A32_SINT;
                        break;
                    }
                };

                inputEl.InputSlot = 0;
                inputEl.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
                inputEl.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                inputs.push_back(inputEl);

            }
            inputLayoutDesc.pInputElementDescs = inputs.data();
            inputLayoutDesc.NumElements = vertexFunc.vertexShaderInputDesc.nParam;

        };


//        MessageBoxA(GetForegroundWindow(),"Creating Pipeline State","NOTE",MB_OK);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC d {};
        d.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        {
            // Honour per-attachment blend state from the descriptor.
            // When the caller supplies more than one BlendDescriptor, enable
            // independent per-target blending.
            const auto & blendDescs = desc.colorBlendDescriptors;
            d.BlendState.IndependentBlendEnable = blendDescs.size() > 1 ? TRUE : FALSE;
            for(unsigned i = 0; i < 8; ++i){
                auto & rt = d.BlendState.RenderTarget[i];
                rt.LogicOpEnable = FALSE;
                if(i < blendDescs.size()){
                    const auto & b = blendDescs[i];
                    rt.BlendEnable          = b.blendEnabled ? TRUE : FALSE;
                    rt.SrcBlend             = convertBlendFactor(b.srcColorFactor);
                    rt.DestBlend            = convertBlendFactor(b.destColorFactor);
                    rt.BlendOp              = convertBlendOperation(b.colorOp);
                    rt.SrcBlendAlpha        = convertBlendFactor(b.srcAlphaFactor);
                    rt.DestBlendAlpha       = convertBlendFactor(b.destAlphaFactor);
                    rt.BlendOpAlpha         = convertBlendOperation(b.alphaOp);
                    rt.RenderTargetWriteMask = convertColorWriteMask(b.writeMask);
                }
                else {
                    // No descriptor supplied → blending disabled, opaque write.
                    rt.BlendEnable = FALSE;
                    rt.SrcBlend = D3D12_BLEND_ONE;
                    rt.DestBlend = D3D12_BLEND_ZERO;
                    rt.BlendOp = D3D12_BLEND_OP_ADD;
                    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                    rt.DestBlendAlpha = D3D12_BLEND_ZERO;
                    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                }
            }
        }
        d.NodeMask = d3d12_device->GetNodeCount();
        d.InputLayout = inputLayoutDesc;
        
        HRESULT hr;

        omegasl_shader shaders[] = {desc.vertexFunc->internal,desc.fragmentFunc->internal};

        ID3D12RootSignature *signature;
        D3D12_ROOT_SIGNATURE_DESC1 rootSigDesc;
        bool b = createRootSignatureFromOmegaSLShaders(2,shaders,&rootSigDesc,&signature);

        if(!b){
            DEBUG_STREAM("Failed to Create Root Signature");
            exit(1);
        }

        auto *vertexShader = (GED3D12Shader *)desc.vertexFunc.get(),
        *fragmentShader = (GED3D12Shader *)desc.fragmentFunc.get();

        d.VS = vertexShader->shaderBytecode;
        d.PS = fragmentShader->shaderBytecode;
        d.pRootSignature = signature;
        {
            const auto & formats = desc.colorPixelFormats;
            const unsigned rtCount = formats.empty() ? 1u : (unsigned)std::min<size_t>(formats.size(), 8);
            for(unsigned i = 0; i < rtCount; ++i){
                d.RTVFormats[i] = pixelFormatToDxgiFormat(
                    formats.empty() ? PixelFormat::RGBA8Unorm : formats[i]);
            }
            for(unsigned i = rtCount; i < 8; ++i){
                d.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
            }
            d.NumRenderTargets = rtCount;
        }
        d.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

        D3D12_CULL_MODE cullMode;
        switch (desc.cullMode) {
            case RasterCullMode::None : {
                cullMode = D3D12_CULL_MODE_NONE;
                break;
            }
            case RasterCullMode::Front : {
                cullMode = D3D12_CULL_MODE_FRONT;
                break;
            }
            case RasterCullMode::Back : {
                cullMode = D3D12_CULL_MODE_BACK;
                break;
            }
        }

        d.RasterizerState.CullMode = cullMode;
        
        D3D12_FILL_MODE fillMode;
        switch (desc.triangleFillMode) {
            case TriangleFillMode::Wireframe : {
                fillMode = D3D12_FILL_MODE_WIREFRAME;
                break;
            }
            case TriangleFillMode::Solid : {
                fillMode = D3D12_FILL_MODE_SOLID;
                break;
            }
        }
        
        d.RasterizerState.FillMode = fillMode;
        d.RasterizerState.ForcedSampleCount = desc.rasterSampleCount;
        d.RasterizerState.FrontCounterClockwise = desc.polygonFrontFaceRotation == GTEPolygonFrontFaceRotation::CounterClockwise? TRUE : FALSE;
        d.RasterizerState.DepthBias = (INT)desc.depthAndStencilDesc.depthBias;
        d.RasterizerState.SlopeScaledDepthBias = desc.depthAndStencilDesc.slopeScale;
        d.RasterizerState.DepthBiasClamp = desc.depthAndStencilDesc.depthClamp;

        switch(desc.primitiveTopologyCategory){
            case PrimitiveTopologyCategory::Line:
                d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
                break;
            case PrimitiveTopologyCategory::Point:
                d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
                break;
            case PrimitiveTopologyCategory::Triangle:
            default:
                d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                break;
        }
        d.DepthStencilState.StencilEnable = desc.depthAndStencilDesc.enableStencil;
        d.DepthStencilState.DepthEnable = desc.depthAndStencilDesc.enableDepth;
        d.DepthStencilState.DepthFunc = convertCompareFunc(desc.depthAndStencilDesc.depthOperation);
        if(desc.depthAndStencilDesc.writeAmount == DepthWriteAmount::All) {
            d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        }
        else {
            d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }

        d.DepthStencilState.StencilReadMask = desc.depthAndStencilDesc.stencilReadMask;
        d.DepthStencilState.StencilWriteMask = desc.depthAndStencilDesc.stencilWriteMask;

        /// Front Face Stencil
        d.DepthStencilState.FrontFace.StencilDepthFailOp = convertStencilOperation(desc.depthAndStencilDesc.frontFaceStencil.depthFail);
        d.DepthStencilState.FrontFace.StencilFailOp = convertStencilOperation(desc.depthAndStencilDesc.frontFaceStencil.stencilFail);
        d.DepthStencilState.FrontFace.StencilPassOp = convertStencilOperation(desc.depthAndStencilDesc.frontFaceStencil.pass);
        d.DepthStencilState.FrontFace.StencilFunc = convertCompareFunc(desc.depthAndStencilDesc.frontFaceStencil.func);

        /// Back Face Stencil
        d.DepthStencilState.BackFace.StencilDepthFailOp = convertStencilOperation(desc.depthAndStencilDesc.backFaceStencil.depthFail);
        d.DepthStencilState.BackFace.StencilFailOp = convertStencilOperation(desc.depthAndStencilDesc.backFaceStencil.stencilFail);
        d.DepthStencilState.BackFace.StencilPassOp = convertStencilOperation(desc.depthAndStencilDesc.backFaceStencil.pass);
        d.DepthStencilState.BackFace.StencilFunc = convertCompareFunc(desc.depthAndStencilDesc.backFaceStencil.func);


        d.SampleMask = UINT_MAX;
        d.SampleDesc.Quality = 0;
        d.SampleDesc.Count = 1;

        // MessageBoxA(GetForegroundWindow(),"Create Bytecode Funcs","NOTE",MB_OK);
        ID3D12PipelineState *state;
        hr = d3d12_device->CreateGraphicsPipelineState(&d,IID_PPV_ARGS(&state));
        if(FAILED(hr)){
            // TODO
            // Proper Logging.
            // MessageBoxA(GetForegroundWindow(),"Failed to Create Pipeline State","NOTE",MB_OK);
            exit(1);
        };
        ATL::CStringW wstr(desc.name.data());
        state->SetName(wstr);
        return SharedHandle<GERenderPipelineState>(new GED3D12RenderPipelineState(desc.vertexFunc,desc.fragmentFunc,state,signature,rootSigDesc));
    };
    SharedHandle<GEComputePipelineState> GED3D12Engine::makeComputePipelineState(ComputePipelineDescriptor &desc){
        if(!_checkPipelineShader(desc.computeFunc,"compute",desc.name)){
            return nullptr;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC d {};
        HRESULT hr;
        ID3D12PipelineState *state;

        auto computeShader = (GED3D12Shader *)desc.computeFunc.get();

        d.NodeMask = d3d12_device->GetNodeCount();
        d.CS = computeShader->shaderBytecode;
        d.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        omegasl_shader shaders[] = {desc.computeFunc->internal};

        ID3D12RootSignature *signature;
        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc1;

        auto b = createRootSignatureFromOmegaSLShaders(1,shaders,&rootSignatureDesc1,&signature);
        if(b) {
            d.pRootSignature = signature;
        }
        else {
            DEBUG_STREAM("Failed to Create Root Signature");
            exit(1);
        }

        hr = d3d12_device->CreateComputePipelineState(&d,IID_PPV_ARGS(&state));
        if(FAILED(hr)){
            DEBUG_STREAM("Failed to Create Compute Pipeline State");
            exit(1);
        }
        ATL::CStringW wstr(desc.name.data());
        state->SetName(wstr);
        return SharedHandle<GEComputePipelineState>(new GED3D12ComputePipelineState(desc.computeFunc,state,signature,rootSignatureDesc1));
    };

    SharedHandle<GERenderPipelineState> GED3D12Engine::makeMeshPipelineState(MeshPipelineDescriptor &desc){
        /// Mesh-Shader-Plan Phase 4b.2 — D3D12 mesh PSO build via the
        /// SM 6.5 pipeline-state-stream API. Mirrors the graphics
        /// `makeRenderPipelineState` above for color formats, blend,
        /// depth-stencil, raster, and sample state; the only divergence
        /// is the geometry side (MS/AS replace VS/HS/DS/GS+IA) and the
        /// matching stream-based `ID3D12Device8::CreatePipelineState`
        /// call. Feature-gate first, same idiom as the raytracing
        /// pattern at `createBoundingBoxesBuffer`.
        if(!gteDevice->features.hasFeature(GTEDEVICE_FEATURE_MESH_SHADER)){
            DEBUG_STREAM("makeMeshPipelineState: device does not advertise "
                         "GTEDEVICE_FEATURE_MESH_SHADER ('" << desc.name << "')");
            return nullptr;
        }
        if(!_checkPipelineShader(desc.meshFunc,"mesh",desc.name)){
            return nullptr;
        }
        if(!_checkPipelineShader(desc.fragmentFunc,"fragment",desc.name)){
            return nullptr;
        }
        /// Phase 5 hard-stop. The descriptor exposes `amplificationFunc`
        /// so callers can write forward-compatible code, but the
        /// per-backend AS/task plumbing (payload-type matching, the
        /// dispatch-children builtins) does not land until Phase 5.
        /// Fail loud here rather than silently dropping the stage.
        if(desc.amplificationFunc){
            DEBUG_STREAM("makeMeshPipelineState: amplification stage is Phase 5 "
                         "(payload + dispatch-children machinery pending); "
                         "passing `amplificationFunc` is not supported yet ('"
                         << desc.name << "')");
            return nullptr;
        }

        auto &meshShaderDesc = desc.meshFunc->internal;
        auto &fragmentDesc   = desc.fragmentFunc->internal;
        assert(meshShaderDesc.type == OMEGASL_SHADER_MESH
               && "Mesh slot does not hold a mesh shader");
        assert(fragmentDesc.type == OMEGASL_SHADER_FRAGMENT
               && "Fragment slot does not hold a fragment shader");

        DEBUG_STREAM("Making D3D12 mesh-shader pipeline '" << desc.name << "'");

        /// Root signature — reuse the existing OmegaSL-driven builder
        /// over {mesh, fragment}. Mesh shaders bind their resources
        /// through the same root-parameter table the vertex path uses
        /// today; the per-shader visibility is captured by the
        /// underlying omegasl_shader and the builder fans it out.
        omegasl_shader shaders[] = {meshShaderDesc, fragmentDesc};
        ID3D12RootSignature *signature = nullptr;
        D3D12_ROOT_SIGNATURE_DESC1 rootSigDesc{};
        if(!createRootSignatureFromOmegaSLShaders(2, shaders, &rootSigDesc, &signature)){
            DEBUG_STREAM("makeMeshPipelineState: failed to create root signature ('"
                         << desc.name << "')");
            return nullptr;
        }

        auto *meshShader     = (GED3D12Shader *)desc.meshFunc.get();
        auto *fragmentShader = (GED3D12Shader *)desc.fragmentFunc.get();

        /// PSO subobjects, mirroring the graphics path field-for-field
        /// where the fields apply. Blend / depth-stencil / raster /
        /// sample / RT-format conversions are identical to
        /// `makeRenderPipelineState` above — same helper functions, same
        /// descriptor sub-struct (`RenderPipelineDescriptor::DepthStencilDesc`
        /// is reused on `MeshPipelineDescriptor` by design).
        CD3DX12_PIPELINE_MESH_STATE_STREAM stream{};
        stream.Flags          = D3D12_PIPELINE_STATE_FLAG_NONE;
        stream.NodeMask       = d3d12_device->GetNodeCount();
        stream.pRootSignature = signature;
        stream.MS             = meshShader->shaderBytecode;
        stream.PS             = fragmentShader->shaderBytecode;

        // ── Blend ───────────────────────────────────────────────
        {
            CD3DX12_BLEND_DESC blendDesc(D3D12_DEFAULT);
            const auto &blendDescs = desc.colorBlendDescriptors;
            blendDesc.IndependentBlendEnable = blendDescs.size() > 1 ? TRUE : FALSE;
            for(unsigned i = 0; i < 8; ++i){
                auto &rt = blendDesc.RenderTarget[i];
                rt.LogicOpEnable = FALSE;
                if(i < blendDescs.size()){
                    const auto &b = blendDescs[i];
                    rt.BlendEnable           = b.blendEnabled ? TRUE : FALSE;
                    rt.SrcBlend              = convertBlendFactor(b.srcColorFactor);
                    rt.DestBlend             = convertBlendFactor(b.destColorFactor);
                    rt.BlendOp               = convertBlendOperation(b.colorOp);
                    rt.SrcBlendAlpha         = convertBlendFactor(b.srcAlphaFactor);
                    rt.DestBlendAlpha        = convertBlendFactor(b.destAlphaFactor);
                    rt.BlendOpAlpha          = convertBlendOperation(b.alphaOp);
                    rt.RenderTargetWriteMask = convertColorWriteMask(b.writeMask);
                } else {
                    rt.BlendEnable           = FALSE;
                    rt.SrcBlend              = D3D12_BLEND_ONE;
                    rt.DestBlend             = D3D12_BLEND_ZERO;
                    rt.BlendOp               = D3D12_BLEND_OP_ADD;
                    rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
                    rt.DestBlendAlpha        = D3D12_BLEND_ZERO;
                    rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
                    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                }
            }
            stream.BlendState = blendDesc;
        }

        // ── Rasterizer ──────────────────────────────────────────
        {
            CD3DX12_RASTERIZER_DESC rast(D3D12_DEFAULT);
            switch(desc.cullMode){
                case RasterCullMode::None:  rast.CullMode = D3D12_CULL_MODE_NONE;  break;
                case RasterCullMode::Front: rast.CullMode = D3D12_CULL_MODE_FRONT; break;
                case RasterCullMode::Back:  rast.CullMode = D3D12_CULL_MODE_BACK;  break;
            }
            switch(desc.triangleFillMode){
                case TriangleFillMode::Wireframe: rast.FillMode = D3D12_FILL_MODE_WIREFRAME; break;
                case TriangleFillMode::Solid:     rast.FillMode = D3D12_FILL_MODE_SOLID;     break;
            }
            rast.ForcedSampleCount       = desc.rasterSampleCount;
            rast.FrontCounterClockwise   = (desc.polygonFrontFaceRotation
                                            == GTEPolygonFrontFaceRotation::CounterClockwise) ? TRUE : FALSE;
            rast.DepthBias               = (INT)desc.depthAndStencilDesc.depthBias;
            rast.SlopeScaledDepthBias    = desc.depthAndStencilDesc.slopeScale;
            rast.DepthBiasClamp          = desc.depthAndStencilDesc.depthClamp;
            stream.RasterizerState = rast;
        }

        // ── Depth / stencil ─────────────────────────────────────
        {
            CD3DX12_DEPTH_STENCIL_DESC1 dsDesc(D3D12_DEFAULT);
            dsDesc.DepthEnable    = desc.depthAndStencilDesc.enableDepth;
            dsDesc.StencilEnable  = desc.depthAndStencilDesc.enableStencil;
            dsDesc.DepthFunc      = convertCompareFunc(desc.depthAndStencilDesc.depthOperation);
            dsDesc.DepthWriteMask =
                (desc.depthAndStencilDesc.writeAmount == DepthWriteAmount::All)
                    ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            dsDesc.StencilReadMask  = desc.depthAndStencilDesc.stencilReadMask;
            dsDesc.StencilWriteMask = desc.depthAndStencilDesc.stencilWriteMask;
            dsDesc.FrontFace.StencilDepthFailOp = convertStencilOperation(desc.depthAndStencilDesc.frontFaceStencil.depthFail);
            dsDesc.FrontFace.StencilFailOp      = convertStencilOperation(desc.depthAndStencilDesc.frontFaceStencil.stencilFail);
            dsDesc.FrontFace.StencilPassOp      = convertStencilOperation(desc.depthAndStencilDesc.frontFaceStencil.pass);
            dsDesc.FrontFace.StencilFunc        = convertCompareFunc(desc.depthAndStencilDesc.frontFaceStencil.func);
            dsDesc.BackFace.StencilDepthFailOp  = convertStencilOperation(desc.depthAndStencilDesc.backFaceStencil.depthFail);
            dsDesc.BackFace.StencilFailOp       = convertStencilOperation(desc.depthAndStencilDesc.backFaceStencil.stencilFail);
            dsDesc.BackFace.StencilPassOp       = convertStencilOperation(desc.depthAndStencilDesc.backFaceStencil.pass);
            dsDesc.BackFace.StencilFunc         = convertCompareFunc(desc.depthAndStencilDesc.backFaceStencil.func);
            stream.DepthStencilState = dsDesc;
        }

        // ── Color attachment formats ────────────────────────────
        {
            D3D12_RT_FORMAT_ARRAY rts{};
            const auto &formats = desc.colorPixelFormats;
            const unsigned rtCount = formats.empty() ? 1u
                                     : (unsigned)std::min<size_t>(formats.size(), 8);
            for(unsigned i = 0; i < rtCount; ++i){
                rts.RTFormats[i] = pixelFormatToDxgiFormat(
                    formats.empty() ? PixelFormat::RGBA8Unorm : formats[i]);
            }
            for(unsigned i = rtCount; i < 8; ++i){
                rts.RTFormats[i] = DXGI_FORMAT_UNKNOWN;
            }
            rts.NumRenderTargets = rtCount;
            stream.RTVFormats = rts;
        }

        // ── Sample state ────────────────────────────────────────
        DXGI_SAMPLE_DESC sampleDesc{};
        sampleDesc.Count   = 1;
        sampleDesc.Quality = 0;
        stream.SampleDesc = sampleDesc;
        stream.SampleMask = UINT_MAX;

        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{};
        streamDesc.SizeInBytes                   = sizeof(stream);
        streamDesc.pPipelineStateSubobjectStream = &stream;

        ID3D12PipelineState *state = nullptr;
        HRESULT hr = d3d12_device->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&state));
        if(FAILED(hr)){
            DEBUG_STREAM("makeMeshPipelineState: CreatePipelineState failed hr=0x"
                         << std::hex << (unsigned)hr << std::dec
                         << " ('" << desc.name << "')");
            if(signature){ signature->Release(); }
            return nullptr;
        }
        ATL::CStringW wstr(desc.name.data());
        state->SetName(wstr);
        return SharedHandle<GERenderPipelineState>(
            new GED3D12RenderPipelineState(desc.meshFunc, desc.fragmentFunc,
                                           state, signature, rootSigDesc,
                                           /*meshVariant=*/true));
    }

    SharedHandle<GEBlitPipelineState> GED3D12Engine::makeBlitPipelineState(BlitPipelineDescriptor &desc) {
        if (!_checkPipelineShader(desc.fragmentFunc, "fragment", desc.name)) {
            return nullptr;
        }
        if (!ensureBlitFullscreenVs()) {
            DEBUG_STREAM("makeBlitPipelineState: ensureBlitFullscreenVs failed");
            return nullptr;
        }

        RenderPipelineDescriptor rpDesc{};
        rpDesc.name = desc.name.empty() ? OmegaCommon::String("OmegaGTE.Internal.BlitPipeline") : desc.name;
        rpDesc.vertexFunc = blitFullscreenVs;
        rpDesc.fragmentFunc = desc.fragmentFunc;
        rpDesc.colorPixelFormats = { desc.destPixelFormat };
        rpDesc.primitiveTopologyCategory = PrimitiveTopologyCategory::Triangle;
        rpDesc.rasterSampleCount = 1;
        rpDesc.cullMode = RasterCullMode::None;
        rpDesc.triangleFillMode = TriangleFillMode::Solid;
        // No blend, no depth, no stencil — defaults already produce that.

        auto rp = makeRenderPipelineState(rpDesc);
        if (!rp) {
            DEBUG_STREAM("makeBlitPipelineState: underlying makeRenderPipelineState failed");
            return nullptr;
        }
        return SharedHandle<GEBlitPipelineState>(new GED3D12BlitPipelineState(rp));
    };

    SharedHandle<GENativeRenderTarget> GED3D12Engine::makeNativeRenderTarget(const NativeRenderTargetDescriptor &desc,
                                                                              SharedHandle<GECommandQueue> presentQueue){
        // Gate the requested color format against the portable
        // swap-chain/drawable intersection. Anything else is rejected at
        // the API boundary rather than silently substituted, so the caller
        // sees the misconfiguration instead of an unexpected color buffer.
        if(!isPortableNativeRenderTargetFormat(desc.pixelFormat)){
            DEBUG_STREAM("makeNativeRenderTarget: requested pixelFormat is not in the portable swap-chain set; rejecting.");
            return nullptr;
        }

        if(presentQueue == nullptr){
            DEBUG_STREAM("makeNativeRenderTarget: null presentQueue");
            return nullptr;
        }

        HRESULT hr;
        constexpr UINT kBackBufferCount = 2;

        // FLIP-model swap chains require a non-typeless, non-SRGB storage
        // format on the buffer itself. For SRGB color, store as UNORM and
        // expose the sRGB view via the RTV format.
        const DXGI_FORMAT rtvDxgiFormat = pixelFormatToDxgiFormat(desc.pixelFormat);
        DXGI_FORMAT bufferDxgiFormat = rtvDxgiFormat;
        if(rtvDxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB){
            bufferDxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        else if(rtvDxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB){
            // Defensive: the portable gate above already excludes RGBA8
            // variants, but keep the mapping correct if the gate widens.
            bufferDxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        }

        auto rtv_desc_size = d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        // NodeMask is a bitmask of GPU nodes (0 == single-GPU default), not
        // a count.
        heap_desc.NodeMask = 0;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = kBackBufferCount;
        ComPtr<ID3D12DescriptorHeap> renderTargetHeap;
        hr = d3d12_device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&renderTargetHeap));
        if(FAILED(hr)){
            DEBUG_STREAM("Failed to create RTV descriptor heap hr=0x"
                         << std::hex << static_cast<unsigned long>(hr) << std::dec);
            return nullptr;
        }

        ComPtr<ID3D12DescriptorHeap> dsvDescHeap;
        if(desc.allowDepthStencilTesting){
            D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
            dsv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            dsv_heap_desc.NodeMask = 0;
            dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsv_heap_desc.NumDescriptors = kBackBufferCount;
            hr = d3d12_device->CreateDescriptorHeap(&dsv_heap_desc,IID_PPV_ARGS(&dsvDescHeap));
            if(FAILED(hr)){
                DEBUG_STREAM("Failed to create DSV descriptor heap hr=0x"
                             << std::hex << static_cast<unsigned long>(hr) << std::dec);
                return nullptr;
            }
            // TODO: allocate companion depth-stencil resources sized to the
            // swap chain and populate this heap with real DSVs (the old
            // implementation created DSVs that aliased the color back
            // buffer, which is invalid). Until that lands, the heap exists
            // so downstream code that already indexes into it does not
            // crash, but the descriptors themselves are null. Callers that
            // actually need depth on a native RT should fall back to a
            // texture render target.
        }

        RECT rc{};
        if(desc.isHwnd){
            if(desc.hwnd == nullptr || !GetClientRect(desc.hwnd, &rc)){
                DEBUG_STREAM("makeNativeRenderTarget: GetClientRect failed for hwnd=" << desc.hwnd);
                return nullptr;
            }
        }
        else {
            rc = RECT{0,0,(LONG)desc.width,(LONG)desc.height};
        }
        const LONG widthLong = rc.right - rc.left;
        const LONG heightLong = rc.bottom - rc.top;
        if(widthLong <= 0 || heightLong <= 0){
            DEBUG_STREAM("makeNativeRenderTarget: non-positive client rect "
                         << widthLong << "x" << heightLong);
            return nullptr;
        }

        DXGI_SWAP_CHAIN_DESC1 swapChaindesc{};
        swapChaindesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChaindesc.Format = bufferDxgiFormat;
        swapChaindesc.BufferCount = kBackBufferCount;
        swapChaindesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChaindesc.Width = static_cast<UINT>(widthLong);
        swapChaindesc.Height = static_cast<UINT>(heightLong);
        swapChaindesc.SampleDesc.Count = 1;
        swapChaindesc.SampleDesc.Quality = 0;
        swapChaindesc.AlphaMode = desc.isHwnd ? DXGI_ALPHA_MODE_IGNORE
                                              : DXGI_ALPHA_MODE_PREMULTIPLIED;
        swapChaindesc.Scaling = DXGI_SCALING_STRETCH;

        IDXGISwapChain3 *rawSwapChain = nullptr;
        if(desc.isHwnd){
            rawSwapChain = createSwapChainFromHWND(desc.hwnd, &swapChaindesc, presentQueue);
        }
        else {
            rawSwapChain = createSwapChainForComposition(&swapChaindesc, presentQueue);
        }
        if(rawSwapChain == nullptr){
            DEBUG_STREAM("makeNativeRenderTarget: swap chain creation returned null");
            return nullptr;
        }
        // Adopt ownership in a ComPtr so any failure below releases it.
        ComPtr<IDXGISwapChain3> swapChain;
        swapChain.Attach(rawSwapChain);

        std::vector<ID3D12Resource *> rtvs;
        rtvs.reserve(kBackBufferCount);
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_cpu_handle(renderTargetHeap->GetCPUDescriptorHandleForHeapStart());

        D3D12_RENDER_TARGET_VIEW_DESC rtvViewDesc{};
        rtvViewDesc.Format = rtvDxgiFormat;
        rtvViewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvViewDesc.Texture2D.MipSlice = 0;
        rtvViewDesc.Texture2D.PlaneSlice = 0;
        const bool needsExplicitRtvDesc = bufferDxgiFormat != rtvDxgiFormat;

        for(unsigned i = 0;i < kBackBufferCount;i++){
            ID3D12Resource *backBuffer = nullptr;
            hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer));
            if(FAILED(hr) || backBuffer == nullptr){
                DEBUG_STREAM("makeNativeRenderTarget: GetBuffer(" << i
                             << ") failed hr=0x"
                             << std::hex << static_cast<unsigned long>(hr) << std::dec);
                for(auto *r : rtvs) if(r) r->Release();
                return nullptr;
            }
            d3d12_device->CreateRenderTargetView(backBuffer,
                                                 needsExplicitRtvDesc ? &rtvViewDesc : nullptr,
                                                 rtv_cpu_handle);
            rtv_cpu_handle.Offset(1, rtv_desc_size);
            rtvs.push_back(backBuffer);
        }

        // Hand the COM objects to the render target via `.Get()`. The
        // constructor's ComPtr members AddRef internally; our local
        // ComPtrs Release on scope exit, leaving refcount 1 owned by the
        // render target. The raw `ID3D12Resource*` back-buffer pointers
        // are transferred at their existing +1 refcount and Released by
        // `~GED3D12NativeRenderTarget` / `resizeSwapChain`.
        return SharedHandle<GENativeRenderTarget>(new GED3D12NativeRenderTarget(
            swapChain.Get(),
            renderTargetHeap.Get(),
            dsvDescHeap.Get(),
            std::move(presentQueue),
            0,
            rtvs.data(),
            rtvs.size(),
            desc.isHwnd ? desc.hwnd : nullptr,
            desc.pixelFormat,
            bufferDxgiFormat,
            rtvDxgiFormat));
    };

    SharedHandle<GETextureRenderTarget> GED3D12Engine::makeTextureRenderTarget(const TextureRenderTargetDescriptor &desc){
        SharedHandle<GETexture> texture;
        if(desc.renderToExistingTexture){
            texture = desc.texture;
        }
        else {
            TextureDescriptor targetDesc {};
            targetDesc.usage = GETexture::RenderTarget;
            targetDesc.kind = TextureKind::Tex2D;
            targetDesc.width = desc.region.w;
            targetDesc.height = desc.region.h;
            texture = makeTexture(targetDesc);
        }
        return SharedHandle<GETextureRenderTarget>(new GED3D12TextureRenderTarget(std::dynamic_pointer_cast<GED3D12Texture>(texture)));
    };

    SharedHandle<GECommandQueue> GED3D12Engine::makeCommandQueue(unsigned int maxBufferCount){
        SharedHandle<GECommandQueue> q(new GED3D12CommandQueue(this,maxBufferCount));
        liveCommandQueues.push_back(q);
        return q;
    };

    SharedHandle<GETexture> GED3D12Engine::makeTexture(const TextureDescriptor &desc){
         DEBUG_STREAM("Making D3D12Texture");
        HRESULT hr;
        D3D12_RESOURCE_DESC d3d12_desc {};
        D3D12_RESOURCE_STATES res_states = D3D12_RESOURCE_STATE_COMMON;

        D3D12_SHADER_RESOURCE_VIEW_DESC res_view_desc {};
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_view_desc {};
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_view_desc {};

        bool isUAV = bool(desc.usage == GETexture::FromGPU || desc.usage == GETexture::GPUAccessOnly || desc.usage == GETexture::RenderTarget);
        bool isSRV = bool(desc.usage == GETexture::ToGPU || desc.usage == GETexture::GPUAccessOnly || desc.usage == GETexture::RenderTarget);
        bool isDSV =  bool(desc.usage == GETexture::RenderTargetAndDepthStencil || desc.usage == GETexture::GPUAccessOnly);

        if(desc.usage == GETexture::RenderTargetAndDepthStencil && desc.kind == TextureKind::Tex3D){
            DEBUG_STREAM("Cannot create a 3D Texture with Depth Stencil Properties");
            return nullptr;
        }

        DXGI_FORMAT dxgiFormat = pixelFormatToDxgiFormat(desc.pixelFormat);

        if(desc.usage == GETexture::RenderTarget){
            res_states |= D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        else if(desc.usage == GETexture::RenderTargetAndDepthStencil){
            res_states |= D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }
        else if(desc.usage == GETexture::ToGPU){
            res_states |= D3D12_RESOURCE_STATE_GENERIC_READ;
        }   
        else if(desc.usage == GETexture::FromGPU){
            res_states |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        else {
            res_states = D3D12_RESOURCE_STATE_COMMON;
        };

        if(isUAV){
            uav_view_desc.Format = dxgiFormat;
        }

        if(isSRV){
            // Bake `desc.defaultSwizzle` into the primary SRV's
            // Shader4ComponentMapping so every bind without a runtime
            // override gets the swizzle for free. Texture-swizzle proposal
            // §4 / Open Q1.
            auto encodeSwizzle = [](TextureSwizzleChannel ch, unsigned positional) -> unsigned {
                switch(ch){
                    case TextureSwizzleChannel::Identity: return positional;
                    case TextureSwizzleChannel::Red:      return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
                    case TextureSwizzleChannel::Green:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
                    case TextureSwizzleChannel::Blue:     return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
                    case TextureSwizzleChannel::Alpha:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
                    case TextureSwizzleChannel::Zero:     return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
                    case TextureSwizzleChannel::One:      return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
                }
                return positional;
            };
            res_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                encodeSwizzle(desc.defaultSwizzle.r, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0),
                encodeSwizzle(desc.defaultSwizzle.g, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1),
                encodeSwizzle(desc.defaultSwizzle.b, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2),
                encodeSwizzle(desc.defaultSwizzle.a, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3));
            res_view_desc.Format = dxgiFormat;
        }

        if(isDSV){
            dsv_view_desc.Format = dxgiFormat;
            dsv_view_desc.Flags = D3D12_DSV_FLAG_NONE;
        }

        D3D12_RENDER_TARGET_VIEW_DESC view_desc {};

        // Pipeline-Completion-Extension-Plan §6.2 — drive native resource
        // shape and SRV/UAV/RTV/DSV view dimension from the descriptor's
        // TextureKind. `Auto` is treated as `Tex2D` for back-compat with
        // descriptors that never set kind explicitly.
        const TextureKind kind = desc.kind == TextureKind::Auto ? TextureKind::Tex2D : desc.kind;
        const unsigned arrayLayers = desc.arrayLayers > 0 ? desc.arrayLayers : 1;
        const bool isMS = (kind == TextureKind::Tex2DMS || kind == TextureKind::Tex2DMSArray);
        const unsigned effectiveSampleCount = isMS ? (desc.sampleCount > 1 ? desc.sampleCount : 1u)
                                                   : 1u;
        // MS textures are single-mip on every backend (D3D12 / Vulkan / Metal).
        const unsigned effectiveMips = isMS ? 1u : desc.mipLevels;

        switch(kind){
        case TextureKind::Tex1D: {
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex1D(dxgiFormat,desc.width,1,effectiveMips);
            if(isUAV) {
                uav_view_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                uav_view_desc.Texture1D.MipSlice = effectiveMips;
            }
            if(isSRV) {
                res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                res_view_desc.Texture1D.MipLevels = effectiveMips;
            }
            if(isDSV){
                dsv_view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
                dsv_view_desc.Texture1D.MipSlice = effectiveMips;
            }
            break;
        }
        case TextureKind::Tex1DArray: {
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex1D(dxgiFormat,desc.width,arrayLayers,effectiveMips);
            if(isUAV){
                uav_view_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                uav_view_desc.Texture1DArray.MipSlice = effectiveMips - 1;
                uav_view_desc.Texture1DArray.FirstArraySlice = 0;
                uav_view_desc.Texture1DArray.ArraySize = arrayLayers;
            }
            if(isSRV){
                res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                res_view_desc.Texture1DArray.MipLevels = effectiveMips;
                res_view_desc.Texture1DArray.MostDetailedMip = 0;
                res_view_desc.Texture1DArray.FirstArraySlice = 0;
                res_view_desc.Texture1DArray.ArraySize = arrayLayers;
            }
            break;
        }
        case TextureKind::Tex2D: {
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,1,effectiveMips,1);
            if(isUAV){
                uav_view_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav_view_desc.Texture2D.MipSlice = effectiveMips - 1;
                uav_view_desc.Texture2D.PlaneSlice = 0;
            }
            if(isSRV) {
                res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                res_view_desc.Texture2D.MipLevels = effectiveMips;
                // MostDetailedMip is the highest-resolution mip the
                // view starts at — must be 0 to address mip 0 (the
                // full-res image). Previously set to `effectiveMips
                // - 1`, which together with `MipLevels = effectiveMips`
                // asks the view to span mips [N-1 .. 2N-2] against a
                // texture that has [0 .. N-1]. For N == 1 the bug was
                // invisible (1-1=0); it surfaced as soon as any caller
                // requested mips — e.g. BitmapTextureCache for images
                // >= 64x64 — with D3D12 debug-layer error
                // CREATESHADERRESOURCEVIEW_INVALIDDIMENSIONS and
                // immediate device removal.
                // ResourceMinLODClamp likewise must be 0.f; clamping
                // to the smallest mip would force every sampler to
                // read the 1x1 mip regardless of derivatives.
                // Every other view dimension in this file already
                // uses MostDetailedMip = 0 (Tex1DArray, Tex2DArray,
                // TextureCube, TextureCubeArray).
                res_view_desc.Texture2D.MostDetailedMip = 0;
                res_view_desc.Texture2D.PlaneSlice = 0;
                res_view_desc.Texture2D.ResourceMinLODClamp = 0.f;
            }
            if(isDSV){
                dsv_view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsv_view_desc.Texture2D.MipSlice = effectiveMips - 1;
            }
            if(desc.usage & GETexture::RenderTarget){
                view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                view_desc.Format = dxgiFormat;
                view_desc.Texture2D.PlaneSlice = 0;
                view_desc.Texture2D.MipSlice = 0;
            }
            break;
        }
        case TextureKind::Tex2DArray: {
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,arrayLayers,effectiveMips,1);
            if(isUAV){
                uav_view_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uav_view_desc.Texture2DArray.MipSlice = effectiveMips - 1;
                uav_view_desc.Texture2DArray.FirstArraySlice = 0;
                uav_view_desc.Texture2DArray.ArraySize = arrayLayers;
                uav_view_desc.Texture2DArray.PlaneSlice = 0;
            }
            if(isSRV){
                res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                res_view_desc.Texture2DArray.MipLevels = effectiveMips;
                res_view_desc.Texture2DArray.MostDetailedMip = 0;
                res_view_desc.Texture2DArray.FirstArraySlice = 0;
                res_view_desc.Texture2DArray.ArraySize = arrayLayers;
                res_view_desc.Texture2DArray.PlaneSlice = 0;
                res_view_desc.Texture2DArray.ResourceMinLODClamp = 0;
            }
            if(isDSV){
                dsv_view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsv_view_desc.Texture2DArray.MipSlice = effectiveMips - 1;
                dsv_view_desc.Texture2DArray.FirstArraySlice = 0;
                dsv_view_desc.Texture2DArray.ArraySize = arrayLayers;
            }
            if(desc.usage & GETexture::RenderTarget){
                view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                view_desc.Format = dxgiFormat;
                view_desc.Texture2DArray.MipSlice = 0;
                view_desc.Texture2DArray.FirstArraySlice = 0;
                view_desc.Texture2DArray.ArraySize = arrayLayers;
                view_desc.Texture2DArray.PlaneSlice = 0;
            }
            break;
        }
        case TextureKind::TexCube: {
            // D3D12 represents cubes as 2D arrays with array_size = 6.
            // The cube-ness is purely a property of the SRV view dimension.
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,6,effectiveMips,1);
            if(isSRV){
                res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                res_view_desc.TextureCube.MipLevels = effectiveMips;
                res_view_desc.TextureCube.MostDetailedMip = 0;
                res_view_desc.TextureCube.ResourceMinLODClamp = 0;
            }
            // Cube UAV writes alias to RWTexture2DArray; OmegaSL §2.1 Sema
            // rejects cube `write`, so this path is unreachable from a
            // generated shader. Pre-fill the array form anyway in case a
            // future call site asks for it.
            if(isUAV){
                uav_view_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uav_view_desc.Texture2DArray.MipSlice = effectiveMips - 1;
                uav_view_desc.Texture2DArray.FirstArraySlice = 0;
                uav_view_desc.Texture2DArray.ArraySize = 6;
                uav_view_desc.Texture2DArray.PlaneSlice = 0;
            }
            break;
        }
        case TextureKind::TexCubeArray: {
            const unsigned cubeArrayLayers = arrayLayers >= 6 ? arrayLayers : 6;
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,cubeArrayLayers,effectiveMips,1);
            if(isSRV){
                res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                res_view_desc.TextureCubeArray.MipLevels = effectiveMips;
                res_view_desc.TextureCubeArray.MostDetailedMip = 0;
                res_view_desc.TextureCubeArray.First2DArrayFace = 0;
                res_view_desc.TextureCubeArray.NumCubes = cubeArrayLayers / 6;
                res_view_desc.TextureCubeArray.ResourceMinLODClamp = 0;
            }
            if(isUAV){
                uav_view_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uav_view_desc.Texture2DArray.MipSlice = effectiveMips - 1;
                uav_view_desc.Texture2DArray.FirstArraySlice = 0;
                uav_view_desc.Texture2DArray.ArraySize = cubeArrayLayers;
                uav_view_desc.Texture2DArray.PlaneSlice = 0;
            }
            break;
        }
        case TextureKind::Tex2DMS: {
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,1,1,effectiveSampleCount);
            if(isSRV){
                res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                // Texture2DMS has no mip / clamp fields to populate.
            }
            if(isDSV){
                dsv_view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
            }
            if(desc.usage & GETexture::RenderTarget){
                view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
                view_desc.Format = dxgiFormat;
            }
            // MS UAVs require Tier-3 HW; skip until a use case lands.
            break;
        }
        case TextureKind::Tex2DMSArray: {
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,arrayLayers,1,effectiveSampleCount);
            if(isSRV){
                res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                res_view_desc.Texture2DMSArray.FirstArraySlice = 0;
                res_view_desc.Texture2DMSArray.ArraySize = arrayLayers;
            }
            if(isDSV){
                dsv_view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                dsv_view_desc.Texture2DMSArray.FirstArraySlice = 0;
                dsv_view_desc.Texture2DMSArray.ArraySize = arrayLayers;
            }
            if(desc.usage & GETexture::RenderTarget){
                view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                view_desc.Format = dxgiFormat;
                view_desc.Texture2DMSArray.FirstArraySlice = 0;
                view_desc.Texture2DMSArray.ArraySize = arrayLayers;
            }
            break;
        }
        case TextureKind::Tex3D: {
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex3D(dxgiFormat,desc.width,desc.height,desc.depth,effectiveMips);
            d3d12_desc.SampleDesc.Count = 1;
            if(isUAV){
                uav_view_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                uav_view_desc.Texture3D.MipSlice = effectiveMips - 1;
                uav_view_desc.Texture3D.FirstWSlice = 0;
                uav_view_desc.Texture3D.WSize = desc.depth;
            }
            if(isSRV){
                res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                res_view_desc.Texture3D.MipLevels = effectiveMips;
            }
            if(desc.usage == GETexture::RenderTarget){
                view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                view_desc.Texture3D.MipSlice = 0;
                view_desc.Texture3D.FirstWSlice = 0;
                view_desc.Texture3D.WSize = desc.depth;
            }
            break;
        }
        case TextureKind::Auto:
            // The Auto-→-Tex2D collapse above guarantees this branch is
            // dead; keep the case for switch exhaustiveness with the legacy
            // 2D placement as a safety net.
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,1,effectiveMips,1);
            break;
        }
        CD3DX12_HEAP_PROPERTIES heap_prop;


        if(desc.usage == GETexture::FromGPU) {
            heap_prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
            d3d12_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        else if(desc.usage == GETexture::MSResolveSrc || desc.usage == GETexture::GPUAccessOnly){
            heap_prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            d3d12_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        }
        else if(desc.usage == GETexture::ToGPU) {
            heap_prop = CD3DX12_HEAP_PROPERTIES( D3D12_HEAP_TYPE_UPLOAD );
            // Grant UAV access when the descriptor asks for multiple
            // mips. The compute-shader mipmap generator
            // (GED3D12CommandBuffer::generateMipmaps) needs to bind
            // each destination mip as a UAV; without this flag the
            // generator's UAV-capability check fails and mips 1..N-1
            // are left at upload-heap zeros, producing undefined
            // samples on minification. Single-mip textures keep the
            // narrower flag set to preserve D3D12's optimization
            // hints for read-only resources. The texture *resource*
            // is on D3D12_HEAP_TYPE_DEFAULT (line ~1992 below), so
            // UAV is legal here; `heap_prop` above describes the
            // separate UPLOAD-heap staging buffer (`cpuSideRes`),
            // which does not need or get a UAV flag.
            d3d12_desc.Flags = (desc.mipLevels > 1)
                ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                : D3D12_RESOURCE_FLAG_NONE;
        }
        else if(desc.usage == GETexture::RenderTarget){
            heap_prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            
            d3d12_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }

        /// Create Texture Resource

        auto textureHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

        D3D12_RESOURCE_STATES states = res_states;


        ID3D12Resource *texture,*cpuSideRes = nullptr;
        D3D12MA::Allocation *texAllocation = nullptr;
        D3D12MA::Allocation *cpuSideAllocation = nullptr;

        D3D12MA::ALLOCATION_DESC texAllocDesc = {};
        texAllocDesc.HeapType       = D3D12_HEAP_TYPE_DEFAULT;
        texAllocDesc.ExtraHeapFlags = D3D12_HEAP_FLAG_SHARED;
        hr = memAllocator->CreateResource(
            &texAllocDesc,
            &d3d12_desc,
            states,
            nullptr,
            &texAllocation,
            IID_PPV_ARGS(&texture));
        if(FAILED(hr)){
            DEBUG_STREAM("Failed to make D3D12 Texture via D3D12MA");
            if (texAllocation) texAllocation->Release();
            return nullptr;
        };


        /// Create GPU/CPU Transition Heap
        if(desc.usage != GETexture::GPUAccessOnly){
            // §7.1/§7.8: size the upload/readback companion for every
            // subresource so copyBytes/getBytes can address an arbitrary
            // (mipLevel, arrayLayer). Only ToGPU/FromGPU heaps are actually
            // CPU-mapped; the others keep the single-subresource size so
            // their (DEFAULT-heap) companion is untouched. planeCount is 1
            // for every pixel format we support today (all color).
            UINT numSubresources = 1;
            if(desc.usage == GETexture::ToGPU || desc.usage == GETexture::FromGPU){
                const auto texDesc = texture->GetDesc();
                const UINT arraySz = (texDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                                     ? 1u : texDesc.DepthOrArraySize;
                numSubresources = UINT(texDesc.MipLevels) * arraySz;
            }
            auto size = GetRequiredIntermediateSize(texture,0,numSubresources);
            auto res = CD3DX12_RESOURCE_DESC::Buffer(size);
            D3D12MA::ALLOCATION_DESC cpuAllocDesc = {};
            cpuAllocDesc.HeapType = heap_prop.Type; // UPLOAD or READBACK matching desc.usage
            hr = memAllocator->CreateResource(
                &cpuAllocDesc,
                &res,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                &cpuSideAllocation,
                IID_PPV_ARGS(&cpuSideRes));
            if(FAILED(hr)){
                DEBUG_STREAM("Failed to make D3D12 Texture transition heap via D3D12MA");
                if (cpuSideAllocation) cpuSideAllocation->Release();
                cpuSideAllocation = nullptr;
                cpuSideRes        = nullptr;
            };
        }



        ID3D12DescriptorHeap *descHeap = nullptr, *rtvDescHeap = nullptr;

        D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc {};
        descHeapDesc.NumDescriptors = 1;
        descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        descHeapDesc.NodeMask = d3d12_device->GetNodeCount();

         ID3D12DescriptorHeap *uavDescHeap = nullptr;

        if(isSRV || isUAV) {
            descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

            hr = d3d12_device->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&descHeap));
            if (FAILED(hr)) {
                DEBUG_STREAM("Failed to Create Descriptor Heap");
                exit(1);
            };
            // auto increment = d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            CD3DX12_CPU_DESCRIPTOR_HANDLE handle(descHeap->GetCPUDescriptorHandleForHeapStart());

            if(isSRV){
                d3d12_device->CreateShaderResourceView(texture, &res_view_desc,
                                                       handle);
            }

            if(isUAV) {
                hr = d3d12_device->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&uavDescHeap));
                if (FAILED(hr)) {
                    DEBUG_STREAM("Failed to Create Descriptor Heap");
                    exit(1);
                };

                CD3DX12_CPU_DESCRIPTOR_HANDLE uav_handle(uavDescHeap->GetCPUDescriptorHandleForHeapStart());

                d3d12_device->CreateUnorderedAccessView(texture,nullptr, &uav_view_desc,
                                                        uav_handle);
            }

        }

        if(desc.usage == GETexture::RenderTarget || desc.usage == GETexture::RenderTargetAndDepthStencil){
            descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr = d3d12_device->CreateDescriptorHeap(&descHeapDesc,IID_PPV_ARGS(&rtvDescHeap));
            if(FAILED(hr)){
                DEBUG_STREAM("Failed to Create RTV Desc Heap");
            };

            d3d12_device->CreateRenderTargetView(texture,&view_desc,rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
            descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        };

        ID3D12DescriptorHeap *dsvDescHeap = nullptr;

        // 3D textures cannot be depth-stencil targets on D3D12; the
        // §6.1 validation rule already rejects DepthStencil + Tex3D, so
        // mirror that here on the resolved kind.
        if(isDSV && kind != TextureKind::Tex3D){
            descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

            hr = d3d12_device->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&dsvDescHeap));
            if(FAILED(hr)){
                DEBUG_STREAM("Failed to Create DSV Desc Heap");
            }

            d3d12_device->CreateDepthStencilView(texture,&dsv_view_desc,dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
        }

        DEBUG_STREAM("Will Return Texture");

        auto result = SharedHandle<GETexture>(new GED3D12Texture(kind,desc.usage,desc.pixelFormat,texture,cpuSideRes,descHeap,uavDescHeap,rtvDescHeap,dsvDescHeap,res_states,texAllocation,cpuSideAllocation));
        // §6.1 — record the resolved shape on the texture so bind-time
        // validation (§6.3) and any future per-kind queries can read
        // it without having to re-derive from `type` + `sampleCount`.
        result->setShape(kind, arrayLayers, effectiveSampleCount);
        if(isSRV){
            // Capture the base SRV desc so the swizzled-SRV cache
            // (texture-swizzle proposal §5) can clone every field except
            // Shader4ComponentMapping.
            auto *d3d12Tex = static_cast<GED3D12Texture *>(result.get());
            d3d12Tex->primarySrvDesc = res_view_desc;
            d3d12Tex->hasPrimarySrvDesc = true;
        }
        return result;
    };

    SharedHandle<GEBuffer> GED3D12Engine::makeBuffer(const BufferDescriptor &desc){
        DEBUG_STREAM("Making D3D12Buffer");
        HRESULT hr;
        ID3D12Resource *buffer;
        D3D12_HEAP_TYPE heap_type;
        D3D12_RESOURCE_FLAGS flags;
        D3D12_RESOURCE_STATES state;

        switch (desc.usage) {
            case BufferDescriptor::Upload : {
                heap_type = D3D12_HEAP_TYPE_UPLOAD;
                flags = D3D12_RESOURCE_FLAG_NONE;
                state = D3D12_RESOURCE_STATE_GENERIC_READ;
                break;
            }
            case BufferDescriptor::Readback : {
                heap_type = D3D12_HEAP_TYPE_READBACK;
                flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                state = D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                break;
            }
            case BufferDescriptor::GPUOnly : {
                heap_type = D3D12_HEAP_TYPE_DEFAULT;
                flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                state = D3D12_RESOURCE_STATE_COPY_DEST;
            }
        }
        // §2.4 — a constant buffer bound via a root CBV must be 256-byte
        // sized/aligned (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT).
        // D3D12MA places buffers at >=256-aligned addresses, so rounding the
        // size up is sufficient for SetGraphicsRootConstantBufferView.
        UINT64 bufferLen = desc.len;
        if(desc.role == BufferDescriptor::Uniform){
            bufferLen = (bufferLen + 255ull) & ~255ull;
        }
        D3D12_RESOURCE_DESC d3d12_desc = CD3DX12_RESOURCE_DESC::Buffer(bufferLen,flags);
        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = heap_type;
        D3D12MA::Allocation *allocation = nullptr;
        hr = memAllocator->CreateResource(
            &allocDesc,
            &d3d12_desc,
            state,
            nullptr,
            &allocation,
            IID_PPV_ARGS(&buffer));

        if(FAILED(hr)){
            DEBUG_STREAM("Failed to Create D3D12 Buffer via D3D12MA");
            if (allocation) allocation->Release();
            return nullptr;
        };

        D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc {};
        descHeapDesc.NumDescriptors = 1;
        descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        descHeapDesc.NodeMask = d3d12_device->GetNodeCount();
        descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        ID3D12DescriptorHeap *descHeap;
        hr = d3d12_device->CreateDescriptorHeap(&descHeapDesc,IID_PPV_ARGS(&descHeap));
        if(FAILED(hr)){

        };

        if(desc.role == BufferDescriptor::Uniform){
            // §2.4 — uniform buffers bind via a root CBV (GPU virtual
            // address), so no per-buffer SRV/UAV descriptor is created here.
        }
        else if(desc.usage == BufferDescriptor::Upload) {

            D3D12_SHADER_RESOURCE_VIEW_DESC res_view_desc{};
            res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            res_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            res_view_desc.Format = DXGI_FORMAT_UNKNOWN;
            res_view_desc.Buffer.StructureByteStride = desc.objectStride;
            res_view_desc.Buffer.FirstElement = 0;
            res_view_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            res_view_desc.Buffer.NumElements = desc.len / desc.objectStride;

            d3d12_device->CreateShaderResourceView(buffer, &res_view_desc,
                                                   descHeap->GetCPUDescriptorHandleForHeapStart());
        }
        else {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
            uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav_desc.Format = DXGI_FORMAT_UNKNOWN;
            uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
            uav_desc.Buffer.NumElements = desc.len/desc.objectStride;
            uav_desc.Buffer.StructureByteStride = desc.objectStride;
            uav_desc.Buffer.CounterOffsetInBytes = 0;
            uav_desc.Buffer.FirstElement = 0;

            d3d12_device->CreateUnorderedAccessView(buffer,nullptr,&uav_desc,descHeap->GetCPUDescriptorHandleForHeapStart());
        }

        auto *d3d12_buffer = new GED3D12Buffer(desc.usage,buffer,descHeap,state,allocation);
        d3d12_buffer->role = desc.role;
        return SharedHandle<GEBuffer>(d3d12_buffer);
    }

    inline D3D12_TEXTURE_ADDRESS_MODE convertAddressMode(const omegasl_shader_static_sampler_address_mode & addressMode){
        switch (addressMode) {
            case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_WRAP : {
                return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            }
            case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_CLAMPTOEDGE : {
                return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            }
            case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRRORWRAP : {
                return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            }
            default : {
                break;
            }
        }
    }

    inline D3D12_TEXTURE_ADDRESS_MODE convertAddressModeGTE(const SamplerDescriptor::AddressMode & addressMode){
        switch (addressMode) {
            case SamplerDescriptor::AddressMode::Wrap : {
                return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            }
            case SamplerDescriptor::AddressMode::ClampToEdge : {
                return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            }
            case SamplerDescriptor::AddressMode::MirrorWrap : {
                return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            }
            default : {
                break;
            }
        }
    }


    bool GED3D12Engine::createRootSignatureFromOmegaSLShaders(unsigned int shaderN,
                                                              omegasl_shader *shader,
                                                              D3D12_ROOT_SIGNATURE_DESC1 * rootSignatureDesc,
                                                              ID3D12RootSignature **pRootSignature) {
        HRESULT hr;
        ArrayRef<omegasl_shader> shaders {shader,shader + shaderN};

        std::vector<D3D12_ROOT_PARAMETER1> params;
        std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;

        UINT registerSpace;

        for(auto & s : shaders){
            if(s.type == OMEGASL_SHADER_FRAGMENT){
                registerSpace = 1;
            }
            else {
                registerSpace = 0;
            }
            ArrayRef<omegasl_shader_layout_desc> sLayout {s.pLayout,s.pLayout + s.nLayout};
            for(auto & l : sLayout){
                CD3DX12_ROOT_PARAMETER1 parameter1;
                if(l.type == OMEGASL_SHADER_SAMPLER1D_DESC || l.type == OMEGASL_SHADER_SAMPLER2D_DESC
                   || l.type == OMEGASL_SHADER_SAMPLER3D_DESC || l.type == OMEGASL_SHADER_SAMPLERCUBE_DESC){
                    // Heap-allocate the range: InitAsDescriptorTable stores the
                    // pointer, which must outlive this loop until the root
                    // signature is serialized below (the texture path does the
                    // same `new`). A stack-local range would dangle — and with
                    // more than one sampler every param would alias the last.
                    // Carry the stage's `registerSpace` so a fragment-stage
                    // sampler (space1) resolves at bind time via
                    // getRootParameterIndexOfResource (Extension 8).
                    auto sampler_range = new CD3DX12_DESCRIPTOR_RANGE1();
                    sampler_range->Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,1,l.gpu_relative_loc,registerSpace);
                    parameter1.InitAsDescriptorTable(1,sampler_range);
                }
                else if(l.type == OMEGASL_SHADER_STATIC_SAMPLER2D_DESC || l.type == OMEGASL_SHADER_STATIC_SAMPLER3D_DESC
                        || l.type == OMEGASL_SHADER_STATIC_SAMPLERCUBE_DESC){
                    D3D12_FILTER filter;

                    switch (l.sampler_desc.filter) {
                        case OMEGASL_SHADER_SAMPLER_LINEAR_FILTER : {
                            filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                            break;
                        }
                        case OMEGASL_SHADER_SAMPLER_POINT_FILTER : {
                            filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                            break;
                        }
                        case OMEGASL_SHADER_SAMPLER_MAX_ANISOTROPY_FILTER : {
                            filter = D3D12_FILTER_MAXIMUM_ANISOTROPIC;
                            break;
                        }
                        case OMEGASL_SHADER_SAMPLER_MIN_ANISOTROPY_FILTER : {
                            filter = D3D12_FILTER_MINIMUM_ANISOTROPIC;
                            break;
                        }
                    }

                    CD3DX12_STATIC_SAMPLER_DESC desc;
                    desc.Init(l.gpu_relative_loc);
                    // desc.Init(l.gpu_relative_loc,filter, convertAddressMode(l.sampler_desc.u_address_mode),
                    //           convertAddressMode(l.sampler_desc.v_address_mode), convertAddressMode(l.sampler_desc.w_address_mode));
                    desc.RegisterSpace = s.type == OMEGASL_SHADER_FRAGMENT? 1 : 0;
                    staticSamplers.push_back(desc);
                    continue;
                }
                else if(l.type == OMEGASL_SHADER_BUFFER_DESC) {
                    if(l.io_mode == OMEGASL_SHADER_DESC_IO_IN) {
                        parameter1.InitAsShaderResourceView(l.gpu_relative_loc,registerSpace);
                    }
                    else {
                        parameter1.InitAsUnorderedAccessView(l.gpu_relative_loc,registerSpace);
                    }
                }
                else if(l.type == OMEGASL_SHADER_UNIFORM_DESC) {
                    /// §2.4 constant buffer — root CBV at register `b`
                    /// (`gpu_relative_loc` is the `b` register index from
                    /// HLSL codegen). Bound with SetGraphics/ComputeRoot-
                    /// ConstantBufferView from the buffer's GPU virtual address.
                    parameter1.InitAsConstantBufferView(l.gpu_relative_loc,registerSpace);
                }
                else if(l.type == OMEGASL_SHADER_PUSH_CONSTANT_DESC) {
                    /// §2.2 push constant — inline root 32-bit constants at
                    /// register `b` (`gpu_relative_loc` is the `b` register
                    /// from HLSL codegen, same class as a CBV but a distinct
                    /// root-parameter *type*). Bound with
                    /// SetGraphics/ComputeRoot32BitConstants from the bytes the
                    /// caller passes to set{Render,Compute}Constants.
                    ///
                    /// Num32BitValues is reserved at the portable 128-byte cap
                    /// (32 DWORDs) because the layout desc does not carry the
                    /// push block's struct size. This costs root-signature
                    /// space (the budget is 64 DWORDs total); threading the
                    /// exact byte size through the layout desc to size this
                    /// tightly is a follow-up.
                    parameter1.InitAsConstants(32, l.gpu_relative_loc, registerSpace);
                }
                /// Create Descriptor Table for Textures
                else {

                    auto range = new CD3DX12_DESCRIPTOR_RANGE1();
                    D3D12_DESCRIPTOR_RANGE_TYPE rangeType;
                    if(l.io_mode == OMEGASL_SHADER_DESC_IO_IN){
                        rangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                    }
                    else {
                        rangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                    }

                    // DATA_VOLATILE rather than the root-sig-1.1
                    // default of DATA_STATIC_WHILE_SET_AT_EXECUTE.
                    // The default forbids the bound resource from
                    // transitioning into a writable state between
                    // SetGraphicsRootDescriptorTable and the next
                    // Draw / Dispatch — which is exactly what the
                    // mipmap generator does as it cycles mip 0 through
                    // UAV → NON_PIXEL_SHADER_RESOURCE → UAV per pair.
                    // DATA_VOLATILE permits the transition at small
                    // optimization cost (driver loses a hoisting hint
                    // that is rarely load-bearing for our UI render
                    // workload). DESCRIPTORS_VOLATILE is also enabled
                    // so the descriptor heap contents themselves can
                    // be updated between bind and execute, which
                    // matches how the runtime authors per-frame
                    // descriptors today.
                    range->Init(rangeType, 1,
                                l.gpu_relative_loc, registerSpace,
                                D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
                    parameter1.InitAsDescriptorTable(1,range);
                }
                params.push_back(parameter1);
            }
        }

        auto root_params = new D3D12_ROOT_PARAMETER1[params.size()];
        std::copy(params.begin(),params.end(),root_params);
        auto static_samplers = new D3D12_STATIC_SAMPLER_DESC[staticSamplers.size()];
        std::copy(staticSamplers.begin(),staticSamplers.end(),static_samplers);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
        
        /// @note Always allow input layout regardless of pipeline type.
        desc.Init_1_1(params.size(),root_params,staticSamplers.size(),static_samplers,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> sigBlob;
        *rootSignatureDesc = desc.Desc_1_1;
        hr = D3DX12SerializeVersionedRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1_1,&sigBlob,nullptr);

        if(FAILED(hr)){
            return false;
        }

        hr = d3d12_device->CreateRootSignature(d3d12_device->GetNodeCount(),sigBlob->GetBufferPointer(),sigBlob->GetBufferSize(),IID_PPV_ARGS(pRootSignature));
        if(FAILED(hr)){
            return false;
        }
        return true;
    };


    SharedHandle<GESamplerState> GED3D12Engine::makeSamplerState(const SamplerDescriptor &desc) {
        D3D12_DESCRIPTOR_HEAP_DESC desc1 {};
        desc1.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        desc1.NodeMask = d3d12_device->GetNodeCount();
        desc1.NumDescriptors = 1;
        desc1.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        

        D3D12_FILTER filter;
        /// TOOD: Handle all Filter Cases
        switch (desc.filter) {
            case SamplerDescriptor::Filter::Linear : {
                filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                break;
            }
            case SamplerDescriptor::Filter::Point : {
                filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                break;
            }
            case SamplerDescriptor::Filter::MaxAnisotropic : {
                filter = D3D12_FILTER_MAXIMUM_ANISOTROPIC;
                break;
            }
            case SamplerDescriptor::Filter::MinAnisotropic : {
                filter = D3D12_FILTER_MINIMUM_ANISOTROPIC;
                break;
            }
        }

        ID3D12DescriptorHeap *descHeap;
        d3d12_device->CreateDescriptorHeap(&desc1,IID_PPV_ARGS(&descHeap));
        D3D12_SAMPLER_DESC samplerDesc {};
        samplerDesc.AddressU = convertAddressModeGTE(desc.uAddressMode);
        samplerDesc.AddressV = convertAddressModeGTE(desc.vAddressMode);
        samplerDesc.AddressW = convertAddressModeGTE(desc.wAddressMode);
        samplerDesc.Filter = filter;
        samplerDesc.MaxAnisotropy = desc.maxAnisotropy;
        
        d3d12_device->CreateSampler(&samplerDesc,descHeap->GetCPUDescriptorHandleForHeapStart());

        ATL::CStringW wstr(desc.name.data());
        descHeap->SetName(wstr.GetString());

        return SharedHandle<GESamplerState>(new GED3D12SamplerState(descHeap,samplerDesc));
    }

_NAMESPACE_END_
