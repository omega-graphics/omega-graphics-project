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

#include <wrl.h>

#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"dxguid.lib")
#pragma comment(lib,"runtimeobject.lib")
#pragma comment(lib,"d3dcompiler.lib")

#ifndef OMEGAGTE_GED3D12_H
#define OMEGAGTE_GED3D12_H

_NAMESPACE_BEGIN_
    using Microsoft::WRL::ComPtr;

    class GED3D12Buffer : public GEBuffer {
    public:

        ComPtr<ID3D12Resource> buffer;
        ComPtr<ID3D12DescriptorHeap> bufferDescHeap;

        D3D12_RESOURCE_STATES currentState;

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
        GED3D12Buffer(const BufferDescriptor::Usage & usage,ID3D12Resource *buffer,ID3D12DescriptorHeap *bufferDescHeap, D3D12_RESOURCE_STATES currentState):
        GEBuffer(usage),buffer(buffer),
        bufferDescHeap(bufferDescHeap),
        currentState(currentState){
            
        };
    };

    class GED3D12Fence : public GEFence {
    public:
        ComPtr<ID3D12Fence> fence;
        void setName(OmegaCommon::StrRef name) override{
            ATL::CStringW str(name.data(),name.size());
            fence->SetName(str);
        }
        void *native() override {
            return fence.Get();
        };
        explicit GED3D12Fence(ID3D12Fence *fence):fence(fence){};
    };

    class GED3D12SamplerState : public GESamplerState {
    public:
        ComPtr<ID3D12DescriptorHeap> descHeap;
        D3D12_SAMPLER_DESC staticSampler;
        explicit GED3D12SamplerState(ID3D12DescriptorHeap *descriptorHeap,D3D12_SAMPLER_DESC & samplerDesc):descHeap(descriptorHeap),staticSampler(samplerDesc){

        };
    };

    class GED3D12Engine;
    struct GTED3D12Device;

//    class GED3D12Heap : public GEHeap {
//        GED3D12Engine *engine;
//        ComPtr<ID3D12Heap> heap;
//        size_t currentOffset;
//    public:
//        GED3D12Heap(GED3D12Engine *engine,ID3D12Heap * heap):engine(engine),heap(heap),currentOffset(0){};
//        size_t currentSize() override{
//            return heap->GetDesc().SizeInBytes;
//        };
//        SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor &desc) override;
//        SharedHandle<GETexture> makeTexture3D(const TextureDescriptor &desc) override;
//    };
    #ifdef OMEGAGTE_RAYTRACING_SUPPORTED
    struct GED3D12AccelerationStruct : public GEAccelerationStruct {
        SharedHandle<GED3D12Buffer> structBuffer;
        SharedHandle<GED3D12Buffer> scratchBuffer;
        explicit GED3D12AccelerationStruct(
            SharedHandle<GED3D12Buffer> & structBuffer,
            SharedHandle<GED3D12Buffer> & scratchBuffer);
    };
    #endif

    class GED3D12Engine : public OmegaGraphicsEngine {
        SharedHandle<GTEShader> _loadShaderFromDesc(omegasl_shader *shaderDesc,bool runtime) override;
    public:
        ComPtr<IDXGIFactory6> dxgi_factory;
        explicit GED3D12Engine(SharedHandle<GTED3D12Device> device);
        ComPtr<ID3D12Debug1> debug_interface;
        ComPtr<ID3D12Device8> d3d12_device;
        void * underlyingNativeDevice() override {
            return d3d12_device.Get();
        }
        // ComPtr<ID3D12DescriptorHeap> descriptorHeapForRes;
        static SharedHandle<OmegaGraphicsEngine> Create(SharedHandle<GTEDevice> & device);
        // SharedHandle<GEShaderLibrary> loadShaderLibrary(FS::Path path);
        // SharedHandle<GEShaderLibrary> loadStdShaderLibrary();
        #ifdef OMEGAGTE_RAYTRACING_SUPPORTED
        SharedHandle<GEBuffer> createBoundingBoxesBuffer(OmegaCommon::ArrayRef<GERaytracingBoundingBox> boxes) override;
        SharedHandle<GEAccelerationStruct> allocateAccelerationStructure(const GEAccelerationStructDescriptor &desc) override;
        #endif
        bool createRootSignatureFromOmegaSLShaders(unsigned shaderN,omegasl_shader *shader,D3D12_ROOT_SIGNATURE_DESC1 * rootSignatureDesc,ID3D12RootSignature **pRootSignature);
        SharedHandle<GEFence> makeFence() override;
        SharedHandle<GESamplerState> makeSamplerState(const SamplerDescriptor &desc) override;
        SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor &desc)  override;
        SharedHandle<GEHeap> makeHeap(const HeapDescriptor &desc)  override;
        SharedHandle<GECommandQueue> makeCommandQueue(unsigned int maxBufferCount)  override;
        SharedHandle<GETexture> makeTexture(const TextureDescriptor &desc)  override;
        SharedHandle<GERenderPipelineState> makeRenderPipelineState(RenderPipelineDescriptor &desc)  override;
        SharedHandle<GEComputePipelineState> makeComputePipelineState(ComputePipelineDescriptor &desc)  override;
        SharedHandle<GENativeRenderTarget> makeNativeRenderTarget(const NativeRenderTargetDescriptor &desc)  override;
        SharedHandle<GETextureRenderTarget> makeTextureRenderTarget(const TextureRenderTargetDescriptor &desc)  override;
        IDXGISwapChain3 *createSwapChainForComposition(DXGI_SWAP_CHAIN_DESC1 *desc,SharedHandle<GECommandQueue> & commandQueue);
        IDXGISwapChain3 *createSwapChainFromHWND(HWND hwnd,DXGI_SWAP_CHAIN_DESC1 *desc,SharedHandle<GECommandQueue> & commandQueue);
    };
_NAMESPACE_END_
#endif




