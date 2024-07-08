#import "GEMetal.h"
#include <sstream>
#include <memory>
#import "GEMetalCommandQueue.h"
#import "GEMetalTexture.h"
#import "GEMetalRenderTarget.h"
#import "GEMetalPipeline.h"
#include <cassert>
#include <fstream>
#include <cstdlib>
#include <cstring>

#include "OmegaGTE.h"

#include "../BufferIO.h"

#import <Metal/Metal.h>
#include <simd/simd.h>

#if !__has_attribute(ext_vector_type)
#pragma error("Requires vector types")
#endif

_NAMESPACE_BEGIN_

    struct GTEMetalDevice : public GTEDevice {
        __strong id<MTLDevice> device;
        GTEMetalDevice(Type type,const char *name,GTEDeviceFeatures & features,id<MTLDevice> _device): GTEDevice(type,name,features),device(_device){}
        const void * native() override {
            return device;
        }
        ~GTEMetalDevice() = default;
    };

    /// GTE Device Enumerate
    OmegaCommon::Vector<SharedHandle<GTEDevice>> enumerateDevices(){
        OmegaCommon::Vector<SharedHandle<GTEDevice>> devs;
        NSArray<id<MTLDevice>> *mtlDevices = MTLCopyAllDevices();

        for(id<MTLDevice> dev in mtlDevices){
            GTEDeviceFeatures features {
                    (bool)dev.supportsRaytracing,
                    (bool)[dev supportsTextureSampleCount:4],
                    (bool)[dev supportsTextureSampleCount:8]
            };
            GTEDevice::Type type;
            if(dev.lowPower){
                type = GTEDevice::Integrated;
            }
            else {
                type = GTEDevice::Discrete;
            }
            devs.push_back(SharedHandle<GTEDevice>(new GTEMetalDevice {type,dev.name.UTF8String,features,dev}));
        }
        return devs;
    }



    /// =========================================================>
    

    NSSmartPtr::NSSmartPtr(const NSObjectHandle & handle):data(handle.data){

    };

    void NSSmartPtr::assertExists(){
        assert(data);
    };

    GEMetalBuffer::GEMetalBuffer(const BufferDescriptor::Usage & usage,
                                 NSSmartPtr & buffer,
                                 NSSmartPtr &layoutDesc):
            GEBuffer(usage),
            metalBuffer(buffer),
            layoutDesc(layoutDesc){

        id<MTLDevice> device = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,buffer.handle()).device;
        resourceBarrier = NSObjectHandle {NSOBJECT_CPP_BRIDGE [device newFence]};
    };
    
    size_t GEMetalBuffer::size(){
        metalBuffer.assertExists();
        return NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer.handle()).length;
    };

    void GEMetalBuffer::setName(OmegaCommon::StrRef name) {
        NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer.handle()).label = [[NSString alloc] initWithUTF8String:name.data()];
    }

    GEMetalBuffer::~GEMetalBuffer(){

    };

    #ifdef OMEGAGTE_RAYTRACING_SUPPORTED

    GEMetalAccelerationStruct::GEMetalAccelerationStruct(NSSmartPtr & accelStruct,
    SharedHandle<GEMetalBuffer> & scratchBuffer):accelStruct(accelStruct),scratchBuffer(scratchBuffer){

    }

    #endif

    /// @brief Metal Buffer Reader/Writer.

    typedef unsigned char MTLByte;

    class GEMetalBufferWriter : public GEBufferWriter {
        GEMetalBuffer *buffer_ = nil;
        MTLByte *_data_ptr = nullptr;
        size_t currentOffset = 0;
        bool inStruct = false;
        
        OmegaCommon::Vector<DataBlock> blocks;
        inline void clearBlocks(){
            for(auto & b : blocks){
                switch (b.type) {
                    case OMEGASL_FLOAT : {
                        delete (float *)b.data;
                        break;
                    }
                    case OMEGASL_FLOAT2 : {
                        delete (simd_float2 *)b.data;
                        break;
                    }
                    case OMEGASL_FLOAT3 : {
                        delete (simd_float3 *)b.data;
                        break;
                    }
                    case OMEGASL_FLOAT4 : {
                        delete (simd_float4 *)b.data;
                        break;
                    }
                }
            }
            blocks.clear();
        }
    public:
        GEMetalBufferWriter() = default;
        void setOutputBuffer(SharedHandle<GEBuffer> &buffer) override {
            buffer_= (GEMetalBuffer *)buffer.get();
            currentOffset = 0;
            _data_ptr = (MTLByte *)[NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,buffer_->metalBuffer.handle()) contents];;
        }
        void structBegin() override {
            if(!blocks.empty()){
                clearBlocks();
            }
            inStruct = true;
        }
        void writeFloat(float &v) override {
            DataBlock block {OMEGASL_FLOAT,new float(v)};
            blocks.push_back(block);
        }
        void writeFloat2(FVec<2> &v) override {
            DataBlock block {OMEGASL_FLOAT2,new simd_float2(simd_make_float2(v[0][0],v[1][0]))};
            blocks.push_back(block);
        }
        void writeFloat3(FVec<3> &v) override {
            DataBlock block {OMEGASL_FLOAT3,new simd_float3(simd_make_float3(v[0][0],v[1][0],v[2][0]))};
            blocks.push_back(block);
        }
        void writeFloat4(FVec<4> &v) override {
            DataBlock block {OMEGASL_FLOAT4,new simd_float4(simd_make_float4(v[0][0],v[1][0],v[2][0],v[3][0]))};
            blocks.push_back(block);
        }
        void structEnd() override {
            inStruct = false;
        }
        void sendToBuffer() override {
            assert(!inStruct && "Struct must be finished be written to before sending data");
            size_t biggestSize = 1;

            OmegaCommon::Vector<size_t> sizes;
            for(auto & d : blocks){
                size_t s = 0;
                switch (d.type) {
                    case OMEGASL_FLOAT : {
                        s = sizeof(float);
                        break;
                    }
                    case OMEGASL_FLOAT2 : {
                        s = sizeof(simd_float2);
                        break;
                    }
                    case OMEGASL_FLOAT3 : {
                        s = sizeof(simd_float3);
                        break;
                    }
                    case OMEGASL_FLOAT4 : {
                        s = sizeof(simd_float4);
                        break;
                    }
                }
                if(s > biggestSize){
                    biggestSize = s;
                }
                sizes.push_back(s);
            }

            size_t offsetBefore = 0;
            size_t offsetAfter = 0;

            bool afterBiggestWord = false;
            for(unsigned i = 0;i < blocks.size();i++){
                auto & s = sizes[i];
                auto & block = blocks[i];
                if(afterBiggestWord){
                    memcpy(_data_ptr + currentOffset + offsetAfter,block.data,s);
                    offsetAfter += s;
                }
                else {
                    if (s == biggestSize) {
                        auto padding = offsetBefore % biggestSize;

                        if(padding > 0) {
                            auto *pad = new MTLByte[padding];

                            size_t paddingLen = padding;
                            auto *pad_it = pad;
                            while (paddingLen > 0) {
                                *pad_it = 0;
                                ++pad_it;
                                --paddingLen;
                            }
                            memcpy(_data_ptr + currentOffset + offsetBefore,pad,padding);
                            offsetBefore += padding;
                            delete [] pad;
                        }

                        memcpy(_data_ptr + currentOffset + offsetBefore,block.data,s);
                        afterBiggestWord = true;
                        currentOffset += (offsetBefore + s);
                    }
                    else {
                        memcpy(_data_ptr + currentOffset + offsetBefore,block.data,s);
                        offsetBefore += s;
                    }
                }
            }

            if(offsetAfter > 0){
                auto padding = offsetAfter % biggestSize;

                if(padding > 0) {
                    auto *pad = new MTLByte[padding];

                    size_t paddingLen = padding;
                    auto *pad_it = pad;
                    while (paddingLen > 0) {
                        *pad_it = 0;
                        ++pad_it;
                        --paddingLen;
                    }
                    memcpy(_data_ptr + currentOffset + offsetAfter,pad,padding);
                    offsetAfter += padding;
                    delete [] pad;
                }
                currentOffset += offsetAfter;
            }
        }
        void flush() override {
            currentOffset = 0;
            buffer_ = nil;
            _data_ptr = nullptr;
            clearBlocks();
        }
    };

    SharedHandle<GEBufferWriter> GEBufferWriter::Create() {
        return std::shared_ptr<GEBufferWriter>(new GEMetalBufferWriter());
    }

    class GEMetalBufferReader : public GEBufferReader {
        GEMetalBuffer *buffer_ = nullptr;
        MTLByte *_data_ptr = nullptr;
        size_t currentOffset = 0;

        size_t structRelativeOffset = 0;

        size_t biggestSizeOffset = 0;

        size_t biggestSize = 0,paddingBefore = 0,paddingAfter = 0;

        size_t fieldIndex = 0;

        bool inStruct = false;

        OmegaCommon::Vector<omegasl_data_type> structLayout;
        inline void readBeforePaddingIfPossible(){
            if(structRelativeOffset == biggestSizeOffset){
                currentOffset += paddingBefore;
                structRelativeOffset += paddingBefore;
            }
        }
        inline void readAfterPaddingIfPossible(){
            if(structLayout.size() == (fieldIndex + 1)){
                currentOffset += paddingAfter;
                structRelativeOffset += paddingAfter;
            }
        }
        inline void offsetAndIncrement(size_t n){
            structRelativeOffset += n;
            currentOffset += n;
            ++fieldIndex;
        }
    public:
        GEMetalBufferReader() = default;
        void setInputBuffer(SharedHandle<GEBuffer> &buffer) override {
            currentOffset = 0;
            buffer_= (GEMetalBuffer *)buffer.get();
            _data_ptr = (MTLByte *)[NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,buffer_->metalBuffer.handle()) contents];
        }
        void setStructLayout(OmegaCommon::Vector<omegasl_data_type> fields) override {
            structLayout = fields;
            OmegaCommon::Vector<size_t> sizes;
            for(auto & f : fields){
                size_t s = 0;
                switch (f) {
                    case OMEGASL_INT:
                    case OMEGASL_FLOAT : {
                        s = sizeof(float);
                        break;
                    }
                    case OMEGASL_INT2 :
                    case OMEGASL_FLOAT2 : {
                        s = sizeof(simd_float2);
                        break;
                    }
                    case OMEGASL_INT3 :
                    case OMEGASL_FLOAT3 : {
                        s = sizeof(simd_float3);
                        break;
                    }
                    case OMEGASL_INT4 :
                    case OMEGASL_FLOAT4 :
                    {
                        s = sizeof(simd_float4);
                        break;
                    }
                }
                if(s > biggestSize){
                    s = biggestSize;
                }
                sizes.push_back(s);
            }

            bool afterBiggest = false;

            size_t relOffsetBeg = 0,relOffsetEnd = 0;

            for(auto & s : sizes){
                if(afterBiggest){
                    relOffsetEnd += s;
                }
                else {
                    if (s == biggestSize) {
                        paddingBefore = relOffsetBeg % biggestSize;
                        biggestSizeOffset = relOffsetBeg;
                       afterBiggest = true;
                    } else {
                        relOffsetBeg += s;
                    }
                }
            }
            paddingAfter = relOffsetEnd % biggestSize;
        }
        void structBegin() override {
            inStruct = true;
            structRelativeOffset = 0;
            fieldIndex = 0;
        }
        void getFloat(float &v) override {
            assert(structLayout[fieldIndex] == OMEGASL_FLOAT && "Field is not a type of float");
            readBeforePaddingIfPossible();

            memcpy(&v,_data_ptr + currentOffset,sizeof(v));
            offsetAndIncrement(sizeof(v));
            readAfterPaddingIfPossible();
        }
        void getFloat2(FVec<2> &v) override {
            assert(structLayout[fieldIndex] == OMEGASL_FLOAT2 && "Field is not a type of float2");
            readBeforePaddingIfPossible();

            simd_float2 _v;
            memcpy(&_v,_data_ptr + currentOffset,sizeof(_v));
            offsetAndIncrement(sizeof(_v));
            
            v[0][0] = _v.x;
            v[1][0] = _v.y;
            readAfterPaddingIfPossible();
        }
        void getFloat3(FVec<3> &v) override {
            assert(structLayout[fieldIndex] == OMEGASL_FLOAT3 && "Field is not a type of float3");
            readBeforePaddingIfPossible();

            simd_float3 _v;
            memcpy(&_v,_data_ptr + currentOffset,sizeof(_v));
            offsetAndIncrement(sizeof(_v));

            v[0][0] = _v.x;
            v[1][0] = _v.y;
            v[2][0] = _v.z;
            readAfterPaddingIfPossible();
        }
        void getFloat4(FVec<4> &v) override {
            assert(structLayout[fieldIndex] == OMEGASL_FLOAT4 && "Field is not a type of float4");
            readBeforePaddingIfPossible();


            simd_float4 _v;
            memcpy(&_v,_data_ptr + currentOffset,sizeof(_v));
            offsetAndIncrement(sizeof(_v));

            v[0][0] = _v.x;
            v[1][0] = _v.y;
            v[2][0] = _v.z;
            v[3][0] = _v.w;
            readAfterPaddingIfPossible();
        }
        void structEnd() override {
            inStruct = false;
        }
        void reset() override {
            _data_ptr = nullptr;
            buffer_ = nullptr;
            structRelativeOffset = 0;
        }
    };

    SharedHandle<GEBufferReader> GEBufferReader::Create() {
        return std::shared_ptr<GEBufferReader>(new GEMetalBufferReader());
    }



    GEMetalFence::GEMetalFence(NSSmartPtr & event):metalEvent(event){

    };

    void GEMetalFence::setName(OmegaCommon::StrRef name) {
        [NSOBJECT_OBJC_BRIDGE(id<MTLEvent>,metalEvent.handle()) setLabel:[NSString stringWithUTF8String:name.data()]];
    }

    GEMetalSamplerState::GEMetalSamplerState(NSSmartPtr &samplerState): samplerState(samplerState) {

    }

    inline MTLCompareFunction convertCompareFunc(CompareFunc & compareFunc){
        MTLCompareFunction res;
        switch (compareFunc) {
            case CompareFunc::Less : {
                res = MTLCompareFunctionLess;
                break;
            }
            case CompareFunc::LessEqual : {
                res = MTLCompareFunctionLessEqual;
                break;
            }
            case CompareFunc::Greater : {
                res = MTLCompareFunctionGreater;
                break;
            }
            case CompareFunc::GreaterEqual : {
                res = MTLCompareFunctionGreaterEqual;
                break;
            }
        }
        return res;
    }

    inline MTLStencilOperation convertStencilOp(StencilOperation & op){
        MTLStencilOperation res;
        switch (op) {
            case StencilOperation::Retain : {
                res = MTLStencilOperationKeep;
                break;
            }
            case StencilOperation::Replace : {
                res = MTLStencilOperationReplace;
                break;
            }
            case StencilOperation::IncrementWrap : {
                res = MTLStencilOperationIncrementWrap;
                break;
            }
            case StencilOperation::DecrementWrap : {
                res = MTLStencilOperationDecrementWrap;
                break;
            }
            case StencilOperation::Zero : {
                res = MTLStencilOperationZero;
                break;
            }
        }
        return res;
    }

    class GEMetalEngine : public OmegaGraphicsEngine {
        NSSmartPtr metalDevice;
        SharedHandle<GTEShader> _loadShaderFromDesc(omegasl_shader *shaderDesc,bool runtime) override {
            NSSmartPtr library;
            NSString *str = [[NSString alloc] initWithUTF8String:shaderDesc->name];
            NSLog(@"Loading Function %@",str);
            if(runtime){
                /// Field `data` is an id<MTLLibrary>
                library = NSObjectHandle {shaderDesc->data};
            }
            else {
                auto data = dispatch_data_create(shaderDesc->data,shaderDesc->dataSize,nullptr,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                NSError *error;
                library = NSObjectHandle {NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newLibraryWithData:data error:&error]};
                dispatch_release(data);

            }
            NSSmartPtr func = NSObjectHandle {NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLLibrary>,library.handle()) newFunctionWithName:str] };
            func.assertExists();


            auto _shader = new GEMetalShader(library,func);
            _shader->internal = *shaderDesc;
            return SharedHandle<GTEShader>(_shader);
        }
    public:
        void * underlyingNativeDevice() override {
            return const_cast<void *>(metalDevice.handle());
        }
        GEMetalEngine(SharedHandle<GTEDevice> & __device){
            __strong id<MTLDevice> device = ((GTEMetalDevice *)__device.get())->device;
            if(device == nil){
                NSLog(@"Metal is not supported on this device! Exiting...");
                exit(1);
            }
            MTLCaptureManager *manager = [MTLCaptureManager sharedCaptureManager];

            MTLCaptureDescriptor *captureDesc = [[MTLCaptureDescriptor alloc] init];
            captureDesc.captureObject = device;
            captureDesc.destination = MTLCaptureDestinationGPUTraceDocument;
            captureDesc.outputURL = [NSURL fileURLWithPath:@"./Trace.gputrace"];
            NSError *error;
            BOOL res = [manager startCaptureWithDescriptor:captureDesc error:&error];

            if(!res){
                NSLog(@"Failed to Start GPU Capture. %@",error);
            }

            metalDevice = NSObjectHandle {NSOBJECT_CPP_BRIDGE device};
            DEBUG_STREAM("GEMetalEngine Successfully Created");
            
        };

        #ifdef OMEGAGTE_RAYTRACING_SUPPORTED

        SharedHandle<GEBuffer> createBoundingBoxesBuffer(OmegaCommon::ArrayRef<GERaytracingBoundingBox> boxes) override {
            auto buffer = std::dynamic_pointer_cast<GEMetalBuffer>(makeBuffer({BufferDescriptor::Upload,sizeof(MTLAxisAlignedBoundingBox) * boxes.size(),sizeof(MTLAxisAlignedBoundingBox)}));
            std::vector<MTLAxisAlignedBoundingBox> bb;
            for(auto & box : boxes){
                MTLAxisAlignedBoundingBox boundingBox;
                boundingBox.min = MTLPackedFloat3Make(box.minX,box.minY,box.minZ);
                boundingBox.max = MTLPackedFloat3Make(box.maxX,box.maxY,box.maxZ);
                bb.push_back(boundingBox);
            }
            void *dataPtr = [NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,buffer->metalBuffer.handle()) contents];
            memmove(dataPtr,bb.data(),bb.size() * sizeof(MTLAxisAlignedBoundingBox));
            return std::static_pointer_cast<GEBuffer>(buffer);
        }

        SharedHandle<GEAccelerationStruct> allocateAccelerationStructure(const GEAccelerationStructDescriptor &desc) override {
            MTLPrimitiveAccelerationStructureDescriptor *d = [[MTLPrimitiveAccelerationStructureDescriptor alloc]init];
            auto sizes = [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) accelerationStructureSizesWithDescriptor:d];
            NSSmartPtr handle = NSObjectHandle{NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newAccelerationStructureWithSize:sizes.accelerationStructureSize]};
            auto buffer = std::dynamic_pointer_cast<GEMetalBuffer>(makeBuffer({BufferDescriptor::GPUOnly,sizes.buildScratchBufferSize}));
            return (SharedHandle<GEAccelerationStruct>)new GEMetalAccelerationStruct (handle,buffer);
        }
        
        #endif

        SharedHandle<GECommandQueue> makeCommandQueue(unsigned int maxBufferCount) override{
            metalDevice.assertExists();
            NSSmartPtr commandQueue ({NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newCommandQueueWithMaxCommandBufferCount:maxBufferCount]});
            return std::shared_ptr<GECommandQueue>(new GEMetalCommandQueue(commandQueue,maxBufferCount));
        };
        SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor &desc) override{
            metalDevice.assertExists();
            MTLBufferLayoutDescriptor *descriptor = [[MTLBufferLayoutDescriptor alloc] init];
            descriptor.stride = desc.objectStride;
            /// Only defines object stride.
            id<MTLBuffer> mtlBuffer = [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newBufferWithLength:desc.len options:MTLResourceStorageModeShared];

            NSSmartPtr buffer ({NSOBJECT_CPP_BRIDGE mtlBuffer}),
            layoutDesc(NSObjectHandle {NSOBJECT_CPP_BRIDGE descriptor});
            return std::shared_ptr<GEBuffer>(new GEMetalBuffer(desc.usage,buffer,layoutDesc));
        };
        SharedHandle<GEComputePipelineState> makeComputePipelineState(ComputePipelineDescriptor &desc) override{
            metalDevice.assertExists();
//
//
//
            auto & threadgroup_desc = desc.computeFunc->internal.threadgroupDesc;

            MTLComputePipelineDescriptor *pipelineDescriptor = [[MTLComputePipelineDescriptor alloc] init];
            if(desc.name.size() > 0) {
                pipelineDescriptor.label = [[NSString alloc] initWithUTF8String:desc.name.data()];
            }
            pipelineDescriptor.maxTotalThreadsPerThreadgroup = (threadgroup_desc.x * threadgroup_desc.y * threadgroup_desc.z);


            GEMetalShader *computeShader = (GEMetalShader *)desc.computeFunc.get();
            computeShader->function.assertExists();

            pipelineDescriptor.computeFunction = NSOBJECT_OBJC_BRIDGE(id<MTLFunction>,computeShader->function.handle());


            NSError *error;
            NSSmartPtr pipelineState =  NSObjectHandle{NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newComputePipelineStateWithDescriptor:pipelineDescriptor options:MTLPipelineOptionNone reflection:nil error:&error]};

            if(pipelineState.handle() == nil){
                DEBUG_STREAM("Failed to Create Compute Pipeline State");
                exit(1);
            };

            return std::shared_ptr<GEComputePipelineState>(new GEMetalComputePipelineState(desc.computeFunc,pipelineState));
        };
        SharedHandle<GEFence> makeFence() override{
             NSSmartPtr fence = NSObjectHandle {NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newEvent]};

             return SharedHandle<GEFence>(new GEMetalFence(fence));
        };
        SharedHandle<GEHeap> makeHeap(const HeapDescriptor &desc) override{
            MTLHeapDescriptor *heapDesc = [[MTLHeapDescriptor alloc] init];
            return nullptr;
        };
        SharedHandle<GENativeRenderTarget> makeNativeRenderTarget(const NativeRenderTargetDescriptor &desc) override{
            metalDevice.assertExists();
            desc.metalLayer.device = NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle());
            auto commandQueue = makeCommandQueue(64);
            return std::shared_ptr<GENativeRenderTarget>(new GEMetalNativeRenderTarget(commandQueue,desc.metalLayer));
        };

        SharedHandle<GERenderPipelineState> makeRenderPipelineState(RenderPipelineDescriptor &desc) override{
            metalDevice.assertExists();
            MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
            if(desc.name.size() > 0) {
                pipelineDesc.label = [[NSString alloc] initWithUTF8String:desc.name.data()];
            }

            bool hasDepthStencilState = desc.depthAndStencilDesc.enableDepth || desc.depthAndStencilDesc.enableStencil;
            NSSmartPtr depthStencilState = NSObjectHandle{nullptr};
            if(hasDepthStencilState){
                MTLDepthStencilDescriptor *stencilDepthDesc = [[MTLDepthStencilDescriptor alloc] init];
                if(!desc.depthAndStencilDesc.enableDepth){
                    stencilDepthDesc.depthWriteEnabled = NO;
                    stencilDepthDesc.depthCompareFunction = MTLCompareFunctionNever;
                }

                if(!desc.depthAndStencilDesc.enableStencil){
                    stencilDepthDesc.frontFaceStencil = nil;
                    stencilDepthDesc.backFaceStencil = nil;
                }
                else {
                    MTLStencilDescriptor *frontFaceStencil = [[MTLStencilDescriptor alloc] init],*backFaceStencil = [[MTLStencilDescriptor alloc] init];
                    backFaceStencil.readMask = frontFaceStencil.readMask = desc.depthAndStencilDesc.stencilReadMask;
                    backFaceStencil.writeMask = frontFaceStencil.writeMask = desc.depthAndStencilDesc.stencilWriteMask;

                    frontFaceStencil.stencilCompareFunction = convertCompareFunc(desc.depthAndStencilDesc.frontFaceStencil.func);
                    frontFaceStencil.stencilFailureOperation = convertStencilOp(desc.depthAndStencilDesc.frontFaceStencil.stencilFail);
                    frontFaceStencil.depthFailureOperation = convertStencilOp(desc.depthAndStencilDesc.frontFaceStencil.depthFail);
                    frontFaceStencil.depthStencilPassOperation = convertStencilOp(desc.depthAndStencilDesc.frontFaceStencil.pass);

                    backFaceStencil.stencilCompareFunction = convertCompareFunc(desc.depthAndStencilDesc.backFaceStencil.func);
                    backFaceStencil.stencilFailureOperation = convertStencilOp(desc.depthAndStencilDesc.backFaceStencil.stencilFail);
                    backFaceStencil.depthFailureOperation = convertStencilOp(desc.depthAndStencilDesc.backFaceStencil.depthFail);
                    backFaceStencil.depthStencilPassOperation = convertStencilOp(desc.depthAndStencilDesc.backFaceStencil.pass);

                    stencilDepthDesc.frontFaceStencil = frontFaceStencil;
                    stencilDepthDesc.backFaceStencil = backFaceStencil;
                }

                stencilDepthDesc.depthWriteEnabled = desc.depthAndStencilDesc.writeAmount == DepthWriteAmount::All? YES : NO;
                stencilDepthDesc.depthCompareFunction = convertCompareFunc(desc.depthAndStencilDesc.depthOperation);



                depthStencilState = NSObjectHandle{NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newDepthStencilStateWithDescriptor:stencilDepthDesc]};
            }
            
            auto vertexFunc = (GEMetalShader *)desc.vertexFunc.get();
            auto fragmentFunc = (GEMetalShader *)desc.fragmentFunc.get();
            vertexFunc->function.assertExists();
            fragmentFunc->function.assertExists();
//            pipelineDesc.label = @"RENDER PIPELINE";
            pipelineDesc.vertexFunction = NSOBJECT_OBJC_BRIDGE(id<MTLFunction>,vertexFunc->function.handle());
            pipelineDesc.fragmentFunction = NSOBJECT_OBJC_BRIDGE(id<MTLFunction>,fragmentFunc->function.handle());
            pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;


            MTLCullMode cullMode;

            if(desc.cullMode == RasterCullMode::Front){
                cullMode = MTLCullModeFront;
            }
            else if(desc.cullMode == RasterCullMode::Back){
                cullMode = MTLCullModeBack;
            }
            else {
                cullMode = MTLCullModeNone;
            }

            MTLTriangleFillMode fillMode;
            if(desc.triangleFillMode == TriangleFillMode::Wireframe){
                fillMode = MTLTriangleFillModeLines;
            }
            else {
                fillMode = MTLTriangleFillModeFill;
            }

            MTLWinding winding;

            if(desc.polygonFrontFaceRotation == GTEPolygonFrontFaceRotation::Clockwise){
                winding = MTLWindingClockwise;
            }
            else {
                winding = MTLWindingCounterClockwise;
            }

            GEMetalRasterizerState rasterizerState {winding,cullMode,fillMode,desc.depthAndStencilDesc.depthBias,desc.depthAndStencilDesc.slopeScale,desc.depthAndStencilDesc.depthClamp};
            
            NSError *error;
            NSSmartPtr pipelineState =  NSObjectHandle{NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newRenderPipelineStateWithDescriptor:pipelineDesc error:&error]};
            
            if(pipelineState.handle() == nil || error.code < 0){
                DEBUG_STREAM("Failed to Create Render Pipeline State");
                exit(1);
            };
            
            return std::shared_ptr<GERenderPipelineState>(new GEMetalRenderPipelineState(desc.vertexFunc,desc.fragmentFunc,pipelineState,hasDepthStencilState,depthStencilState,rasterizerState));
        };

        SharedHandle<GETextureRenderTarget> makeTextureRenderTarget(const TextureRenderTargetDescriptor &desc) override {
            metalDevice.assertExists();
            SharedHandle<GETexture> texture;
            if(desc.renderToExistingTexture){
                texture = desc.texture;
            }
            else {
                TextureDescriptor textureDescriptor {
                    GETexture::Texture2D,
                    Shared,
                    GETexture::RenderTarget,
                    TexturePixelFormat::RGBA8Unorm,
                    desc.region.w,
                    desc.region.h,
                    desc.region.d};

                texture = makeTexture(textureDescriptor);
            }
            auto commandQueue = makeCommandQueue(64);
            return SharedHandle<GETextureRenderTarget>(new GEMetalTextureRenderTarget(texture,commandQueue));
        };
        SharedHandle<GETexture> makeTexture(const TextureDescriptor &desc) override{
            assert(desc.sampleCount >= 1 && "Can only create textures with 1 or more samples");
            metalDevice.assertExists();
            MTLTextureDescriptor *mtlDesc = [[MTLTextureDescriptor alloc] init];
            MTLTextureType textureType;
            MTLTextureUsage usage;
            switch (desc.usage) {
                case GETexture::ToGPU : {
                    usage = MTLTextureUsageShaderRead;
                    break;
                }
                case GETexture::FromGPU : {
                    usage = MTLTextureUsageShaderWrite;
                    break;
                }
                case GETexture::GPUAccessOnly : {
                    usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                    break;
                }
                case GETexture::RenderTarget :
                case GETexture::RenderTargetAndDepthStencil :
                case GETexture::MSResolveSrc : {
                    usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                    break;
                }
            }

            switch (desc.type) {
                case GETexture::Texture1D : {
                    textureType = MTLTextureType1D;
                    break;
                }
                case GETexture::Texture2D : {
                    if(desc.sampleCount > 1){
                        textureType = MTLTextureType2DMultisample;
                    }
                    else {
                        textureType = MTLTextureType2D;
                    }
                    break;
                }
                case GETexture::Texture3D : {
                    textureType = MTLTextureType3D;
                    break;
                }
            }

            mtlDesc.usage = usage;
            mtlDesc.textureType = textureType;
            mtlDesc.width = desc.width;
            mtlDesc.height = desc.height;
            mtlDesc.sampleCount = desc.sampleCount;
            mtlDesc.depth = desc.depth;
            mtlDesc.arrayLength = 1;
            mtlDesc.storageMode = MTLStorageModePrivate;

            MTLPixelFormat pixelFormat;
            switch (desc.pixelFormat) {
                case TexturePixelFormat::RGBA8Unorm : {
                    pixelFormat = MTLPixelFormatRGBA8Unorm;
                    break;
                }
                case TexturePixelFormat::RGBA16Unorm : {
                    pixelFormat = MTLPixelFormatRGBA16Unorm;
                    break;
                }
                case TexturePixelFormat::RGBA8Unorm_SRGB : {
                    pixelFormat = MTLPixelFormatRGBA8Unorm_sRGB;
                    break;
                }
            }

            mtlDesc.pixelFormat = pixelFormat;
            mtlDesc.mipmapLevelCount = desc.mipLevels;

            NSSmartPtr texture = NSObjectHandle {NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newTextureWithDescriptor:mtlDesc]};
            texture.assertExists();
            return std::shared_ptr<GETexture>(new GEMetalTexture(desc.type,desc.usage,desc.pixelFormat,texture));
        };

        SharedHandle<GESamplerState> makeSamplerState(const SamplerDescriptor &desc) override {
            MTLSamplerDescriptor *mtlSamplerDescriptor = [[MTLSamplerDescriptor alloc] init];
            if(desc.name.size() > 0) {
                mtlSamplerDescriptor.label = [[NSString alloc] initWithUTF8String:desc.name.data()];
            }

            switch (desc.filter) {
                case SamplerDescriptor::Filter::Linear : {
                    mtlSamplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
                    mtlSamplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
                    mtlSamplerDescriptor.mipFilter = MTLSamplerMipFilterLinear;
                    break;
                }
            }

            mtlSamplerDescriptor.maxAnisotropy = desc.maxAnisotropy;

            MTLSamplerAddressMode samplerAddressMode = MTLSamplerAddressModeClampToZero;
            switch (desc.uAddressMode) {
                case SamplerDescriptor::AddressMode::Wrap : {
                    samplerAddressMode = MTLSamplerAddressModeRepeat;
                    break;
                }
                case SamplerDescriptor::AddressMode::ClampToEdge : {
                    samplerAddressMode = MTLSamplerAddressModeClampToEdge;
                    break;
                }
                case SamplerDescriptor::AddressMode::MirrorWrap : {
                    samplerAddressMode = MTLSamplerAddressModeMirrorRepeat;
                    break;
                }
                case SamplerDescriptor::AddressMode::MirrorClampToEdge : {
                    samplerAddressMode = MTLSamplerAddressModeMirrorClampToEdge;
                    break;
                }
            }
            mtlSamplerDescriptor.sAddressMode = samplerAddressMode;
            switch (desc.vAddressMode) {
                case SamplerDescriptor::AddressMode::Wrap : {
                    samplerAddressMode = MTLSamplerAddressModeRepeat;
                    break;
                }
                case SamplerDescriptor::AddressMode::ClampToEdge : {
                    samplerAddressMode = MTLSamplerAddressModeClampToEdge;
                    break;
                }
                case SamplerDescriptor::AddressMode::MirrorWrap : {
                    samplerAddressMode = MTLSamplerAddressModeMirrorRepeat;
                    break;
                }
                case SamplerDescriptor::AddressMode::MirrorClampToEdge : {
                    samplerAddressMode = MTLSamplerAddressModeMirrorClampToEdge;
                    break;
                }
            }
            mtlSamplerDescriptor.tAddressMode = samplerAddressMode;
            switch (desc.wAddressMode) {
                case SamplerDescriptor::AddressMode::Wrap : {
                    samplerAddressMode = MTLSamplerAddressModeRepeat;
                    break;
                }
                case SamplerDescriptor::AddressMode::ClampToEdge : {
                    samplerAddressMode = MTLSamplerAddressModeClampToEdge;
                    break;
                }
                case SamplerDescriptor::AddressMode::MirrorWrap : {
                    samplerAddressMode = MTLSamplerAddressModeMirrorRepeat;
                    break;
                }
                case SamplerDescriptor::AddressMode::MirrorClampToEdge : {
                    samplerAddressMode = MTLSamplerAddressModeMirrorClampToEdge;
                    break;
                }
            }
            mtlSamplerDescriptor.rAddressMode = samplerAddressMode;

            NSSmartPtr samplerState = NSObjectHandle{NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newSamplerStateWithDescriptor:mtlSamplerDescriptor]};

            return SharedHandle<GESamplerState>(new GEMetalSamplerState {samplerState});
        }
    };


    SharedHandle<OmegaGraphicsEngine> CreateMetalEngine(SharedHandle<GTEDevice> & device){
        return std::shared_ptr<OmegaGraphicsEngine>(new GEMetalEngine(device));
    };
_NAMESPACE_END_
