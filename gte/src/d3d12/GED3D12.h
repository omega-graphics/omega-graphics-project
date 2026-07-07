#include "omegaGTE/GE.h"

#include "d3dx12.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <d3d12shader.h>
#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <iostream>
#include <atlstr.h>
#include <cstdint>

#include <wrl.h>
#include <memory>
#include <vector>
#include <set>
#include <mutex>
#include "D3D12MemAlloc.h"
#include "../common/GEResourceTracker.h"
#include "../common/GERetentionQueue.h"
#include "D3D12DescriptorAllocator.h"

#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"dxguid.lib")
#pragma comment(lib,"runtimeobject.lib")
#pragma comment(lib,"d3dcompiler.lib")

#ifndef OMEGAGTE_GED3D12_H
#define OMEGAGTE_GED3D12_H

_NAMESPACE_BEGIN_
    using Microsoft::WRL::ComPtr;

    // Allocator-Lifetime-Hardening Phase 1 — shared owner of the D3D12MA
    // allocator. The engine and every allocator-created resource each hold a
    // `shared_ptr<GED3D12AllocatorOwner>`; the underlying `D3D12MA::Allocator`
    // is `Release()`d exactly once, when the last holder drops. That makes
    // teardown order-independent: `Close()` (→ `~GED3D12Engine`) is safe even
    // while a caller still holds `GEBuffer` / `GETexture` handles — whichever
    // party outlives the other tears the allocator down. Resources created
    // outside the allocator (heap-placed / imported swap-chain buffers) simply
    // hold a null owner.
    struct GED3D12AllocatorOwner {
        D3D12MA::Allocator *allocator = nullptr;
        explicit GED3D12AllocatorOwner(D3D12MA::Allocator *allocator): allocator(allocator) {}
        ~GED3D12AllocatorOwner() {
            if (allocator) {
                allocator->Release();
                allocator = nullptr;
            }
        }
        GED3D12AllocatorOwner(const GED3D12AllocatorOwner &) = delete;
        GED3D12AllocatorOwner & operator=(const GED3D12AllocatorOwner &) = delete;
    };

    // §16 Phase H — engine back-reference hardening (teardown-order safety,
    // sibling to `GED3D12AllocatorOwner`). A `GED3D12Texture` / `GED3D12SamplerState`
    // keeps a raw `owningEngine` pointer so its destructor can hand its shared
    // descriptor-heap slot back to the engine's allocator. If such a resource
    // outlives the engine — a caller drops the resource handle after `Close()`
    // has already run `~GED3D12Engine` — that pointer would dangle and the
    // destructor would fault. Every such resource registers with the engine;
    // `~GED3D12Engine` calls `onEngineDestroyed()` on each still-registered one,
    // which nulls the back-pointer. The destructor then skips the slot-free —
    // correct, because the engine and its descriptor heaps are already gone and
    // the GPU was drained to idle before teardown, so the free is moot. (The
    // D3D12MA allocations these resources also hold are already teardown-safe via
    // `GED3D12AllocatorOwner`; this closes the remaining descriptor-slot path.)
    struct GED3D12EngineBackRef {
        virtual void onEngineDestroyed() noexcept = 0;
    protected:
        ~GED3D12EngineBackRef() = default;
    };

    class GED3D12Buffer : public GEBuffer {
    public:

        ComPtr<ID3D12Resource> buffer;
        std::uint64_t traceResourceId = 0;

        D3D12_RESOURCE_STATES currentState;

        // D3D12MA-owned suballocation backing `buffer`. nullptr for resources
        // created outside the allocator (heap-placed or imported swap-chain
        // buffers); when non-null, must be Release()d to free the underlying
        // memory. Owned: ref-released exactly once in the destructor.
        D3D12MA::Allocation *d3d12maAllocation = nullptr;

        // D3D12-CPU-Accessible-Buffer-Plan Phase 1 — CPU-side companion for a
        // `Readback` (or `Universal`) buffer. The primary `buffer` lives on a
        // DEFAULT heap (so the GPU can UAV-write it); this READBACK companion
        // receives the GPU→CPU copy and is what `GEBufferReader` maps. nullptr
        // for Upload / GPUOnly / Uniform buffers, which keep their CPU-visible
        // primary resource. Released alongside its allocation in the destructor.
        ComPtr<ID3D12Resource> cpuSideResource;
        D3D12MA::Allocation *cpuSideAllocation = nullptr;

        // `Universal`-usage upload companion (UPLOAD heap, GENERIC_READ).
        // GEBufferWriter maps THIS instead of the non-mappable DEFAULT
        // primary; the written range is copied into the primary the next time
        // the buffer is bound in a compute pass or used by a blit copy
        // (GED3D12CommandBuffer::flushPendingUpload). nullptr for every other
        // usage.
        ComPtr<ID3D12Resource> uploadCompanion;
        D3D12MA::Allocation *uploadAllocation = nullptr;
        // Bytes [0, uploadDirtyBytes) of `uploadCompanion` hold CPU writes not
        // yet copied into the primary; 0 means clean. Set by the buffer
        // writer's flush; cleared by flushPendingUpload, and by a blit copy
        // into this buffer (which happens after the CPU write in program order
        // and therefore supersedes it).
        UINT64 uploadDirtyBytes = 0;

        // Allocator-Lifetime-Hardening Phase 1 — keeps the D3D12MA allocator
        // alive at least as long as this buffer's allocations. Set by the
        // allocator-creating makeBuffer paths; null for resources made outside
        // the allocator. The destructor body releases the allocations above
        // while this member (destroyed only after the body) still guarantees
        // the allocator is live.
        std::shared_ptr<GED3D12AllocatorOwner> allocatorOwner;

        void setName(OmegaCommon::StrRef name) override{
            ATL::CStringW wstr(name.data());
            buffer->SetName(wstr);
        }

        void *native() override {
            return (void *)buffer.Get();
        }

        size_t size() override{
            return buffer->GetDesc().Width;
        };
        GED3D12Buffer(const BufferDescriptor::Usage & usage,ID3D12Resource *buffer, D3D12_RESOURCE_STATES currentState, D3D12MA::Allocation *d3d12maAllocation = nullptr,
                      ID3D12Resource *cpuSideResource = nullptr, D3D12MA::Allocation *cpuSideAllocation = nullptr,
                      ID3D12Resource *uploadCompanion = nullptr, D3D12MA::Allocation *uploadAllocation = nullptr):
        GEBuffer(usage),buffer(buffer),
        traceResourceId(ResourceTracking::Tracker::instance().nextResourceId()), currentState(currentState),
        d3d12maAllocation(d3d12maAllocation),
        cpuSideResource(cpuSideResource), cpuSideAllocation(cpuSideAllocation),
        uploadCompanion(uploadCompanion), uploadAllocation(uploadAllocation){

            ResourceTracking::Tracker::instance().emit(
                    ResourceTracking::EventType::Create,
                    ResourceTracking::Backend::D3D12,
                    "Buffer",
                    traceResourceId,
                    this->buffer.Get(),
                    static_cast<float>(this->buffer != nullptr ? this->buffer->GetDesc().Width : 0));
            DEBUG_INFO(DEBUG_DOMAIN_RESOURCE, "Buffer created: id=" << traceResourceId
                       << " size=" << (this->buffer != nullptr ? this->buffer->GetDesc().Width : 0));
        };
        ~GED3D12Buffer() override {
            ResourceTracking::Tracker::instance().emit(
                    ResourceTracking::EventType::Destroy,
                    ResourceTracking::Backend::D3D12,
                    "Buffer",
                    traceResourceId,
                    this->buffer.Get(),
                    static_cast<float>(this->buffer != nullptr ? this->buffer->GetDesc().Width : 0));
            DEBUG_INFO(DEBUG_DOMAIN_RESOURCE, "Buffer destroyed: id=" << traceResourceId);
            // Drop the COM ref to the resource before releasing the
            // allocation so D3D12MA's leak validator sees the resource
            // already destroyed when the allocation goes away.
            buffer.Reset();
            if (d3d12maAllocation) {
                d3d12maAllocation->Release();
                d3d12maAllocation = nullptr;
            }
            // D3D12-CPU-Accessible-Buffer-Plan Phase 1 — release the Readback
            // companion (if any) the same way: resource ref first, then its
            // D3D12MA allocation.
            cpuSideResource.Reset();
            if (cpuSideAllocation) {
                cpuSideAllocation->Release();
                cpuSideAllocation = nullptr;
            }
            // Universal-usage upload companion, same discipline.
            uploadCompanion.Reset();
            if (uploadAllocation) {
                uploadAllocation->Release();
                uploadAllocation = nullptr;
            }
        }
    };

    class GED3D12Fence : public GEFence {
    public:
        ComPtr<ID3D12Fence> fence;
        std::uint64_t lastSignaledValue = 0;
        std::uint64_t nextSignalValue = 1;
        void setName(OmegaCommon::StrRef name) override{
            ATL::CStringW str(name.data(),INT(name.size()));
            fence->SetName(str);
        }
        void *native() override {
            return fence.Get();
        }
        std::uint64_t getLastSignaledValue() const override { return lastSignaledValue; }
        explicit GED3D12Fence(ID3D12Fence *fence):fence(fence){};
        ~GED3D12Fence() override = default;
    };

    class GED3D12Engine;

    class GED3D12SamplerState : public GESamplerState, public GED3D12EngineBackRef {
    public:
        // Phase 2 — sampler slot inside the engine's shared SAMPLER heap.
        D3D12DescriptorHandle samplerHandle{};
        D3D12_SAMPLER_DESC staticSampler;
        GED3D12Engine *owningEngine = nullptr;
        // §16 Phase H — engine teardown detach: drop the back-pointer so the
        // destructor skips its (now-moot) descriptor-slot free.
        void onEngineDestroyed() noexcept override { owningEngine = nullptr; }
        // Constructor is out-of-line (GED3D12.cpp) so it can `registerBackRef`
        // with the engine, whose full type is not visible at this point.
        explicit GED3D12SamplerState(GED3D12Engine *engine,
                                     const D3D12DescriptorHandle & handle,
                                     D3D12_SAMPLER_DESC & samplerDesc);
        // SharedHandle<GESamplerState> is shared_ptr, whose deleter uses the
        // static type at wrap time, so this destructor runs even though the
        // base GESamplerState has no virtual dtor.
        ~GED3D12SamplerState();
    };

    class GED3D12Engine;
    struct GTED3D12Device;

    class GED3D12Heap : public GEHeap {
        GED3D12Engine *engine;
        // D3D12MA pool backing this heap. One-block-per-pool with the
        // requested size matches the previous CreateHeap behavior; suballocation
        // and offset tracking move into D3D12MA. Released in the destructor.
        D3D12MA::Pool *pool;
        size_t poolSize;
        // Allocator-Lifetime-Hardening Phase 1 — keeps the D3D12MA allocator
        // alive at least as long as this heap's `pool`. `D3D12MA::Pool` is
        // created from the allocator and its `Release()` in the destructor needs
        // the allocator still live, so the heap holds an owner ref exactly like
        // allocator-created buffers / textures do.
        std::shared_ptr<GED3D12AllocatorOwner> allocatorOwner;
    public:
        GED3D12Heap(GED3D12Engine *engine, D3D12MA::Pool *pool, size_t poolSize,
                    std::shared_ptr<GED3D12AllocatorOwner> allocatorOwner)
            : engine(engine), pool(pool), poolSize(poolSize), allocatorOwner(std::move(allocatorOwner)) {};
        size_t currentSize() override { return poolSize; };
        SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor &desc) override;
        SharedHandle<GETexture> makeTexture(const TextureDescriptor &desc) override;
        ~GED3D12Heap() override {
            if (pool) {
                pool->Release();
                pool = nullptr;
            }
        };
    };
    struct GED3D12AccelerationStruct : public GEAccelerationStruct {
        SharedHandle<GED3D12Buffer> structBuffer;
        SharedHandle<GED3D12Buffer> scratchBuffer;
        explicit GED3D12AccelerationStruct(
            SharedHandle<GED3D12Buffer> & structBuffer,
            SharedHandle<GED3D12Buffer> & scratchBuffer);
    };

    class GED3D12Engine : public OmegaGraphicsEngine {
        SharedHandle<GTEShader> _loadShaderFromDesc(omegasl_shader *shaderDesc,bool runtime) override;
    public:
        ComPtr<IDXGIFactory6> dxgi_factory;
        explicit GED3D12Engine(SharedHandle<GTED3D12Device> device);
        ~GED3D12Engine() override;
        ComPtr<ID3D12Debug1> debug_interface;
#ifdef __ID3D12InfoQueue1_INTERFACE_DEFINED__
        ComPtr<ID3D12InfoQueue1> debug_info_queue;
#endif
        DWORD debug_message_cookie = 0;
        ComPtr<ID3D12Device8> d3d12_device;
        // Raw, non-owning convenience pointer to the D3D12MA allocator used by
        // every CreateResource / CreatePool call. Ownership lives in
        // `allocatorOwner` (Allocator-Lifetime-Hardening Phase 1); this stays
        // valid for as long as the engine holds its `allocatorOwner` ref.
        D3D12MA::Allocator *memAllocator = nullptr;
        // Shared owner of `memAllocator`. Dropping the engine's ref in
        // `~GED3D12Engine` no longer forces the allocator's release — any
        // still-live resource keeps it alive until the last handle drops.
        std::shared_ptr<GED3D12AllocatorOwner> allocatorOwner;
        SharedHandle<GTEDevice> gteDevice;

        // GPU-safe deferred-release queue. Encoders / submit paths hand
        // resources here gated on a per-queue retention fence; the entries
        // are released at drainCompleted() time, after the GPU is provably
        // done with them. See gte/docs/GPU-Safe-Resource-Deletion-Plan.md.
        Retention::Queue retentionQueue;

        // Weak-ref registry of every command queue this engine has handed
        // out. Used by ~GED3D12Engine to issue a per-queue Signal+wait so
        // the GPU is provably idle before retentionQueue.drainAll() runs.
        // D3D12 has no device-wide wait-idle; we have to do it per queue.
        OmegaCommon::Vector<std::weak_ptr<GECommandQueue>> liveCommandQueues;

        // Mipmap generation pipeline (compiled from gte/src/shaders/mipmap_gen_2d.omegasl
        // via the OmegaSL runtime compiler). Lazily created on first use.
        SharedHandle<GEComputePipelineState> mipmapGenPipeline;
        std::shared_ptr<omegasl_shader_lib> mipmapGenShaderLib;
        bool ensureMipmapGenPipeline();

        // Built-in full-screen-triangle vertex shader used by every blit
        // pipeline (Extension 3). Compiled lazily on first makeBlitPipelineState.
        SharedHandle<GTEShader> blitFullscreenVs;
        std::shared_ptr<omegasl_shader_lib> blitFullscreenVsLib;
        bool ensureBlitFullscreenVs();

        // Indirect command signatures. Lazily created. These are generic
        // signatures (no root parameters) carrying a single argument type.
        ComPtr<ID3D12CommandSignature> drawIndirectSignature;
        ComPtr<ID3D12CommandSignature> drawIndexedIndirectSignature;
        ComPtr<ID3D12CommandSignature> dispatchIndirectSignature;
        ID3D12CommandSignature * getDrawIndirectSignature();
        ID3D12CommandSignature * getDrawIndexedIndirectSignature();
        ID3D12CommandSignature * getDispatchIndirectSignature();
        void * underlyingNativeDevice() override {
            return d3d12_device.Get();
        }
        // D3D12 has no device-wide wait-idle; iterate liveCommandQueues
        // and Signal+wait on each one. Mirrors what ~GED3D12Engine
        // does in its first stanza, but exposed publicly so callers
        // (OmegaGTE::Close, Compositor::~Compositor) can force a
        // GPU drain before they release resources held outside the
        // engine. Without this override the base-class waitForGPUIdle
        // is an empty {} and `<final-release>` of any GPU-referenced
        // resource trips the D3D12 debug layer's CORRUPTION 921.
        void waitForGPUIdle() override;
        // ComPtr<ID3D12DescriptorHeap> descriptorHeapForRes;

        // Single-slot shader-visible CBV/SRV/UAV heap reused by
        // GED3D12CommandBuffer::fillBuffer. ClearUnorderedAccessViewUint
        // requires a matched (CPU, GPU) UAV handle pair, so the UAV is
        // rewritten in place per fill rather than allocated per buffer.
        // Created at engine init, released in the destructor. Single-slot
        // suffices while fills are serialized per command queue; promote
        // to a fence-keyed ring if concurrent fills become possible.
        ComPtr<ID3D12DescriptorHeap> clearUavHelperHeap;

        // Shared-Descriptor-Heap-Plan Phase 2: one device-wide shader-
        // visible CBV/SRV/UAV heap and one device-wide shader-visible
        // SAMPLER heap. Texture / sampler creation suballocates a slot;
        // the per-resource heap that the old path created per call is
        // gone. Released after waitForGPUIdle() in the destructor.
        std::unique_ptr<D3D12DescriptorAllocator> resourceDescriptorAllocator;  // CBV/SRV/UAV
        std::unique_ptr<D3D12DescriptorAllocator> samplerDescriptorAllocator;   // SAMPLER

        // §16 Phase H — registry of resources that keep a raw `owningEngine`
        // back-pointer (textures + samplers, which free a shared descriptor-heap
        // slot on destruction). `~GED3D12Engine` nulls each still-registered
        // one's back-pointer (`onEngineDestroyed`) so a resource that outlives
        // the engine skips its slot-free instead of dereferencing freed engine
        // memory. The mutex guards the (rare) case a resource is released off the
        // frame thread while another resource registers/unregisters. See
        // `GED3D12EngineBackRef`.
        std::mutex engineBackRefMutex;
        std::set<GED3D12EngineBackRef *> engineBackRefs;
        void registerBackRef(GED3D12EngineBackRef *ref){
            std::lock_guard<std::mutex> lk(engineBackRefMutex);
            engineBackRefs.insert(ref);
        }
        void unregisterBackRef(GED3D12EngineBackRef *ref){
            std::lock_guard<std::mutex> lk(engineBackRefMutex);
            engineBackRefs.erase(ref);
        }

        // Build a fence-gate vector that signals once every command list
        // currently submitted to every live command queue has retired.
        // Used by texture / sampler destruction paths to defer descriptor-
        // slot frees behind GPU completion via `retentionQueue`. Empty if
        // no queues are live (no submissions to wait on).
        std::vector<Retention::FenceGate> snapshotAllQueuesGates();

        // Convenience wrapper: enqueue a deferred free of `handle` against
        // `allocator`, gated on the current live-queue snapshot. Safe to
        // call from any thread; if no queues are live, frees immediately.
        void freeDescriptorAfterQueueDrain(D3D12DescriptorAllocator *allocator,
                                           const D3D12DescriptorHandle &handle);

        static SharedHandle<OmegaGraphicsEngine> Create(SharedHandle<GTEDevice> & device);
        // SharedHandle<GEShaderLibrary> loadShaderLibrary(FS::Path path);
        // SharedHandle<GEShaderLibrary> loadStdShaderLibrary();
        
        SharedHandle<GEBuffer> createBoundingBoxesBuffer(OmegaCommon::ArrayRef<GERaytracingBoundingBox> boxes) override;
        SharedHandle<GEAccelerationStruct> allocateAccelerationStructure(const GEAccelerationStructDescriptor &desc) override;
       
        bool createRootSignatureFromOmegaSLShaders(unsigned shaderN,omegasl_shader *shader,D3D12_ROOT_SIGNATURE_DESC1 * rootSignatureDesc,ID3D12RootSignature **pRootSignature);
        SharedHandle<GEFence> makeFence() override;
        SharedHandle<GESamplerState> makeSamplerState(const SamplerDescriptor &desc) override;
        SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor &desc)  override;
        SharedHandle<GEHeap> makeHeap(const HeapDescriptor &desc)  override;
        SharedHandle<GECommandQueue> makeCommandQueue(const GECommandQueueDesc & desc) override;
        SharedHandle<GETexture> makeTexture(const TextureDescriptor &desc)  override;
        SharedHandle<GERenderPipelineState> makeRenderPipelineState(RenderPipelineDescriptor &desc)  override;
        SharedHandle<GEComputePipelineState> makeComputePipelineState(ComputePipelineDescriptor &desc)  override;
        SharedHandle<GEBlitPipelineState> makeBlitPipelineState(BlitPipelineDescriptor &desc) override;
        /// Mesh-Shader-Plan Phase 3 — public API stub. Returns nullptr
        /// with a diagnostic for now; Phase 4b lands the real
        /// CD3DX12_PIPELINE_MESH_STATE_STREAM PSO build.
        SharedHandle<GERenderPipelineState> makeMeshPipelineState(MeshPipelineDescriptor &desc) override;
        SharedHandle<GENativeRenderTarget> makeNativeRenderTarget(const NativeRenderTargetDescriptor &desc,
                                                                   SharedHandle<GECommandQueue> presentQueue)  override;
        SharedHandle<GETextureRenderTarget> makeTextureRenderTarget(const TextureRenderTargetDescriptor &desc)  override;
        IDXGISwapChain3 *createSwapChainForComposition(DXGI_SWAP_CHAIN_DESC1 *desc,SharedHandle<GECommandQueue> & commandQueue);
        IDXGISwapChain3 *createSwapChainFromHWND(HWND hwnd,DXGI_SWAP_CHAIN_DESC1 *desc,SharedHandle<GECommandQueue> & commandQueue);
    };
_NAMESPACE_END_
#endif


