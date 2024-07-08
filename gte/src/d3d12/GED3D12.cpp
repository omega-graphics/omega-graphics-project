#include "GED3D12.h"
#include "GED3D12CommandQueue.h"
#include "GED3D12Texture.h"
#include "GED3D12RenderTarget.h"
#include "GED3D12Pipeline.h"

#include "../BufferIO.h"

#include <atlstr.h>
#include <cassert>
#include <d3d12.h>
#include <memory>

#include "OmegaGTE.h"


_NAMESPACE_BEGIN_

struct GTED3D12Device : public GTEDevice {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    const void * native() override {
        return (const void *)adapter.Get();
    }
    GTED3D12Device(GTEDevice::Type type,const char *name,GTEDeviceFeatures & features,IDXGIAdapter1 *adapter) : GTEDevice(type,name,features),adapter(adapter){

    };
    ~GTED3D12Device() = default;
};

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
        GTEDeviceFeatures features {false};
        devs.emplace_back(SharedHandle<GTEDevice>(new GTED3D12Device {GTEDevice::Discrete,atlString.GetBuffer(),features,adapter}));
    }

    for(unsigned i = 0;SUCCEEDED(dxgi_factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_MINIMUM_POWER,
                                                                          IID_PPV_ARGS(&adapter)));i++){
        adapter->GetDesc1(&desc);
        CAtlString atlString(desc.Description);
        GTEDeviceFeatures features {false};
        devs.emplace_back(SharedHandle<GTEDevice>(new GTED3D12Device {GTEDevice::Integrated,atlString.GetBuffer(),features,adapter}));
    }

    return devs;
}


//SharedHandle<GEBuffer> GED3D12Heap::makeBuffer(const BufferDescriptor &desc){
//    HRESULT hr;
//    D3D12_RESOURCE_DESC d3d12_desc = CD3DX12_RESOURCE_DESC::Buffer(desc.len);
//    ID3D12Resource *buffer;
//    hr = engine->d3d12_device->CreatePlacedResource(heap.Get(),currentOffset,&d3d12_desc,D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_GENERIC_READ,NULL,IID_PPV_ARGS(&buffer));
//    if(FAILED(hr)){
//        exit(1);
//    };
//
//    D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc;
//    descHeapDesc.NumDescriptors = 1;
//    descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
//    descHeapDesc.NodeMask = engine->d3d12_device->GetNodeCount();
//    descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
//    ID3D12DescriptorHeap *descHeap;
//    hr = engine->d3d12_device->CreateDescriptorHeap(&descHeapDesc,IID_PPV_ARGS(&descHeap));
//    if(FAILED(hr)){
//
//    };
//
//    D3D12_SHADER_RESOURCE_VIEW_DESC res_view_desc;
//    res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
//    res_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
//    res_view_desc.Format = DXGI_FORMAT_UNKNOWN;
//    res_view_desc.Buffer.StructureByteStride = desc.objectStride;
//    res_view_desc.Buffer.FirstElement = 0;
//    res_view_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
//    res_view_desc.Buffer.NumElements = desc.len/desc.objectStride;
//
//    engine->d3d12_device->CreateShaderResourceView(buffer,&res_view_desc,descHeap->GetCPUDescriptorHandleForHeapStart());
//
//    D3D12_RESOURCE_STATES state;
//
//    auto alloc_info = engine->d3d12_device->GetResourceAllocationInfo(engine->d3d12_device->GetNodeCount(),1,&d3d12_desc);
//    currentOffset += alloc_info.SizeInBytes;
//    return SharedHandle<GEBuffer>(new GED3D12Buffer(desc.usage,buffer,descHeap,state));
//};

//SharedHandle<GETexture> GED3D12Heap::makeTexture3D(const TextureDescriptor &desc){
//    HRESULT hr;
//    D3D12_RESOURCE_DESC d3d12_desc;
//    D3D12_RESOURCE_STATES res_states;
//
//    D3D12_SHADER_RESOURCE_VIEW_DESC res_view_desc;
//
//    if(desc.usage & GETexture::RenderTarget){
//        res_states |= D3D12_RESOURCE_STATE_RENDER_TARGET;
//    }
//    else if(desc.usage & GETexture::ToGPU){
//        res_states |= D3D12_RESOURCE_STATE_GENERIC_READ;
//    }
//    else if(desc.usage & GETexture::FromGPU){
//        res_states |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
//    };
//
//        res_view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
//
//
//    D3D12_RENDER_TARGET_VIEW_DESC view_desc;
//
//    view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
//
//    if(desc.type == GETexture::Texture2D){
//        d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM,desc.width,desc.height);
//        res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
//            if(desc.usage & GETexture::RenderTarget){
//                view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
//
//            }
//    }
//    else if(desc.type == GETexture::Texture3D){
//        d3d12_desc = CD3DX12_RESOURCE_DESC::Tex3D(DXGI_FORMAT_R8G8B8A8_UNORM,desc.width,desc.height,desc.depth);
//        res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
//        if(desc.usage & GETexture::RenderTarget){
//                view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
//        }
//    };
//
//    ID3D12Resource *texture;
//    hr = engine->d3d12_device->CreatePlacedResource(heap.Get(),currentOffset,&d3d12_desc,res_states,nullptr,IID_PPV_ARGS(&texture));
//    auto info = engine->d3d12_device->GetResourceAllocationInfo(engine->d3d12_device->GetNodeCount(),1,&d3d12_desc);
//    currentOffset += info.SizeInBytes;
//    if(FAILED(hr)){
//
//    };
//    D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc;
//    descHeapDesc.NumDescriptors = 1;
//    descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
//    descHeapDesc.NodeMask = engine->d3d12_device->GetNodeCount();
//    descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
//    ID3D12DescriptorHeap *descHeap, *rtvDescHeap = nullptr;
//    hr = engine->d3d12_device->CreateDescriptorHeap(&descHeapDesc,IID_PPV_ARGS(&descHeap));
//    if(FAILED(hr)){
//
//    };
//
//    engine->d3d12_device->CreateShaderResourceView(texture,&res_view_desc,descHeap->GetCPUDescriptorHandleForHeapStart());
//
//    if(desc.usage & GETexture::RenderTarget){
//        descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
//
//        hr = engine->d3d12_device->CreateDescriptorHeap(&descHeapDesc,IID_PPV_ARGS(&rtvDescHeap));
//        if(FAILED(hr)){
//
//        };
//
//        engine->d3d12_device->CreateRenderTargetView(texture,&view_desc,rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
//    };
//
//    return std::make_shared<GED3D12Texture>(texture,descHeap,rtvDescHeap);
//
//};

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

        CreateDXGIFactory(IID_PPV_ARGS(&dxgi_factory));

        D3D12GetDebugInterface(IID_PPV_ARGS(&debug_interface));
        debug_interface->EnableDebugLayer();
        debug_interface->SetEnableGPUBasedValidation(true);

        hr = D3D12CreateDevice(device->adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d3d12_device));
        if(FAILED(hr)){
            exit(1);
        };

        DEBUG_STREAM("GED3D12Engine Intialized!");

    };

    SharedHandle<GTEShader> GED3D12Engine::_loadShaderFromDesc(omegasl_shader *shaderDesc,bool runtime) {
        auto shader = new GED3D12Shader();
        shader->internal = *shaderDesc;
        shader->shaderBytecode.pShaderBytecode = shaderDesc->data;
        shader->shaderBytecode.BytecodeLength = shaderDesc->dataSize;
        return SharedHandle<GTEShader>(shader);
    }

    typedef unsigned char D3DByte;

    class GED3D12BufferWriter : public GEBufferWriter {
        GED3D12Buffer * _buffer = nullptr;
        D3DByte *_data_buffer = nullptr;

        bool inStruct=false;
        OmegaCommon::Vector<DataBlock> blocks;
        size_t currentOffset = 0;
    public:
        void setOutputBuffer(SharedHandle<GEBuffer> &buffer) override {
            _buffer = (GED3D12Buffer *)buffer.get();
            CD3DX12_RANGE range(0,0);

            _buffer->buffer->Map(0,&range,(void **)&_data_buffer);
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
        void structEnd() override {
            inStruct = false;
        }

        void sendToBuffer() override {
            assert(!inStruct && "");
            for(auto & block : blocks){
                size_t dataSize = 0;
                if(block.type == OMEGASL_FLOAT){
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
                memcpy(_data_buffer + currentOffset,block.data,dataSize);
                currentOffset += dataSize;
            }
        }

        void flush() override {
            _buffer->buffer->Unmap(0,nullptr);
            _buffer = nullptr;
            _data_buffer = nullptr;
            // std::cout << "LastOffset:" << currentOffset << std::endl;
            currentOffset = 0;
        }
    };

    SharedHandle<GEBufferWriter> GEBufferWriter::Create() {
        return SharedHandle<GEBufferWriter>(new GED3D12BufferWriter());
    }

    class GED3D12BufferReader : public GEBufferReader {
        GED3D12Buffer * _buffer = nullptr;
        D3DByte *_data_buffer = nullptr;
        size_t currentOffset = 0;
    public:
        void setInputBuffer(SharedHandle<GEBuffer> &buffer) override {
            currentOffset = 0;
            _buffer = (GED3D12Buffer *)buffer.get();
            CD3DX12_RANGE range(0,0);

            _buffer->buffer->Map(0,&range,(void **)&_data_buffer);
        }
        void setStructLayout(OmegaCommon::Vector<omegasl_data_type> fields) override {

        }
        void structBegin() override {

        }
        void getFloat(float &v) override {
            memcpy(&v,_data_buffer + currentOffset,sizeof(v));
            currentOffset += sizeof(v);
        }
        void getFloat2(FVec<2> &v) override {
            DirectX::XMFLOAT2 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
        }
        void getFloat3(FVec<3> &v) override {
            DirectX::XMFLOAT3 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
            v[2][0] = _v.z;
        }
        void getFloat4(FVec<4> &v) override {
            DirectX::XMFLOAT4 _v {};
            memcpy(&_v,_data_buffer + currentOffset,sizeof(_v));
            currentOffset += sizeof(_v);
            v[0][0] = _v.x;
            v[1][0] = _v.y;
            v[2][0] = _v.z;
            v[3][0] = _v.w;
        }
        void structEnd() override {

        }
        void reset() override {
            _data_buffer = nullptr;
            currentOffset = 0;
            _buffer->buffer->Unmap(0,nullptr);
            _buffer = nullptr;
        }
    };

    SharedHandle<GEBufferReader> GEBufferReader::Create() {
        return SharedHandle<GEBufferReader>(new GED3D12BufferReader());
    }


    IDXGISwapChain3 *GED3D12Engine::createSwapChainForComposition(DXGI_SWAP_CHAIN_DESC1 *desc,SharedHandle<GECommandQueue> & commandQueue){
        auto *d3d12_queue = (GED3D12CommandQueue *)commandQueue.get();
        IDXGISwapChain1 *swapChain;
        HRESULT hr = dxgi_factory->CreateSwapChainForComposition(d3d12_queue->commandQueue.Get(),desc,nullptr,&swapChain);
        if(FAILED(hr)){
            exit(1);
        };
        IDXGISwapChain3 *lswapChain;
        hr = swapChain->QueryInterface(&lswapChain);
        if(FAILED(hr)){
            exit(1);
        };
        return lswapChain;
    }

    IDXGISwapChain3 *GED3D12Engine::createSwapChainFromHWND(HWND hwnd,DXGI_SWAP_CHAIN_DESC1 *desc,SharedHandle<GECommandQueue> & commandQueue){
        auto *d3d12_queue = (GED3D12CommandQueue *)commandQueue.get();
        IDXGISwapChain1 *swapChain;
        HRESULT hr = dxgi_factory->CreateSwapChainForHwnd(d3d12_queue->commandQueue.Get(),hwnd,desc,NULL,NULL,&swapChain);
        if(FAILED(hr)){
             MessageBoxA(GetForegroundWindow(),"Failed to Create SwapChain.","NOTE",MB_OK);
            exit(1);
        };
        IDXGISwapChain3 *lswapChain;
        hr = swapChain->QueryInterface(&lswapChain);
        if(FAILED(hr)){
            MessageBoxA(GetForegroundWindow(),"Failed to Query SwapChain.","NOTE",MB_OK);
            exit(1);
        };
        return lswapChain;
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
        return nullptr;
    }

    #ifdef OMEGAGTE_RAYTRACING_SUPPORTED

    GED3D12AccelerationStruct::GED3D12AccelerationStruct(SharedHandle<GED3D12Buffer> & structBuffer,
        SharedHandle<GED3D12Buffer> & scratchBuffer):structBuffer(structBuffer),scratchBuffer(scratchBuffer){

    }

    SharedHandle<GEBuffer> GED3D12Engine::createBoundingBoxesBuffer(OmegaCommon::ArrayRef<GERaytracingBoundingBox> boxes){
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
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs {};
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo {};

        d3d12_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs,&prebuildInfo);
        
        auto dataBuffer = std::dynamic_pointer_cast<GED3D12Buffer>(makeBuffer({BufferDescriptor::GPUOnly,prebuildInfo.ResultDataMaxSizeInBytes}));
        auto scratchBuffer = std::dynamic_pointer_cast<GED3D12Buffer>(makeBuffer({BufferDescriptor::GPUOnly,prebuildInfo.ScratchDataSizeInBytes}));
   
        return (SharedHandle<GEAccelerationStruct>)new GED3D12AccelerationStruct(dataBuffer,scratchBuffer);
    }

    #endif

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

    SharedHandle<GERenderPipelineState> GED3D12Engine::makeRenderPipelineState(RenderPipelineDescriptor &desc){
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
            auto el = new D3D12_INPUT_ELEMENT_DESC {"SV_VertexID",0,DXGI_FORMAT_R32_UINT,0,D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
            inputLayoutDesc.NumElements = 1;
            inputLayoutDesc.pInputElementDescs = el;
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
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.NumRenderTargets = 1;
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

        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
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

        MessageBoxA(GetForegroundWindow(),"Create Bytecode Funcs","NOTE",MB_OK);
        ID3D12PipelineState *state;
        hr = d3d12_device->CreateGraphicsPipelineState(&d,IID_PPV_ARGS(&state));
        if(FAILED(hr)){
            MessageBoxA(GetForegroundWindow(),"Failed to Create Pipeline State","NOTE",MB_OK);
            exit(1);
        };
        ATL::CStringW wstr(desc.name.data());
        state->SetName(wstr);
        return SharedHandle<GERenderPipelineState>(new GED3D12RenderPipelineState(desc.vertexFunc,desc.fragmentFunc,state,signature,rootSigDesc));
    };
    SharedHandle<GEComputePipelineState> GED3D12Engine::makeComputePipelineState(ComputePipelineDescriptor &desc){
        D3D12_COMPUTE_PIPELINE_STATE_DESC d;
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
        }
        ATL::CStringW wstr(desc.name.data());
        state->SetName(wstr);
        return SharedHandle<GEComputePipelineState>(new GED3D12ComputePipelineState(desc.computeFunc,state,signature,rootSignatureDesc1));
    };

    SharedHandle<GENativeRenderTarget> GED3D12Engine::makeNativeRenderTarget(const NativeRenderTargetDescriptor &desc){
        HRESULT hr;

        /// Swap Chain must have 2 Frames
        auto rtv_desc_size = d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto dsv_desc_Size = d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        heap_desc.NodeMask = d3d12_device->GetNodeCount();
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 2;
        ID3D12DescriptorHeap *renderTargetHeap;
        hr = d3d12_device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&renderTargetHeap));
        if(FAILED(hr)){
             DEBUG_STREAM("Failed to Create RTV Desc Heap");
            exit(1);
        };

        ID3D12DescriptorHeap *dsvDescHeap = nullptr;

        if(desc.allowDepthStencilTesting){
            heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            hr = d3d12_device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&dsvDescHeap));

            if(FAILED(hr)){
                DEBUG_STREAM("Failed to Create DSV Desc Heap");
                exit(1);
            };
        }

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_cpu_handle (renderTargetHeap->GetCPUDescriptorHandleForHeapStart());
        
        RECT rc;
        if(desc.isHwnd) {
            GetClientRect(desc.hwnd, &rc);
        }
        else {
            rc = RECT {0,0,(LONG)desc.width,(LONG)desc.height};
        }
        DXGI_SWAP_CHAIN_DESC1 swapChaindesc {};
        swapChaindesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // swapChaindesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        swapChaindesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChaindesc.BufferCount = 2;
        swapChaindesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        // swapChaindesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        swapChaindesc.Height = rc.bottom - rc.top;
        swapChaindesc.Width = rc.right - rc.left;
        // swapChaindesc.Scaling = DXGI_SCALING_NONE;
        // swapChaindesc.Stereo = TRUE;
        swapChaindesc.SampleDesc.Count = 1;
         swapChaindesc.SampleDesc.Quality = 0;

        auto commandQueue = makeCommandQueue(64);
        IDXGISwapChain3 *swapChain;
        if(desc.isHwnd) {
            swapChain = createSwapChainFromHWND(desc.hwnd, &swapChaindesc, commandQueue);
        }
        else {
            swapChain = createSwapChainForComposition(&swapChaindesc,commandQueue);
        }
       

        std::vector<ID3D12Resource *> rtvs;

        CD3DX12_CPU_DESCRIPTOR_HANDLE dsv_cpu_handle{};
        if(desc.allowDepthStencilTesting) {
            dsv_cpu_handle = dsvDescHeap->GetCPUDescriptorHandleForHeapStart();
        }

        for(unsigned i = 0;i < 2;i++){
            rtvs.resize(i + 1);
            hr = swapChain->GetBuffer(i,IID_PPV_ARGS(&rtvs[i]));
            if(FAILED(hr)){
                exit(1);
            };
            d3d12_device->CreateRenderTargetView(rtvs[i],nullptr,rtv_cpu_handle);
            rtv_cpu_handle.Offset(1,rtv_desc_size);
            if(desc.allowDepthStencilTesting){
                d3d12_device->CreateDepthStencilView(rtvs[i],nullptr,dsv_cpu_handle);
                dsv_cpu_handle.Offset(1,dsv_desc_Size);
            }
        };

        

        return SharedHandle<GENativeRenderTarget>(new GED3D12NativeRenderTarget(swapChain,renderTargetHeap,dsvDescHeap,std::move(commandQueue),swapChain->GetCurrentBackBufferIndex(),rtvs.data(),rtvs.size(),desc.hwnd));
    };

    SharedHandle<GETextureRenderTarget> GED3D12Engine::makeTextureRenderTarget(const TextureRenderTargetDescriptor &desc){
        SharedHandle<GETexture> texture;
        if(desc.renderToExistingTexture){
            texture = desc.texture;
        }
        else {
            TextureDescriptor targetDesc {};
            targetDesc.usage = GETexture::RenderTarget;
            targetDesc.type = GETexture::Texture2D;
            targetDesc.width = desc.region.w;
            targetDesc.height = desc.region.h;
            texture = makeTexture(targetDesc);
        }
        auto commandQueue = makeCommandQueue(64);
        return SharedHandle<GETextureRenderTarget>(new GED3D12TextureRenderTarget(std::dynamic_pointer_cast<GED3D12Texture>(texture),commandQueue));
    };

    SharedHandle<GECommandQueue> GED3D12Engine::makeCommandQueue(unsigned int maxBufferCount){
        return SharedHandle<GECommandQueue>(new GED3D12CommandQueue(this,maxBufferCount));
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

        if(desc.usage == GETexture::RenderTargetAndDepthStencil && desc.type == GETexture::Texture3D){
            DEBUG_STREAM("Cannot create a 3D Texture with Depth Stencil Properties");
            return nullptr;
        }

        DXGI_FORMAT dxgiFormat;
        switch (desc.pixelFormat) {
            case TexturePixelFormat::RGBA8Unorm : {
                dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
                break;
            }
            case TexturePixelFormat::RGBA16Unorm : {
                dxgiFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
                break;
            }
            case TexturePixelFormat::RGBA8Unorm_SRGB : {
                dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                break;
            }
        }

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
            res_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            res_view_desc.Format = dxgiFormat;
        }

        if(isDSV){
            dsv_view_desc.Format = dxgiFormat;
            dsv_view_desc.Flags = D3D12_DSV_FLAG_NONE;
        }

        D3D12_RENDER_TARGET_VIEW_DESC view_desc {};
        if(desc.type == GETexture::Texture1D){
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex1D(dxgiFormat,desc.width,1,desc.mipLevels);
            if(isUAV) {
                uav_view_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                uav_view_desc.Texture1D.MipSlice = desc.mipLevels;
            }

            if(isSRV) {
                res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                res_view_desc.Texture1D.MipLevels = desc.mipLevels;
            }

            if(isDSV){
                dsv_view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
                dsv_view_desc.Texture1D.MipSlice = desc.mipLevels;
            }

        }
        else if(desc.type == GETexture::Texture2D){
            d3d12_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,desc.width,desc.height,1,desc.mipLevels,desc.sampleCount);
            if(isUAV){
                uav_view_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav_view_desc.Texture2D.MipSlice = desc.mipLevels - 1;
                uav_view_desc.Texture2D.PlaneSlice = 0;
            }

            if(isSRV) {

                if (desc.sampleCount > 1) {
                    res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                } else {
                    res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                }

                
                res_view_desc.Texture2D.MipLevels = desc.mipLevels;
                res_view_desc.Texture2D.MostDetailedMip = desc.mipLevels - 1;
                res_view_desc.Texture2D.PlaneSlice = 0;
                res_view_desc.Texture2D.ResourceMinLODClamp = desc.mipLevels - 1;
            }

            if(isDSV){
                if (desc.sampleCount > 1) {
                    dsv_view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
                } else {
                    dsv_view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                }
                dsv_view_desc.Texture2D.MipSlice = desc.mipLevels - 1;
            }


             if(desc.usage & GETexture::RenderTarget){
                 if(desc.sampleCount > 1) {
                     view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
                 }
                 else {
                     view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                 }
                  view_desc.Format = dxgiFormat;
                  view_desc.Texture2D.PlaneSlice = 0;
                  view_desc.Texture2D.MipSlice = 0;
             }
        }
        else if(desc.type == GETexture::Texture3D){
           d3d12_desc = CD3DX12_RESOURCE_DESC::Tex3D(dxgiFormat,desc.width,desc.height,desc.depth,desc.mipLevels);
           d3d12_desc.SampleDesc.Count = desc.sampleCount;
           d3d12_desc.DepthOrArraySize = 1;

           if(isUAV){
                uav_view_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                uav_view_desc.Texture3D.MipSlice = desc.mipLevels - 1;
                uav_view_desc.Texture3D.FirstWSlice = 0;
                uav_view_desc.Texture3D.WSize = desc.depth;
           }

           if(isSRV){
               res_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
               res_view_desc.Texture3D.MipLevels = desc.mipLevels;
           }

           if(desc.usage == GETexture::RenderTarget){
                 view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                 view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                 view_desc.Texture3D.MipSlice = 0;
                 view_desc.Texture3D.FirstWSlice = 0;
                 view_desc.Texture3D.WSize = desc.depth;
            }
        };
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
            d3d12_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        }
        else if(desc.usage == GETexture::RenderTarget){
            heap_prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            
            d3d12_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }

        /// Create Texture Resource

        auto textureHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

        D3D12_RESOURCE_STATES states = res_states;


        ID3D12Resource *texture,*cpuSideRes = nullptr;

        hr = d3d12_device->CreateCommittedResource(&textureHeapProps, D3D12_HEAP_FLAG_SHARED,&d3d12_desc,states,nullptr,IID_PPV_ARGS(&texture));
        if(FAILED(hr)){
            DEBUG_STREAM("Failed to make D3D12TextureResource");
        };


        /// Create GPU/CPU Transition Heap
        if(desc.usage != GETexture::GPUAccessOnly){
            auto size = GetRequiredIntermediateSize(texture,0,1);
            auto res = CD3DX12_RESOURCE_DESC::Buffer(size);
            hr = d3d12_device->CreateCommittedResource(&heap_prop,D3D12_HEAP_FLAG_NONE,&res,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&cpuSideRes));
            if(FAILED(hr)){
                DEBUG_STREAM("Failed to make D3D12TextureResource Transition Heap");
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

        if(isDSV && desc.type != GETexture::Texture3D){
            descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

            hr = d3d12_device->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&dsvDescHeap));
            if(FAILED(hr)){
                DEBUG_STREAM("Failed to Create DSV Desc Heap");
            }

            d3d12_device->CreateDepthStencilView(texture,&dsv_view_desc,dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
        }

        DEBUG_STREAM("Will Return Texture");

        return SharedHandle<GETexture>(new GED3D12Texture(desc.type,desc.usage,desc.pixelFormat,texture,cpuSideRes,descHeap,uavDescHeap,rtvDescHeap,dsvDescHeap,res_states));
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
        D3D12_RESOURCE_DESC d3d12_desc = CD3DX12_RESOURCE_DESC::Buffer(desc.len,flags);
        auto heap_prop = CD3DX12_HEAP_PROPERTIES( heap_type );
        hr = d3d12_device->CreateCommittedResource(
            &heap_prop,D3D12_HEAP_FLAG_NONE,
            &d3d12_desc,
            state,
            nullptr,IID_PPV_ARGS(&buffer));

        if(FAILED(hr)){
            ///
            DEBUG_STREAM("Failed to Create D3D12 Buffer");
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

        if(desc.usage == BufferDescriptor::Upload) {

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

        return SharedHandle<GEBuffer>(new GED3D12Buffer(desc.usage,buffer,descHeap,state));
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
                if(l.type == OMEGASL_SHADER_SAMPLER2D_DESC || l.type == OMEGASL_SHADER_SAMPLER3D_DESC){
                    CD3DX12_DESCRIPTOR_RANGE1 desc_table;
                    desc_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,1,l.gpu_relative_loc);
                    parameter1.InitAsDescriptorTable(1,&desc_table);
                }
                else if(l.type == OMEGASL_SHADER_STATIC_SAMPLER2D_DESC || l.type == OMEGASL_SHADER_STATIC_SAMPLER3D_DESC){
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

                    range->Init(rangeType,1,l.gpu_relative_loc,registerSpace);
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