#include "omegaGTE/GECommandQueue.h"
#include "GED3D12.h"

#ifndef OMEGAGTE_GED3D12COMMANDQUEUE_H
#define OMEGAGTE_GED3D12COMMANDQUEUE_H

_NAMESPACE_BEGIN_
    class GED3D12CommandQueue;
    class GED3D12RenderPipelineState;
    class GED3D12ComputePipelineState;

    class GED3D12CommandBuffer : public GECommandBuffer {
        ComPtr<ID3D12GraphicsCommandList6> commandList;
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        GED3D12CommandQueue *parentQueue;
        bool inComputePass;
        bool inBlitPass;
        bool inRenderPass;
        bool firstRenderPass = true;
        bool closed = false;

        friend class GED3D12CommandQueue;

        struct {
            GED3D12NativeRenderTarget *native = nullptr;
            GED3D12TextureRenderTarget *texture = nullptr;
        } currentTarget;

        GED3D12RenderPipelineState *currentRenderPipeline = nullptr;
        GED3D12ComputePipelineState *currentComputePipeline = nullptr;

        D3D12_ROOT_SIGNATURE_DESC1 * currentRootSignature = nullptr;

       friend class GED3D12CommandQueue;

       unsigned getRootParameterIndexOfResource(unsigned id,omegasl_shader &shader);
       D3D12_RESOURCE_STATES getRequiredResourceStateForResourceID(unsigned & id,omegasl_shader &shader);
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
        void finishBlitPass() override;

        #ifdef OMEGAGTE_RAYTRACING_SUPPORTED
        void beginAccelStructPass() override;
        void buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, const GEAccelerationStructDescriptor &desc) override;
        void copyAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, SharedHandle<GEAccelerationStruct> &dest) override;
        void refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,SharedHandle<GEAccelerationStruct> &dest, const GEAccelerationStructDescriptor &desc) override;
        void finishAccelStructPass() override;
        #endif

        void startRenderPass(const GERenderPassDescriptor &desc) override;
        void setVertexBuffer(SharedHandle<GEBuffer> &buffer) override;
        void setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState) override;
        void bindResourceAtVertexShader(SharedHandle<GEBuffer> &buffer, unsigned int id) override;
        void bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned int id) override;
        void bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned int id) override;
        void bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned int id) override;
        void setStencilRef(unsigned int ref) override;
       
        void drawPolygons(RenderPassDrawPolygonType polygonType, unsigned int vertexCount, size_t startIdx) override;
        void setViewports(std::vector<GEViewport> viewports) override;
        void setScissorRects(std::vector<GEScissorRect> scissorRects) override;
        void finishRenderPass() override;

        void startComputePass(const GEComputePassDescriptor &desc) override;
        void setComputePipelineState(SharedHandle<GEComputePipelineState> &pipelineState) override;
        void bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned int id) override;
        void bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int id) override;
        #ifdef OMEGAGTE_RAYTRACING_SUPPORTED
        void bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> &accelStruct, unsigned int id) override;
        void dispatchRays(unsigned int x, unsigned int y, unsigned int z) override;
        #endif
        void dispatchThreads(unsigned int x, unsigned int y, unsigned int z) override;
        void finishComputePass() override;

        GED3D12CommandBuffer(ID3D12GraphicsCommandList6 *commandList,ID3D12CommandAllocator *commandAllocator,GED3D12CommandQueue *parentQueue);
        void reset() override;

        ~GED3D12CommandBuffer() override;
    };

    class GED3D12CommandQueue : public GECommandQueue {
        GED3D12Engine *engine;

        std::vector<ID3D12GraphicsCommandList6 *> commandLists;
        ComPtr<ID3D12CommandQueue> commandQueue;

        ComPtr<ID3D12Fence> fence;

        HANDLE cpuEvent;

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
        ID3D12GraphicsCommandList6 * getLastCommandList();
        void commitToGPU() override;
        void commitToGPUAndWait() override;
        void notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &waitFence) override;
        void submitCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer) override;
        void submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &signalFence) override;
        SharedHandle<GECommandBuffer> getAvailableBuffer() override;
        GED3D12CommandQueue(GED3D12Engine *engine,unsigned size);
        ~GED3D12CommandQueue() override;
    };
_NAMESPACE_END_
#endif
