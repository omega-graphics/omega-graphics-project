#include "omegaGTE/GECommandQueue.h"
#include "GED3D12.h"
#include <cstdint>

#ifndef OMEGAGTE_GED3D12COMMANDQUEUE_H
#define OMEGAGTE_GED3D12COMMANDQUEUE_H

_NAMESPACE_BEGIN_
    class GED3D12CommandQueue;
    class GED3D12RenderPipelineState;
    class GED3D12ComputePipelineState;
    class GED3D12TextureRenderTarget;
    class GED3D12NativeRenderTarget;

    class GED3D12CommandBuffer : public GECommandBuffer {
        ComPtr<ID3D12GraphicsCommandList6> commandList;
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        GED3D12CommandQueue *parentQueue;
        bool inComputePass;
        bool inBlitPass;
        bool inRenderPass;
        bool firstRenderPass = true;
        bool closed = false;
        std::uint64_t traceResourceId = 0;

        friend class GED3D12CommandQueue;

        struct {
            GED3D12NativeRenderTarget *native = nullptr;
            GED3D12TextureRenderTarget *texture = nullptr;
        } currentTarget;

        GED3D12RenderPipelineState *currentRenderPipeline = nullptr;
        GED3D12ComputePipelineState *currentComputePipeline = nullptr;

        D3D12_ROOT_SIGNATURE_DESC1 * currentRootSignature = nullptr;

        // Extension 8 — D3D12 allows at most one CBV/SRV/UAV heap and one
        // SAMPLER heap bound at a time. Track the last-bound heap of each
        // type so a sampler bind can keep the resource heap live (and vice
        // versa); `rebindDescriptorHeaps` sets both in one SetDescriptorHeaps
        // call. nullptr means "none bound yet".
        ID3D12DescriptorHeap *currentResourceDescHeap = nullptr;  // CBV/SRV/UAV
        ID3D12DescriptorHeap *currentSamplerDescHeap = nullptr;   // SAMPLER

       friend class GED3D12CommandQueue;

       unsigned getRootParameterIndexOfResource(unsigned id,omegasl_shader &shader);
       /// Extension 8 — (re)bind whichever of the resource / sampler heaps are
       /// set, so both types can be visible to a single draw / dispatch.
       void rebindDescriptorHeaps();
       D3D12_RESOURCE_STATES getRequiredResourceStateForResourceID(unsigned & id,omegasl_shader &shader);
       /// Combine a runtime swizzle override with the shader layout's
       /// `swizzle_desc` (texture-swizzle proposal §4 precedence rule).
       TextureSwizzle resolveEffectiveSwizzle(const TextureSwizzle & runtime,unsigned id,omegasl_shader &shader);
    public:

        bool multisampleResolvePass = false;

        void setName(OmegaCommon::StrRef name) override{
            ATL::CStringW wstr(name.data());
            commandList->SetName(wstr.GetString());
        }

        void *native() override {
            return (void *)commandList.Get();
        }

        void startBlitPass() override;
        void copyTextureToTexture(SharedHandle<GETexture> & src,SharedHandle<GETexture> & dest) override;
        void copyTextureToTexture(SharedHandle<GETexture> & src,SharedHandle<GETexture> & dest,const TextureRegion & region,const GPoint3D & destCoord) override;
        void copyBufferToBuffer(SharedHandle<GEBuffer> & src,SharedHandle<GEBuffer> & dest,
                                size_t size, size_t srcOffset, size_t destOffset) override;
        void copyBufferToTexture(SharedHandle<GEBuffer> & src,SharedHandle<GETexture> & dest,
                                 size_t bytesPerRow, size_t bytesPerImage,
                                 const TextureRegion & destRegion, size_t srcBufferOffset) override;
        void copyTextureToBuffer(SharedHandle<GETexture> & src,SharedHandle<GEBuffer> & dest,
                                 size_t bytesPerRow, size_t bytesPerImage,
                                 const TextureRegion & srcRegion, size_t destBufferOffset) override;
        void generateMipmaps(SharedHandle<GETexture> & texture) override;
        void blitWithPipeline(SharedHandle<GEBlitPipelineState> & pipelineState,
                              SharedHandle<GETexture> & src,
                              SharedHandle<GETexture> & dest) override;
        void blitWithPipeline(SharedHandle<GEBlitPipelineState> & pipelineState,
                              SharedHandle<GETexture> & src,
                              SharedHandle<GETexture> & dest,
                              const TextureRegion & srcRegion,
                              const TextureRegion & destRegion) override;
        void fillBuffer(SharedHandle<GEBuffer> & buffer, uint32_t value,
                        size_t offset, size_t size) override;
        void finishBlitPass() override;
        /// Raytracing Funcs
        void beginAccelStructPass() override;
        void buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, const GEAccelerationStructDescriptor &desc) override;
        void copyAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, SharedHandle<GEAccelerationStruct> &dest) override;
        void refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,SharedHandle<GEAccelerationStruct> &dest, const GEAccelerationStructDescriptor &desc) override;
        void finishAccelStructPass() override;

        void startRenderPass(const GERenderPassDescriptor &desc) override;
        void setVertexBuffer(SharedHandle<GEBuffer> &buffer) override;
        void setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState) override;
        void bindResourceAtVertexShader(SharedHandle<GEBuffer> &buffer, unsigned int id) override;
        void bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned int id,
                                        const TextureSwizzle & swizzle) override;
        void bindResourceAtVertexShader(SharedHandle<GESamplerState> &sampler, unsigned int id) override;
        void bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned int id) override;
        void bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned int id,
                                          const TextureSwizzle & swizzle) override;
        void bindResourceAtFragmentShader(SharedHandle<GESamplerState> &sampler, unsigned int id) override;
        void setStencilRef(unsigned int ref) override;
       
        void drawPolygons(RenderPassDrawPolygonType polygonType, unsigned int vertexCount, size_t startIdx) override;
        void setIndexBuffer(SharedHandle<GEBuffer> & buffer, RenderPassIndexType indexType) override;
        void drawIndexedPolygons(RenderPassDrawPolygonType polygonType,
                                 unsigned indexCount, size_t startIndex,
                                 int baseVertex) override;
        void drawPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                   unsigned vertexCount, size_t startIdx,
                                   unsigned instanceCount, unsigned firstInstance) override;
        void drawIndexedPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                           unsigned indexCount, size_t startIndex,
                                           int baseVertex, unsigned instanceCount,
                                           unsigned firstInstance) override;
        void drawPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                   SharedHandle<GEBuffer> & argumentBuffer,
                                   size_t argumentBufferOffset) override;
        void drawIndexedPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                          SharedHandle<GEBuffer> & argumentBuffer,
                                          size_t argumentBufferOffset) override;
        void setViewports(std::vector<GEViewport> viewports) override;
        void setScissorRects(std::vector<GEScissorRect> scissorRects) override;
        void finishRenderPass() override;

        void startComputePass(const GEComputePassDescriptor &desc) override;
        void setComputePipelineState(SharedHandle<GEComputePipelineState> &pipelineState) override;
        void bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned int id) override;
        void bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int id,
                                         const TextureSwizzle & swizzle) override;
        void bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> &accelStruct, unsigned int id) override;
        void dispatchRays(unsigned int x, unsigned int y, unsigned int z) override;
        void dispatchThreadgroups(unsigned int x, unsigned int y, unsigned int z) override;
        void dispatchThreads(unsigned int x, unsigned int y, unsigned int z) override;
        void dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                           size_t argumentBufferOffset) override;
        void finishComputePass() override;

        GED3D12CommandBuffer(ID3D12GraphicsCommandList6 *commandList,ID3D12CommandAllocator *commandAllocator,GED3D12CommandQueue *parentQueue);
        void reset() override;

        ~GED3D12CommandBuffer() override;
    };

    class GED3D12CommandQueue : public GECommandQueue {
        GED3D12Engine *engine;

        // Pending submit batch: command lists queued by submitCommandBuffer
        // calls and not yet handed to ExecuteCommandLists, plus the
        // SharedHandles / descriptor heaps they depend on. At flush time
        // these are moved into the engine retention queue with a fence gate
        // for the just-issued submit; they are NOT released inline.
        std::vector<ID3D12GraphicsCommandList6 *> commandLists;
        std::vector<SharedHandle<GECommandBuffer>> retainedCommandBuffers;
        std::vector<ComPtr<ID3D12DescriptorHeap>> retainedDescriptorHeaps;
        ComPtr<ID3D12CommandQueue> commandQueue;

        // Monotonic fence for retention gating. Distinct from `fence` below,
        // which is the binary-valued fence used by commitToGPUAndWait. Each
        // ExecuteCommandLists is paired with Signal(retentionFence,
        // ++nextSubmitValue); the captured value drives the FenceGate handed
        // to the engine retention queue.
        ComPtr<ID3D12Fence> retentionFence;
        std::uint64_t       nextSubmitValue = 0;

        // Build a gate that returns true once the next submit's signal value
        // (i.e. ++nextSubmitValue) has been reached on retentionFence. Must be
        // called immediately before issuing the corresponding Signal().
        Retention::FenceGate gateForNextSubmit();

        // Move every accumulated retained command buffer / descriptor heap
        // into the engine retention queue under `gate`, then clear the local
        // pending vectors.
        void flushPendingRetentionUnder(const Retention::FenceGate &gate);

        ComPtr<ID3D12Fence> fence;

        HANDLE cpuEvent;
        std::uint64_t traceResourceId = 0;
        std::vector<std::uint64_t> submittedTraceCommandBufferIds;

        bool multiQueueSync = false;

        unsigned currentCount;
        friend class GED3D12Engine;
        friend class GED3D12CommandBuffer;
    public:
        void setName(OmegaCommon::StrRef name) override{
            ATL::CStringW wstr(name.data());
            commandQueue->SetName(wstr);
        }

        void *native() override {
            return (void *)commandQueue.Get();
        }
        std::uint64_t traceId() const {
            return traceResourceId;
        }
        std::uint64_t lastSubmittedCommandBufferTraceId() const {
            if(submittedTraceCommandBufferIds.empty()){
                return 0;
            }
            return submittedTraceCommandBufferIds.back();
        }
        void clearSubmittedTraceCommandBufferIds() {
            submittedTraceCommandBufferIds.clear();
        }
        ID3D12GraphicsCommandList6 * getLastCommandList();
        void commitToGPU() override;
        void commitToGPUAndWait() override;
        void notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &waitFence) override;
        void submitCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer) override;
        void submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &signalFence) override;
        void signalFence(SharedHandle<GEFence> & fence) override;
        void waitForFence(SharedHandle<GEFence> & fence, std::uint64_t value) override;
        SharedHandle<GECommandBuffer> getAvailableBuffer() override;
        GED3D12CommandQueue(GED3D12Engine *engine,unsigned size);
        ~GED3D12CommandQueue() override;
    };
_NAMESPACE_END_
#endif
