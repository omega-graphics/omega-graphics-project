#include "GED3D12.h"
#include "omegaGTE/GETexture.h"
#include <cstdint>
#include <utility>

#ifndef OMEGAGTE_D3D12_GED3D12TEXTURE_H
#define OMEGAGTE_D3D12_GED3D12TEXTURE_H

_NAMESPACE_BEGIN_

class GED3D12Texture : public GETexture {
public:
    std::uint64_t traceResourceId = 0;
    void copyBytes(void *bytes, size_t len) override;
    void copyBytes(void *bytes, size_t bytesPerRow, const TextureRegion &destRegion) override;
    size_t getBytes(void *bytes, size_t bytesPerRow) override;

    bool onGpu;

    /// §7.1: subresource indices written via copyBytes(region) since the
    /// last upload. uploadTextureFromUploadHeap copies each of these; when
    /// empty it falls back to subresource 0, preserving the legacy
    /// whole-mip-0 copyBytes(bytes,bytesPerRow) path. Cleared after upload.
    OmegaCommon::Vector<UINT> dirtySubresources;

    ComPtr<ID3D12Resource> resource;

    ComPtr<ID3D12Resource> cpuSideresource;
    ComPtr<ID3D12DescriptorHeap> srvDescHeap;
    ComPtr<ID3D12DescriptorHeap> uavDescHeap;
    ComPtr<ID3D12DescriptorHeap> rtvDescHeap;
    ComPtr<ID3D12DescriptorHeap> dsvDescHeap;

    /// Captured at primary-SRV creation time so swizzled-view recreation
    /// can clone every field except `Shader4ComponentMapping`. Populated
    /// only for textures that actually have an SRV (`isSRV` in
    /// `GED3D12Engine::makeTexture`); other textures leave it default-init
    /// and the cache helper short-circuits to the primary heap.
    D3D12_SHADER_RESOURCE_VIEW_DESC primarySrvDesc{};
    bool hasPrimarySrvDesc = false;

    /// Lazily-created single-slot SRV descriptor heaps for non-identity
    /// swizzles requested at bind time. Linear scan; freed alongside the
    /// parent texture when the ComPtr drops to zero.
    OmegaCommon::Vector<std::pair<TextureSwizzle, ComPtr<ID3D12DescriptorHeap>>> swizzledSrvCache;

    /// Resolve (or lazily create) an SRV heap whose single descriptor has
    /// the requested swizzle baked in via `Shader4ComponentMapping`.
    /// Identity returns `srvDescHeap` directly. If the texture has no
    /// captured base SRV desc, falls back to the primary heap silently.
    ID3D12DescriptorHeap *getOrCreateSwizzledSrvHeap(ID3D12Device *device,
                                                     const TextureSwizzle & swizzle);

    // D3D12MA-owned suballocations. nullptr when the corresponding resource
    // was created outside the allocator (heap-placed via GED3D12Heap, or
    // imported swap-chain back buffer). Released exactly once in the
    // destructor, after the COM ref to the resource has been dropped.
    D3D12MA::Allocation *d3d12maAllocation        = nullptr;
    D3D12MA::Allocation *d3d12maCpuSideAllocation = nullptr;

    D3D12_RESOURCE_STATES currentState;

    void downloadTextureToReadbackHeap(ID3D12GraphicsCommandList *commandList);
    void uploadTextureFromUploadHeap(ID3D12GraphicsCommandList *commandList);

    void updateAndValidateStatus(ID3D12GraphicsCommandList *commandList);
    bool needsValidation();

    void setName(OmegaCommon::StrRef name) override{
        ATL::CStringW wstr(name.data());
        resource->SetName(wstr);
    }

    void *native() override {
        return (void *)resource.Get();
    }

    explicit GED3D12Texture(
            const TextureKind & kind,
            const GETextureUsage & usage,
            const TexturePixelFormat & pixelFormat,
            ID3D12Resource *res,
            ID3D12Resource *cpuSideRes,
            ID3D12DescriptorHeap *descHeap,
            ID3D12DescriptorHeap *uavDescHeap,
            ID3D12DescriptorHeap *rtvDescHeap,
            ID3D12DescriptorHeap *dsvDescHeap,
            D3D12_RESOURCE_STATES & currentState,
            D3D12MA::Allocation *d3d12maAllocation = nullptr,
            D3D12MA::Allocation *d3d12maCpuSideAllocation = nullptr);
    ~GED3D12Texture() override;
};

_NAMESPACE_END_

#endif
