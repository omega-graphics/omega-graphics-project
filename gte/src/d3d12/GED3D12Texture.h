#include "GED3D12.h"
#include "omegaGTE/GETexture.h"
#include <cstdint>
#include <utility>

#ifndef OMEGAGTE_D3D12_GED3D12TEXTURE_H
#define OMEGAGTE_D3D12_GED3D12TEXTURE_H

_NAMESPACE_BEGIN_

class GED3D12Texture : public GETexture, public GED3D12EngineBackRef {
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

    /// Shared-Descriptor-Heap-Plan Phase 2: SRV / UAV slots live in the
    /// engine's device-wide shader-visible CBV/SRV/UAV heap. Invalid
    /// handle = no view of that kind for this texture. RTV / DSV stay as
    /// per-texture non-shader-visible heaps (different heap type, low
    /// churn, lifetime-tied).
    D3D12DescriptorHandle srvHandle{};
    D3D12DescriptorHandle uavHandle{};
    ComPtr<ID3D12DescriptorHeap> rtvDescHeap;
    ComPtr<ID3D12DescriptorHeap> dsvDescHeap;

    /// Owning engine pointer for descriptor-slot lifetime management. In the
    /// common case the engine outlives every texture it hands out
    /// (waitForGPUIdle + retentionQueue.drainAll run in `~GED3D12Engine` before
    /// the allocators are released). §16 Phase H hardens the reverse case — a
    /// texture that outlives the engine — via `GED3D12EngineBackRef`: the engine
    /// nulls this pointer on teardown, so the destructor skips the slot-free.
    GED3D12Engine *owningEngine = nullptr;
    /// §16 Phase H — engine teardown detach (see `GED3D12EngineBackRef`).
    void onEngineDestroyed() noexcept override { owningEngine = nullptr; }

    /// Captured at primary-SRV creation time so swizzled-view recreation
    /// can clone every field except `Shader4ComponentMapping`. Populated
    /// only for textures that actually have an SRV (`isSRV` in
    /// `GED3D12Engine::makeTexture`); other textures leave it default-init
    /// and the cache helper short-circuits to the primary handle.
    D3D12_SHADER_RESOURCE_VIEW_DESC primarySrvDesc{};
    bool hasPrimarySrvDesc = false;

    /// Lazily-allocated swizzled-SRV slots in the engine's shared CBV/SRV/UAV
    /// heap, keyed by swizzle. Linear scan (typical swizzle counts are 1-3).
    /// Each slot is deferred-freed alongside the parent texture in the
    /// destructor.
    OmegaCommon::Vector<std::pair<TextureSwizzle, D3D12DescriptorHandle>> swizzledSrvCache;

    /// Resolve (or lazily create) an SRV slot whose descriptor has the
    /// requested swizzle baked into `Shader4ComponentMapping`. Identity
    /// returns the primary `srvHandle` directly. If the texture has no
    /// captured base SRV desc, falls back to the primary handle silently.
    D3D12DescriptorHandle getOrCreateSwizzledSrvHandle(GED3D12Engine *engine,
                                                        const TextureSwizzle & swizzle);

    // D3D12MA-owned suballocations. nullptr when the corresponding resource
    // was created outside the allocator (heap-placed via GED3D12Heap, or
    // imported swap-chain back buffer). Released exactly once in the
    // destructor, after the COM ref to the resource has been dropped.
    D3D12MA::Allocation *d3d12maAllocation        = nullptr;
    D3D12MA::Allocation *d3d12maCpuSideAllocation = nullptr;

    // Allocator-Lifetime-Hardening Phase 1 — keeps the D3D12MA allocator alive
    // at least as long as this texture's allocations. Set by the
    // allocator-creating makeTexture paths; null for textures made outside the
    // allocator (imported swap-chain / DirectXTK ScratchImage uploads). The
    // destructor releases the allocations above while this member still
    // guarantees the allocator is live.
    std::shared_ptr<GED3D12AllocatorOwner> allocatorOwner;

    D3D12_RESOURCE_STATES currentState;

    void downloadTextureToReadbackHeap(ID3D12GraphicsCommandList *commandList);
    void uploadTextureFromUploadHeap(ID3D12GraphicsCommandList *commandList);

    /// True for a CPU-readback texture (its `getBytes` maps the READBACK
    /// companion). Exposes the protected `usage` so the command-buffer copy
    /// paths can populate that companion after writing the primary resource.
    bool isReadbackTexture() const { return usage == FromGPU; }

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
            GED3D12Engine *engine,
            const TextureKind & kind,
            const GETextureUsage & usage,
            const TexturePixelFormat & pixelFormat,
            ID3D12Resource *res,
            ID3D12Resource *cpuSideRes,
            const D3D12DescriptorHandle & srvHandle,
            const D3D12DescriptorHandle & uavHandle,
            ID3D12DescriptorHeap *rtvDescHeap,
            ID3D12DescriptorHeap *dsvDescHeap,
            D3D12_RESOURCE_STATES & currentState,
            D3D12MA::Allocation *d3d12maAllocation = nullptr,
            D3D12MA::Allocation *d3d12maCpuSideAllocation = nullptr);
    ~GED3D12Texture() override;
};

_NAMESPACE_END_

#endif
