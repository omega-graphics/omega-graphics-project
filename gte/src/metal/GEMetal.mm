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
#include "../common/GEResourceTracker.h"

#include "../BufferIO.h"

#import <Metal/Metal.h>
#include <simd/simd.h>

#if !__has_attribute(ext_vector_type)
#pragma error("Requires vector types")
#endif

_NAMESPACE_BEGIN_

    inline MTLPixelFormat pixelFormatToMTLPixelFormat(PixelFormat fmt, bool renderTargetUsage = false){
        switch(fmt){
            case PixelFormat::RGBA8Unorm:      return renderTargetUsage ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
            case PixelFormat::RGBA16Unorm:     return MTLPixelFormatRGBA16Unorm;
            case PixelFormat::RGBA8Unorm_SRGB: return renderTargetUsage ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm_sRGB;
            case PixelFormat::BGRA8Unorm:      return MTLPixelFormatBGRA8Unorm;
            case PixelFormat::BGRA8Unorm_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
            default:                           return MTLPixelFormatRGBA8Unorm;
        }
    }

static inline NSString *ns_string_from_str_ref(OmegaCommon::StrRef str){
    if(str.data() == nullptr || str.size() == 0){
        return @"";
    }
    NSString *value = [[NSString alloc] initWithBytes:str.data()
                                               length:str.size()
                                             encoding:NSUTF8StringEncoding];
    if(value == nil){
        value = [[NSString alloc] initWithUTF8String:""];
    }
    return [value autorelease];
}

    struct GTEMetalDevice : public GTEDevice {
        __strong id<MTLDevice> device;
        GTEMetalDevice(Type type,const char *name,GTEDeviceFeatures & features,id<MTLDevice> _device): GTEDevice(type,name,features),device(_device){}
        const void * native() override {
            return device;
        }
        GTEDeviceMemoryBudget queryMemoryBudget() override {
            GTEDeviceMemoryBudget out;
            if(@available(macOS 10.15, iOS 13.0, *)){
                if(device.hasUnifiedMemory){
                    out.unifiedMemory = true;
                    out.dedicatedVideoMemory = 0;
                    out.availableVideoMemory = 0;
                    return out;
                }
            }
            out.unifiedMemory = false;
            out.dedicatedVideoMemory = (uint64_t)device.recommendedMaxWorkingSetSize;
            uint64_t used = (uint64_t)device.currentAllocatedSize;
            out.availableVideoMemory = (out.dedicatedVideoMemory > used)
                ? (out.dedicatedVideoMemory - used) : 0;
            return out;
        }
        ~GTEMetalDevice() = default;
    };

    static GTEDeviceFeatures queryMetalFeatures(id<MTLDevice> dev){
        GTEDeviceFeatures features{};

        // Family detection. MTLGPUFamily enum is available macOS 10.15+/iOS 13.0+.
        bool appleSilicon = false, apple4 = false, apple6 = false, apple7 = false, mac2 = false;
        if(@available(macOS 10.15, iOS 13.0, *)){
            appleSilicon = [dev supportsFamily:MTLGPUFamilyApple1];
            apple4 = [dev supportsFamily:MTLGPUFamilyApple4];
            apple6 = [dev supportsFamily:MTLGPUFamilyApple6];
            apple7 = [dev supportsFamily:MTLGPUFamilyApple7];
            mac2   = [dev supportsFamily:MTLGPUFamilyMac2];
        }

        // ── Raytracing ─────────────────────────────────────────
        if(dev.supportsRaytracing){
            features.flags |= GTEDEVICE_FEATURE_RAYTRACING;
        }

        // ── Rasterizer / blend / sampler ───────────────────────
        features.flags |= GTEDEVICE_FEATURE_INDEPENDENT_BLEND;
        features.flags |= GTEDEVICE_FEATURE_DUAL_SOURCE_BLENDING;
        features.flags |= GTEDEVICE_FEATURE_DEPTH_CLAMP;
        features.flags |= GTEDEVICE_FEATURE_DEPTH_BIAS_CLAMP;
        features.flags |= GTEDEVICE_FEATURE_FILL_MODE_NON_SOLID;
        // WIDE_LINES, CONSERVATIVE_RASTERIZATION: unsupported on Metal.
        features.flags |= GTEDEVICE_FEATURE_SAMPLER_ANISOTROPY;
        features.flags |= GTEDEVICE_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE;
        if(apple4 || mac2){
            features.flags |= GTEDEVICE_FEATURE_MULTI_DRAW_INDIRECT;
        }

        // ── Shader stages ──────────────────────────────────────
        // GEOMETRY_SHADER: Metal has no geometry stage.
        features.flags |= GTEDEVICE_FEATURE_TESSELLATION_SHADER;
        if(apple7){
            features.flags |= GTEDEVICE_FEATURE_MESH_SHADER;
            features.flags |= GTEDEVICE_FEATURE_VARIABLE_RATE_SHADING;
        }

        // ── Fragment extras ────────────────────────────────────
        if([dev respondsToSelector:@selector(supportsShaderBarycentricCoordinates)]){
            if(dev.supportsShaderBarycentricCoordinates){
                features.flags |= GTEDEVICE_FEATURE_SHADER_BARYCENTRIC;
            }
        }

        // ── Bindless / argument buffers ────────────────────────
        if(dev.argumentBuffersSupport == MTLArgumentBuffersTier2){
            features.flags |= GTEDEVICE_FEATURE_DESCRIPTOR_INDEXING;
        }

        // ── Shader precision ───────────────────────────────────
        // Metal natively supports half/short; no double or 64-bit int.
        features.flags |= GTEDEVICE_FEATURE_SHADER_FLOAT16;
        features.flags |= GTEDEVICE_FEATURE_SHADER_INT16;
        // SHADER_FLOAT64, SHADER_INT64: Metal does not expose these in MSL.

        // ── Texture compression ────────────────────────────────
        bool bcSupported = false;
        if([dev respondsToSelector:@selector(supportsBCTextureCompression)]){
            bcSupported = dev.supportsBCTextureCompression;
        } else {
            bcSupported = mac2;
        }
        if(bcSupported) features.flags |= GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_BC;
        if(appleSilicon){
            features.flags |= GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_ETC2;
            features.flags |= GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_ASTC;
        }

        // ── Timestamp queries ──────────────────────────────────
        if(@available(macOS 11.0, iOS 14.0, *)){
            if([dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]){
                features.flags |= GTEDEVICE_FEATURE_TIMESTAMP_QUERIES;
                // Apple GPUs: MTLTimestamp at stage boundaries is already in nanoseconds.
                features.timestampPeriod = 1.0f;
            }
        }

        // ── MSAA ───────────────────────────────────────────────
        uint8_t maxSamples = 1;
        for(NSUInteger n : {(NSUInteger)32,(NSUInteger)16,(NSUInteger)8,(NSUInteger)4,(NSUInteger)2}){
            if([dev supportsTextureSampleCount:n]){
                maxSamples = (uint8_t)n;
                break;
            }
        }
        features.maxMSAASamples = maxSamples;

        // ── Shader model (coarse mapping from GPU family) ──────
        if(apple7)      features.shaderModel = GTEDeviceFeatures::ShaderModel::SM_6_5;
        else if(apple6) features.shaderModel = GTEDeviceFeatures::ShaderModel::SM_6_4;
        else if(apple4 || mac2) features.shaderModel = GTEDeviceFeatures::ShaderModel::SM_6_0;
        else            features.shaderModel = GTEDeviceFeatures::ShaderModel::SM_5_0;

        // ── Limits ─────────────────────────────────────────────
        features.maxBufferSize = (uint64_t)dev.maxBufferLength;

        MTLSize mtg = dev.maxThreadsPerThreadgroup;
        features.maxComputeWorkGroupSizeX = (uint32_t)mtg.width;
        features.maxComputeWorkGroupSizeY = (uint32_t)mtg.height;
        features.maxComputeWorkGroupSizeZ = (uint32_t)mtg.depth;
        // Metal does not expose a distinct "max total invocations" property;
        // the X-dim cap matches the documented per-threadgroup total on all current GPUs.
        features.maxComputeWorkGroupInvocations = (uint32_t)mtg.width;
        features.maxComputeSharedMemorySize = (uint32_t)dev.maxThreadgroupMemoryLength;

        features.maxSamplerAnisotropy = 16;

        // Texture dimension limits from Metal feature-set tables.
        if(apple4 || mac2){
            features.maxTextureDimension2D   = 16384;
            features.maxTextureDimensionCube = 16384;
        } else if(appleSilicon){
            features.maxTextureDimension2D   = 8192;
            features.maxTextureDimensionCube = 8192;
        } else {
            features.maxTextureDimension2D   = 16384;
            features.maxTextureDimensionCube = 16384;
        }
        features.maxTextureDimension3D = 2048;

        return features;
    }

    /// GTE Device Enumerate
    OmegaCommon::Vector<SharedHandle<GTEDevice>> enumerateDevices(){
        OmegaCommon::Vector<SharedHandle<GTEDevice>> devs;
        NSArray<id<MTLDevice>> *mtlDevices = MTLCopyAllDevices();

        for(id<MTLDevice> dev in mtlDevices){
            GTEDeviceFeatures features = queryMetalFeatures(dev);
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
        traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Create,
                ResourceTracking::Backend::Metal,
                "Buffer",
                traceResourceId,
                metalBuffer.handle(),
                static_cast<float>(NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer.handle()).length));
    };
    
    size_t GEMetalBuffer::size(){
        metalBuffer.assertExists();
        return NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer.handle()).length;
    };

    void GEMetalBuffer::setName(OmegaCommon::StrRef name) {
        NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer.handle()).label = ns_string_from_str_ref(name);
    }

    GEMetalBuffer::~GEMetalBuffer(){
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Destroy,
                ResourceTracking::Backend::Metal,
                "Buffer",
                traceResourceId,
                metalBuffer.handle(),
                static_cast<float>(NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer.handle()).length));
    };


    GEMetalAccelerationStruct::GEMetalAccelerationStruct(NSSmartPtr & accelStruct,
    SharedHandle<GEMetalBuffer> & scratchBuffer):accelStruct(accelStruct),scratchBuffer(scratchBuffer){

    }


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
                    case OMEGASL_INT :
                    case OMEGASL_UINT : {
                        delete (int *)b.data;
                        break;
                    }
                    case OMEGASL_INT2 :
                    case OMEGASL_UINT2 : {
                        delete (simd_int2 *)b.data;
                        break;
                    }
                    case OMEGASL_INT3 :
                    case OMEGASL_UINT3 : {
                        delete (simd_int3 *)b.data;
                        break;
                    }
                    case OMEGASL_INT4 :
                    case OMEGASL_UINT4 : {
                        delete (simd_int4 *)b.data;
                        break;
                    }
                    default: break;
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
        void writeInt(int &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT,new int(v)});
        }
        void writeInt2(IVec<2> &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT2,new simd_int2(simd_make_int2(v[0][0],v[1][0]))});
        }
        void writeInt3(IVec<3> &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT3,new simd_int3(simd_make_int3(v[0][0],v[1][0],v[2][0]))});
        }
        void writeInt4(IVec<4> &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT4,new simd_int4(simd_make_int4(v[0][0],v[1][0],v[2][0],v[3][0]))});
        }
        void writeUint(unsigned &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT,new unsigned(v)});
        }
        void writeUint2(UVec<2> &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT2,new simd_uint2(simd_make_uint2(v[0][0],v[1][0]))});
        }
        void writeUint3(UVec<3> &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT3,new simd_uint3(simd_make_uint3(v[0][0],v[1][0],v[2][0]))});
        }
        void writeUint4(UVec<4> &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT4,new simd_uint4(simd_make_uint4(v[0][0],v[1][0],v[2][0],v[3][0]))});
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
                    case OMEGASL_INT :
                    case OMEGASL_UINT : {
                        s = sizeof(int);
                        break;
                    }
                    case OMEGASL_INT2 :
                    case OMEGASL_UINT2 : {
                        s = sizeof(simd_int2);
                        break;
                    }
                    case OMEGASL_INT3 :
                    case OMEGASL_UINT3 : {
                        s = sizeof(simd_int3);
                        break;
                    }
                    case OMEGASL_INT4 :
                    case OMEGASL_UINT4 : {
                        s = sizeof(simd_int4);
                        break;
                    }
                    default: break;
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
                    biggestSize = s;
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



    GEMetalFence::GEMetalFence(NSSmartPtr & event):metalEvent(event),eventValue(0){

    };

    void GEMetalFence::setName(OmegaCommon::StrRef name) {
        [NSOBJECT_OBJC_BRIDGE(id<MTLEvent>,metalEvent.handle()) setLabel:ns_string_from_str_ref(name)];
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

    inline MTLVertexFormat convertVertexFormatToMTL(VertexFormat fmt){
        switch(fmt){
            case VertexFormat::Float:    return MTLVertexFormatFloat;
            case VertexFormat::Float2:   return MTLVertexFormatFloat2;
            case VertexFormat::Float3:   return MTLVertexFormatFloat3;
            case VertexFormat::Float4:   return MTLVertexFormatFloat4;
            case VertexFormat::Int:      return MTLVertexFormatInt;
            case VertexFormat::Int2:     return MTLVertexFormatInt2;
            case VertexFormat::Int3:     return MTLVertexFormatInt3;
            case VertexFormat::Int4:     return MTLVertexFormatInt4;
            case VertexFormat::UInt:     return MTLVertexFormatUInt;
            case VertexFormat::UInt2:    return MTLVertexFormatUInt2;
            case VertexFormat::UInt3:    return MTLVertexFormatUInt3;
            case VertexFormat::UInt4:    return MTLVertexFormatUInt4;
            case VertexFormat::UNorm8x4: return MTLVertexFormatUChar4Normalized;
            case VertexFormat::SNorm8x4: return MTLVertexFormatChar4Normalized;
            case VertexFormat::UShort2:  return MTLVertexFormatUShort2;
            case VertexFormat::UShort4:  return MTLVertexFormatUShort4;
            case VertexFormat::Half2:    return MTLVertexFormatHalf2;
            case VertexFormat::Half4:    return MTLVertexFormatHalf4;
        }
        return MTLVertexFormatFloat4;
    }

    inline MTLBlendFactor convertBlendFactorMTL(BlendFactor f){
        switch(f){
            case BlendFactor::Zero:             return MTLBlendFactorZero;
            case BlendFactor::One:              return MTLBlendFactorOne;
            case BlendFactor::SrcColor:         return MTLBlendFactorSourceColor;
            case BlendFactor::InvSrcColor:      return MTLBlendFactorOneMinusSourceColor;
            case BlendFactor::SrcAlpha:         return MTLBlendFactorSourceAlpha;
            case BlendFactor::InvSrcAlpha:      return MTLBlendFactorOneMinusSourceAlpha;
            case BlendFactor::DestColor:        return MTLBlendFactorDestinationColor;
            case BlendFactor::InvDestColor:     return MTLBlendFactorOneMinusDestinationColor;
            case BlendFactor::DestAlpha:        return MTLBlendFactorDestinationAlpha;
            case BlendFactor::InvDestAlpha:     return MTLBlendFactorOneMinusDestinationAlpha;
            case BlendFactor::SrcAlphaSaturated:return MTLBlendFactorSourceAlphaSaturated;
            case BlendFactor::Src1Color:        return MTLBlendFactorSource1Color;
            case BlendFactor::InvSrc1Color:     return MTLBlendFactorOneMinusSource1Color;
            case BlendFactor::Src1Alpha:        return MTLBlendFactorSource1Alpha;
            case BlendFactor::InvSrc1Alpha:     return MTLBlendFactorOneMinusSource1Alpha;
        }
        return MTLBlendFactorOne;
    }

    inline MTLBlendOperation convertBlendOperationMTL(BlendOperation op){
        switch(op){
            case BlendOperation::Add:             return MTLBlendOperationAdd;
            case BlendOperation::Subtract:        return MTLBlendOperationSubtract;
            case BlendOperation::ReverseSubtract: return MTLBlendOperationReverseSubtract;
            case BlendOperation::Min:             return MTLBlendOperationMin;
            case BlendOperation::Max:             return MTLBlendOperationMax;
        }
        return MTLBlendOperationAdd;
    }

    inline MTLColorWriteMask convertColorWriteMaskMTL(uint8_t mask){
        MTLColorWriteMask res = MTLColorWriteMaskNone;
        if(mask & ColorWriteRed)   res |= MTLColorWriteMaskRed;
        if(mask & ColorWriteGreen) res |= MTLColorWriteMaskGreen;
        if(mask & ColorWriteBlue)  res |= MTLColorWriteMaskBlue;
        if(mask & ColorWriteAlpha) res |= MTLColorWriteMaskAlpha;
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
                if(shaderDesc->data == nullptr){
                    NSLog(@"Failed to load runtime shader function %@: no compiled library payload.",str);
                    return nullptr;
                }
                library = NSObjectHandle {shaderDesc->data};
            }
            else {
                auto data = dispatch_data_create(shaderDesc->data,shaderDesc->dataSize,nullptr,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                NSError *error;
                library = NSObjectHandle {NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newLibraryWithData:data error:&error]};
                dispatch_release(data);

            }
            if(library.handle() == nullptr){
                NSLog(@"Failed to create Metal library for function %@",str);
                return nullptr;
            }
            NSSmartPtr func = NSObjectHandle {NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLLibrary>,library.handle()) newFunctionWithName:str] };
            if(func.handle() == nullptr){
                NSLog(@"Failed to load Metal function %@",str);
                return nullptr;
            }


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
            // GPU capture disabled — it intercepts Metal calls via CaptureMTLDevice
            // and can interfere with drawable presentation.

            metalDevice = NSObjectHandle {NSOBJECT_CPP_BRIDGE device};
            DEBUG_STREAM("GEMetalEngine Successfully Created");
            
        };

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
                pipelineDescriptor.label = ns_string_from_str_ref(desc.name);
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
            heapDesc.size = desc.len;
            heapDesc.storageMode = MTLStorageModeShared;
            heapDesc.cpuCacheMode = MTLCPUCacheModeDefaultCache;

            id<MTLHeap> mtlHeap = [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newHeapWithDescriptor:heapDesc];
            if(mtlHeap == nil){
                DEBUG_STREAM("Failed to create MTLHeap");
                return nullptr;
            }

            class GEMetalHeap : public GEHeap {
                NSSmartPtr metalDevice;
                NSSmartPtr metalHeap;
            public:
                GEMetalHeap(NSSmartPtr device, NSSmartPtr heap)
                    : metalDevice(device), metalHeap(heap) {}

                size_t currentSize() override {
                    return [NSOBJECT_OBJC_BRIDGE(id<MTLHeap>,metalHeap.handle()) currentAllocatedSize];
                }

                SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor & desc) override {
                    id<MTLBuffer> buf = [NSOBJECT_OBJC_BRIDGE(id<MTLHeap>,metalHeap.handle())
                        newBufferWithLength:desc.len
                        options:MTLResourceStorageModeShared];
                    if(buf == nil) return nullptr;

                    MTLBufferLayoutDescriptor *layoutDesc = [[MTLBufferLayoutDescriptor alloc] init];
                    layoutDesc.stride = desc.objectStride;

                    NSSmartPtr bufPtr(NSObjectHandle{NSOBJECT_CPP_BRIDGE buf});
                    NSSmartPtr layoutPtr(NSObjectHandle{NSOBJECT_CPP_BRIDGE layoutDesc});
                    return SharedHandle<GEBuffer>(new GEMetalBuffer(desc.usage, bufPtr, layoutPtr));
                }

                SharedHandle<GETexture> makeTexture(const TextureDescriptor & desc) override {
                    MTLTextureDescriptor *mtlDesc = [[MTLTextureDescriptor alloc] init];
                    MTLTextureType textureType;
                    MTLTextureUsage usage;
                    switch(desc.usage){
                        case GETexture::ToGPU: usage = MTLTextureUsageShaderRead; break;
                        case GETexture::FromGPU: usage = MTLTextureUsageShaderWrite; break;
                        case GETexture::GPUAccessOnly: usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; break;
                        default: usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; break;
                    }
                    switch(desc.type){
                        case GETexture::Texture1D: textureType = MTLTextureType1D; break;
                        case GETexture::Texture2D:
                            textureType = desc.sampleCount > 1 ? MTLTextureType2DMultisample : MTLTextureType2D; break;
                        case GETexture::Texture3D: textureType = MTLTextureType3D; break;
                    }
                    mtlDesc.usage = usage;
                    mtlDesc.textureType = textureType;
                    mtlDesc.width = desc.width;
                    mtlDesc.height = desc.height;
                    mtlDesc.depth = desc.depth;
                    mtlDesc.sampleCount = desc.sampleCount;
                    mtlDesc.storageMode = MTLStorageModeShared;
                    mtlDesc.pixelFormat = MTLPixelFormatRGBA8Unorm;
                    mtlDesc.mipmapLevelCount = desc.mipLevels;

                    id<MTLTexture> tex = [NSOBJECT_OBJC_BRIDGE(id<MTLHeap>,metalHeap.handle()) newTextureWithDescriptor:mtlDesc];
                    if(tex == nil) return nullptr;

                    NSSmartPtr texPtr(NSObjectHandle{NSOBJECT_CPP_BRIDGE tex});
                    return SharedHandle<GETexture>(new GEMetalTexture(desc.type, desc.usage, desc.pixelFormat, texPtr));
                }
            };

            NSSmartPtr heapPtr(NSObjectHandle{NSOBJECT_CPP_BRIDGE mtlHeap});
            return SharedHandle<GEHeap>(new GEMetalHeap(metalDevice, heapPtr));
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
                pipelineDesc.label = ns_string_from_str_ref(desc.name);
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

            if(!desc.vertexInputDescriptor.attributes.empty()){
                MTLVertexDescriptor *vDesc = [[MTLVertexDescriptor alloc] init];
                for(unsigned i = 0; i < desc.vertexInputDescriptor.bufferLayouts.size(); ++i){
                    const auto & bl = desc.vertexInputDescriptor.bufferLayouts[i];
                    vDesc.layouts[i].stride = bl.stride;
                    vDesc.layouts[i].stepFunction = (bl.stepFunction == VertexStepFunction::PerInstance)
                                                        ? MTLVertexStepFunctionPerInstance
                                                        : MTLVertexStepFunctionPerVertex;
                    vDesc.layouts[i].stepRate = bl.stepRate;
                }
                for(const auto & a : desc.vertexInputDescriptor.attributes){
                    vDesc.attributes[a.shaderLocation].format = convertVertexFormatToMTL(a.format);
                    vDesc.attributes[a.shaderLocation].offset = a.offset;
                    vDesc.attributes[a.shaderLocation].bufferIndex = a.bufferIndex;
                }
                pipelineDesc.vertexDescriptor = vDesc;
            }

            {
                const unsigned attachmentCount =
                    desc.colorPixelFormats.empty() ? 1u : (unsigned)desc.colorPixelFormats.size();
                for(unsigned i = 0; i < attachmentCount; ++i){
                    const PixelFormat pf = desc.colorPixelFormats.empty()
                                               ? PixelFormat::RGBA8Unorm
                                               : desc.colorPixelFormats[i];
                    MTLRenderPipelineColorAttachmentDescriptor *ca = pipelineDesc.colorAttachments[i];
                    ca.pixelFormat = pixelFormatToMTLPixelFormat(pf, true);
                    if(i < desc.colorBlendDescriptors.size()){
                        const auto & b = desc.colorBlendDescriptors[i];
                        ca.blendingEnabled             = b.blendEnabled ? YES : NO;
                        ca.rgbBlendOperation           = convertBlendOperationMTL(b.colorOp);
                        ca.alphaBlendOperation         = convertBlendOperationMTL(b.alphaOp);
                        ca.sourceRGBBlendFactor        = convertBlendFactorMTL(b.srcColorFactor);
                        ca.destinationRGBBlendFactor   = convertBlendFactorMTL(b.destColorFactor);
                        ca.sourceAlphaBlendFactor      = convertBlendFactorMTL(b.srcAlphaFactor);
                        ca.destinationAlphaBlendFactor = convertBlendFactorMTL(b.destAlphaFactor);
                        ca.writeMask                   = convertColorWriteMaskMTL(b.writeMask);
                    }
                    else {
                        ca.blendingEnabled = NO;
                        ca.writeMask = MTLColorWriteMaskAll;
                    }
                }
            }


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
                    GPUOnly,
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
                    usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
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
            switch(desc.storage_opts){
                case StorageOpts::Shared:
                    mtlDesc.storageMode = MTLStorageModeShared;
                    break;
                case StorageOpts::GPUOnly:
                    mtlDesc.storageMode = MTLStorageModePrivate;
                    break;
            }

            const bool renderTargetUsage =
                    desc.usage == GETexture::RenderTarget ||
                    desc.usage == GETexture::RenderTargetAndDepthStencil ||
                    desc.usage == GETexture::MSResolveSrc;
            MTLPixelFormat pixelFormat = pixelFormatToMTLPixelFormat(desc.pixelFormat, renderTargetUsage);

            mtlDesc.pixelFormat = pixelFormat;
            mtlDesc.mipmapLevelCount = desc.mipLevels;

            NSSmartPtr texture = NSObjectHandle {NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newTextureWithDescriptor:mtlDesc]};
            texture.assertExists();
            return std::shared_ptr<GETexture>(new GEMetalTexture(desc.type,desc.usage,desc.pixelFormat,texture));
        };

        SharedHandle<GESamplerState> makeSamplerState(const SamplerDescriptor &desc) override {
            MTLSamplerDescriptor *mtlSamplerDescriptor = [[MTLSamplerDescriptor alloc] init];
            if(desc.name.size() > 0) {
                mtlSamplerDescriptor.label = ns_string_from_str_ref(desc.name);
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
