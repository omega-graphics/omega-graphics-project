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
#include <ctime>
#include <unistd.h>

#include "omegaGTE/GTEDevice.h"
#include "omegaGTE/GECommandQueue.h"
#include "omegaGTE/GEPipeline.h"
#include "omegaGTE/GERenderTarget.h"
#include "omegaGTE/GTEMath.h"
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
        // TESSELLATION_SHADER: not advertised. Metal has no D3D-style
        // hull stage, and the Metal codegen + runtime do not yet
        // implement the compute-kernel-for-factors + post-tessellation-
        // vertex pipeline that Apple's tessellation model requires.
        // See OmegaSL-Reference.md bug 3.
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
                    default:
                        /// Matrix blocks are heap-allocated `unsigned char[]`
                        /// (std430-padded bytes from `encodeFMatrixToStd430`).
                        /// Free them as arrays — the non-matrix branches above
                        /// already covered the typed-allocation cases.
                        if(isMatrixDataType(b.type)){
                            delete[] (unsigned char *)b.data;
                        }
                        break;
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
            /// §2.4 — Metal reads both `device T*` (storage) and `constant T&`
            /// (uniform) buffers with its natural simd/C++ layout, which equals
            /// std430 for our types. So this writer always packs std430 and
            /// intentionally ignores `buffer_->role`. Do NOT switch uniform
            /// buffers to std140 here — that's a Vulkan/D3D12-only layout and
            /// would mismatch what MSL `constant T&` reads.
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
        /// Matrix uploads. Metal's `simd_floatCxR` / `matrix_floatCxR`
        /// matches std430 byte-for-byte (column-major, vec3 columns
        /// padded to 16 bytes), so the same encode helper used by the
        /// Vulkan path produces bytes that Metal can consume directly
        /// from a device buffer / argument buffer. See §12.2.
        void writeFloat2x2(FMatrix<2,2> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<2,2>(), encodeFMatrixToStd430<2,2>(m)});
        }
        void writeFloat3x3(FMatrix<3,3> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<3,3>(), encodeFMatrixToStd430<3,3>(m)});
        }
        void writeFloat4x4(FMatrix<4,4> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<4,4>(), encodeFMatrixToStd430<4,4>(m)});
        }
        void writeFloat2x3(FMatrix<2,3> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<2,3>(), encodeFMatrixToStd430<2,3>(m)});
        }
        void writeFloat2x4(FMatrix<2,4> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<2,4>(), encodeFMatrixToStd430<2,4>(m)});
        }
        void writeFloat3x2(FMatrix<3,2> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<3,2>(), encodeFMatrixToStd430<3,2>(m)});
        }
        void writeFloat3x4(FMatrix<3,4> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<3,4>(), encodeFMatrixToStd430<3,4>(m)});
        }
        void writeFloat4x2(FMatrix<4,2> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<4,2>(), encodeFMatrixToStd430<4,2>(m)});
        }
        void writeFloat4x3(FMatrix<4,3> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<4,3>(), encodeFMatrixToStd430<4,3>(m)});
        }
        void structEnd() override {
            inStruct = false;
        }
        void sendToBuffer() override {
            assert(!inStruct && "Struct must be finished be written to before sending data");
            /// Sequential std430 / Metal-native placement: each member is
            /// written at the running offset and the cursor advances by the
            /// member's size; the struct is rounded up to its alignment (max
            /// member alignment — scalar 4, vec2 8, vec3/vec4/matrix 16) so a
            /// contiguous array of structs stays aligned. Symmetric with
            /// GEMetalBufferReader. This replaces a heuristic that padded to
            /// the biggest member *size* (e.g. 64 for a float4x4) and applied
            /// the padding around the first biggest-size member, which both
            /// mis-aligned structs whose biggest member wasn't first and wrote
            /// trailing padding past the end of the (correctly sized) buffer.
            size_t structAlign = 1;
            size_t local = 0;
            for(auto & block : blocks){
                size_t s = 0, a = 16;
                if(isMatrixDataType(block.type)){
                    auto [cols, rows] = matrixDims(block.type);
                    s = std430MatrixSize(cols, rows);
                    a = std430MatrixAlignment(rows);
                }
                else {
                    switch (block.type) {
                        case OMEGASL_FLOAT :
                        case OMEGASL_INT :
                        case OMEGASL_UINT :  s = sizeof(float);       a = 4;  break;
                        case OMEGASL_FLOAT2 :
                        case OMEGASL_INT2 :
                        case OMEGASL_UINT2 : s = sizeof(simd_float2); a = 8;  break;
                        case OMEGASL_FLOAT3 :
                        case OMEGASL_INT3 :
                        case OMEGASL_UINT3 : s = sizeof(simd_float3); a = 16; break;
                        case OMEGASL_FLOAT4 :
                        case OMEGASL_INT4 :
                        case OMEGASL_UINT4 : s = sizeof(simd_float4); a = 16; break;
                        default: break;
                    }
                }
                memcpy(_data_ptr + currentOffset + local, block.data, s);
                local += s;
                if(a > structAlign) structAlign = a;
            }
            size_t rem = local % structAlign;
            if(rem != 0) local += structAlign - rem;
            currentOffset += local;
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
        /// No-ops. The reader walks fields sequentially at `currentOffset`
        /// (each get* advances by the field's std430/Metal-native size) and
        /// rounds to the struct alignment in `structEnd` — the same model as
        /// the Vulkan / D3D12 readers. The previous heuristic computed trailing
        /// padding from the biggest member *size* (e.g. 64 for a float4x4)
        /// rather than the struct *alignment* (16), and applied it one field
        /// early because `fieldIndex` is pre-incremented in `offsetAndIncrement`
        /// — so the last field of a struct whose tail is smaller than its
        /// biggest member (matrices followed by a float4) was read past its
        /// real offset, returning garbage / zeros.
        inline void readBeforePaddingIfPossible(){}
        inline void readAfterPaddingIfPossible(){}
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
            /// Store the layout for the per-field type asserts. Offsets are
            /// tracked sequentially by the get* methods (and rounded to the
            /// struct alignment in `structEnd`); no padding heuristic — see
            /// `readBeforePaddingIfPossible`.
            structLayout = fields;
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
        /// Matrix downloads. Metal stores `matrix_floatCxR` column-major
        /// with the same Cx3 column padding as std430, so the shared
        /// decode helper produces the right host-side `FMatrix<C,R>`.
        template<unsigned C, unsigned R>
        void getMatrixImpl(FMatrix<C,R> &m, omegasl_data_type tag){
            assert(structLayout[fieldIndex] == tag && "Field is not the expected matrix type");
            readBeforePaddingIfPossible();
            decodeFMatrixFromStd430<C, R>(_data_ptr + currentOffset, m);
            offsetAndIncrement(std430MatrixSize(C, R));
            readAfterPaddingIfPossible();
        }
        void getFloat2x2(FMatrix<2,2> &m) override { getMatrixImpl<2,2>(m, OMEGASL_FLOAT2x2); }
        void getFloat3x3(FMatrix<3,3> &m) override { getMatrixImpl<3,3>(m, OMEGASL_FLOAT3x3); }
        void getFloat4x4(FMatrix<4,4> &m) override { getMatrixImpl<4,4>(m, OMEGASL_FLOAT4x4); }
        void getFloat2x3(FMatrix<2,3> &m) override { getMatrixImpl<2,3>(m, OMEGASL_FLOAT2x3); }
        void getFloat2x4(FMatrix<2,4> &m) override { getMatrixImpl<2,4>(m, OMEGASL_FLOAT2x4); }
        void getFloat3x2(FMatrix<3,2> &m) override { getMatrixImpl<3,2>(m, OMEGASL_FLOAT3x2); }
        void getFloat3x4(FMatrix<3,4> &m) override { getMatrixImpl<3,4>(m, OMEGASL_FLOAT3x4); }
        void getFloat4x2(FMatrix<4,2> &m) override { getMatrixImpl<4,2>(m, OMEGASL_FLOAT4x2); }
        void getFloat4x3(FMatrix<4,3> &m) override { getMatrixImpl<4,3>(m, OMEGASL_FLOAT4x3); }
        void structEnd() override {
            inStruct = false;
            /// std430 struct alignment = max member alignment (scalar 4,
            /// vec2 8, vec3/vec4/matrix 16). Round the cursor up so the next
            /// struct in a contiguous array starts on the struct's alignment.
            size_t structAlign = 1;
            for(auto f : structLayout){
                size_t a = isMatrixDataType(f)
                    ? std430MatrixAlignment(matrixDims(f).second)
                    : (f == OMEGASL_FLOAT || f == OMEGASL_INT || f == OMEGASL_UINT) ? 4
                    : (f == OMEGASL_FLOAT2 || f == OMEGASL_INT2 || f == OMEGASL_UINT2) ? 8
                    : 16;
                if(a > structAlign) structAlign = a;
            }
            size_t rem = currentOffset % structAlign;
            if(rem != 0) currentOffset += structAlign - rem;
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
        SharedHandle<GTEDevice> gteDevice;
        // Extension 3: cached built-in full-screen-triangle vertex shader.
        SharedHandle<GTEShader> blitFullscreenVs;
        std::shared_ptr<omegasl_shader_lib> blitFullscreenVsLib;
        bool ensureBlitFullscreenVs();
        // Set true only when a programmatic GPU capture was actually
        // started in the constructor, so the destructor knows to stop it.
        bool gpuCaptureActive = false;
        // §3 of gte/docs/Debug-Layer-Plan.md. Starts a .gputrace capture
        // when GTEInitOptions::captureOnInit was set (and the debug layer
        // is on). No-op unless the embedding app enabled capture via its
        // Info.plist (MetalCaptureEnabled=YES) / MTL_CAPTURE_ENABLED=1.
        void maybeStartGpuCapture(id<MTLDevice> device){
            if(!isDebugLayerEnabled() || !isCaptureOnInitEnabled()){
                return;
            }
            if(@available(macOS 10.15, iOS 13.0, *)){
                MTLCaptureManager *mgr = [MTLCaptureManager sharedCaptureManager];
                if(![mgr supportsDestination:MTLCaptureDestinationGPUTraceDocument]){
                    DEBUG_STREAM("GPU capture requested but the GPUTraceDocument "
                                 "destination is unsupported. Set MetalCaptureEnabled=YES "
                                 "in the app Info.plist (or MTL_CAPTURE_ENABLED=1). Skipping.");
                    return;
                }
                // Resolve the output path: explicit override, else a default
                // omegagte-<pid>-<ts>.gputrace in the working directory. Both
                // branches yield autoreleased NSStrings (no manual release).
                const char *configured = captureOutputPath();
                NSString *path;
                if(configured != nullptr && configured[0] != '\0'){
                    path = [NSString stringWithUTF8String:configured];
                } else {
                    path = [NSString stringWithFormat:@"omegagte-%d-%ld.gputrace",
                                                      (int)getpid(), (long)time(nullptr)];
                }
                MTLCaptureDescriptor *desc = [[MTLCaptureDescriptor alloc] init];
                desc.captureObject = device;
                desc.destination = MTLCaptureDestinationGPUTraceDocument;
                desc.outputURL = [NSURL fileURLWithPath:path];
                NSError *error = nil;
                if([mgr startCaptureWithDescriptor:desc error:&error]){
                    gpuCaptureActive = true;
                    DEBUG_STREAM("GEMetalEngine GPU capture started -> " << [path UTF8String]);
                } else {
                    DEBUG_STREAM("GEMetalEngine GPU capture failed to start: "
                                 << (error ? [[error localizedDescription] UTF8String]
                                           : "(unknown error)"));
                }
                [desc release];
            } else {
                DEBUG_STREAM("GPU capture requested but requires macOS 10.15+/iOS 13+. Skipping.");
            }
        }
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

            metalDevice = NSObjectHandle {NSOBJECT_CPP_BRIDGE device};
            gteDevice = __device;
            _deviceFeatures = __device->features.featuresAsBitmask();

            // Programmatic GPU frame capture, opt-in via
            // GTEInitOptions::captureOnInit. Started here (before the first
            // command queue) and stopped in ~GEMetalEngine. See
            // GTEInitOptions::captureOnInit for the toggle.
            maybeStartGpuCapture(device);

            DEBUG_STREAM("GEMetalEngine Successfully Created");

        };

        ~GEMetalEngine() override {
            if(gpuCaptureActive){
                if(@available(macOS 10.15, iOS 13.0, *)){
                    [[MTLCaptureManager sharedCaptureManager] stopCapture];
                }
                gpuCaptureActive = false;
                DEBUG_STREAM("GEMetalEngine GPU capture stopped");
            }
        }

        SharedHandle<GEBuffer> createBoundingBoxesBuffer(OmegaCommon::ArrayRef<GERaytracingBoundingBox> boxes) override {
            if(!gteDevice->features.hasFeature(GTEDEVICE_FEATURE_RAYTRACING)){
                DEBUG_STREAM("Raytracing not supported on this device");
                return nullptr;
            }
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
            if(!gteDevice->features.hasFeature(GTEDEVICE_FEATURE_RAYTRACING)){
                DEBUG_STREAM("Raytracing not supported on this device");
                return nullptr;
            }
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
            auto *mtl_buffer = new GEMetalBuffer(desc.usage,buffer,layoutDesc);
            /// Metal binds `constant T&` (uniform) and `device T*` (storage)
            /// identically via setBuffer:atIndex:, and MTLBuffer creation is
            /// the same for both — so the role is recorded only so bind-time
            /// validation can confirm a uniform slot receives a Uniform buffer.
            mtl_buffer->role = desc.role;
            return std::shared_ptr<GEBuffer>(mtl_buffer);
        };
        SharedHandle<GEComputePipelineState> makeComputePipelineState(ComputePipelineDescriptor &desc) override{
            if(!_checkPipelineShader(desc.computeFunc,"compute",desc.name)){
                return nullptr;
            }
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
                    auto *mtl_buffer = new GEMetalBuffer(desc.usage, bufPtr, layoutPtr);
                    mtl_buffer->role = desc.role;
                    return SharedHandle<GEBuffer>(mtl_buffer);
                }

                SharedHandle<GETexture> makeTexture(const TextureDescriptor & desc) override {
                    MTLTextureDescriptor *mtlDesc = [[MTLTextureDescriptor alloc] init];
                    MTLTextureUsage usage;
                    switch(desc.usage){
                        case GETexture::ToGPU: usage = MTLTextureUsageShaderRead; break;
                        case GETexture::FromGPU: usage = MTLTextureUsageShaderWrite; break;
                        case GETexture::GPUAccessOnly: usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; break;
                        default: usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; break;
                    }

                    // §6.2 — drive MTLTextureType from the descriptor's kind so
                    // cube / array / MS variants pick the right native type.
                    const TextureKind kind = desc.kind == TextureKind::Auto ? TextureKind::Tex2D : desc.kind;
                    const unsigned arrayLayers = desc.arrayLayers > 0 ? desc.arrayLayers : 1;
                    const bool isMS = (kind == TextureKind::Tex2DMS || kind == TextureKind::Tex2DMSArray);
                    const unsigned effectiveSampleCount = isMS ? (desc.sampleCount > 1 ? desc.sampleCount : 1u) : 1u;
                    const unsigned effectiveMips = isMS ? 1u : desc.mipLevels;

                    MTLTextureType textureType = MTLTextureType2D;
                    NSUInteger mtlArrayLength = 1;
                    switch(kind){
                        case TextureKind::Tex1D:        textureType = MTLTextureType1D; break;
                        case TextureKind::Tex1DArray:
                            textureType = MTLTextureType1DArray;
                            mtlArrayLength = arrayLayers;
                            break;
                        case TextureKind::Tex2D:        textureType = MTLTextureType2D; break;
                        case TextureKind::Tex2DArray:
                            textureType = MTLTextureType2DArray;
                            mtlArrayLength = arrayLayers;
                            break;
                        case TextureKind::TexCube:
                            textureType = MTLTextureTypeCube;
                            break;
                        case TextureKind::TexCubeArray:
                            textureType = MTLTextureTypeCubeArray;
                            // Metal expresses cube arrays as `arrayLength` =
                            // number of *cubes*, not number of faces.
                            mtlArrayLength = (arrayLayers >= 6 ? arrayLayers : 6) / 6;
                            break;
                        case TextureKind::Tex2DMS:
                            textureType = MTLTextureType2DMultisample;
                            break;
                        case TextureKind::Tex2DMSArray:
                            // MTLTextureType2DMultisampleArray requires
                            // macOS 11+ / iOS 14+. Below that the runtime
                            // would reject this in newTextureWithDescriptor.
                            if (@available(macOS 11.0, iOS 14.0, *)) {
                                textureType = MTLTextureType2DMultisampleArray;
                            } else {
                                std::cerr << "[OmegaGTE] Tex2DMSArray requires macOS 11+ / iOS 14+" << std::endl;
                                return nullptr;
                            }
                            mtlArrayLength = arrayLayers;
                            break;
                        case TextureKind::Tex3D:        textureType = MTLTextureType3D; break;
                        case TextureKind::Auto:
                            textureType = effectiveSampleCount > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
                            break;
                    }

                    mtlDesc.usage = usage;
                    mtlDesc.textureType = textureType;
                    mtlDesc.width = desc.width;
                    mtlDesc.height = desc.height;
                    mtlDesc.depth = desc.depth;
                    mtlDesc.sampleCount = effectiveSampleCount;
                    mtlDesc.arrayLength = mtlArrayLength;
                    mtlDesc.storageMode = MTLStorageModeShared;
                    mtlDesc.pixelFormat = MTLPixelFormatRGBA8Unorm;
                    mtlDesc.mipmapLevelCount = effectiveMips;

                    id<MTLTexture> tex = [NSOBJECT_OBJC_BRIDGE(id<MTLHeap>,metalHeap.handle()) newTextureWithDescriptor:mtlDesc];
                    if(tex == nil) return nullptr;

                    id<MTLTexture> primary = tex;
                    if(!desc.defaultSwizzle.isIdentity()){
                        if(@available(macOS 10.15, iOS 13.0, *)){
                            auto swPos = [](TextureSwizzleChannel ch, MTLTextureSwizzle pos){
                                switch(ch){
                                    case TextureSwizzleChannel::Identity: return pos;
                                    case TextureSwizzleChannel::Red:      return MTLTextureSwizzleRed;
                                    case TextureSwizzleChannel::Green:    return MTLTextureSwizzleGreen;
                                    case TextureSwizzleChannel::Blue:     return MTLTextureSwizzleBlue;
                                    case TextureSwizzleChannel::Alpha:    return MTLTextureSwizzleAlpha;
                                    case TextureSwizzleChannel::Zero:     return MTLTextureSwizzleZero;
                                    case TextureSwizzleChannel::One:      return MTLTextureSwizzleOne;
                                }
                                return pos;
                            };
                            MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(
                                swPos(desc.defaultSwizzle.r, MTLTextureSwizzleRed),
                                swPos(desc.defaultSwizzle.g, MTLTextureSwizzleGreen),
                                swPos(desc.defaultSwizzle.b, MTLTextureSwizzleBlue),
                                swPos(desc.defaultSwizzle.a, MTLTextureSwizzleAlpha)
                            );
                            primary = [tex newTextureViewWithPixelFormat:tex.pixelFormat
                                                              textureType:tex.textureType
                                                                   levels:NSMakeRange(0, tex.mipmapLevelCount)
                                                                   slices:NSMakeRange(0, tex.arrayLength)
                                                                  swizzle:sw];
                        }
                    }

                    NSSmartPtr texPtr(NSObjectHandle{NSOBJECT_CPP_BRIDGE primary});
                    auto result = SharedHandle<GETexture>(new GEMetalTexture(kind, desc.usage, desc.pixelFormat, texPtr));
                    result->setShape(kind, arrayLayers, effectiveSampleCount);
                    return result;
                }
            };

            NSSmartPtr heapPtr(NSObjectHandle{NSOBJECT_CPP_BRIDGE mtlHeap});
            return SharedHandle<GEHeap>(new GEMetalHeap(metalDevice, heapPtr));
        };
        SharedHandle<GENativeRenderTarget> makeNativeRenderTarget(const NativeRenderTargetDescriptor &desc,
                                                                   SharedHandle<GECommandQueue> presentQueue) override{
            // Gate the requested color format against the portable
            // swap-chain/drawable intersection so a backend mismatch is
            // surfaced rather than silently substituted.
            if(!isPortableNativeRenderTargetFormat(desc.pixelFormat)){
                std::cerr << "[GEMetalEngine_Internal] makeNativeRenderTarget: requested pixelFormat is not in the portable drawable set; rejecting." << std::endl;
                return nullptr;
            }
            if(desc.metalLayer == nil){
                std::cerr << "[GEMetalEngine_Internal] makeNativeRenderTarget: descriptor.metalLayer is nil" << std::endl;
                return nullptr;
            }
            metalDevice.assertExists();
            desc.metalLayer.device = NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle());
            // Apply the requested format to the layer so the drawable's
            // texture is created in that format.
            desc.metalLayer.pixelFormat = pixelFormatToMTLPixelFormat(desc.pixelFormat, /*renderTargetUsage=*/true);
            return std::shared_ptr<GENativeRenderTarget>(new GEMetalNativeRenderTarget(std::move(presentQueue),desc.metalLayer,desc.pixelFormat));
        };

        SharedHandle<GERenderPipelineState> makeRenderPipelineState(RenderPipelineDescriptor &desc) override{
            if(!_checkPipelineShader(desc.vertexFunc,"vertex",desc.name) ||
               !_checkPipelineShader(desc.fragmentFunc,"fragment",desc.name)){
                return nullptr;
            }
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

        SharedHandle<GEBlitPipelineState> makeBlitPipelineState(BlitPipelineDescriptor &desc) override {
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
            auto rp = makeRenderPipelineState(rpDesc);
            if (!rp) {
                DEBUG_STREAM("makeBlitPipelineState: underlying makeRenderPipelineState failed");
                return nullptr;
            }
            return SharedHandle<GEBlitPipelineState>(new GEMetalBlitPipelineState(rp));
        };

        SharedHandle<GETextureRenderTarget> makeTextureRenderTarget(const TextureRenderTargetDescriptor &desc) override {
            metalDevice.assertExists();
            SharedHandle<GETexture> texture;
            if(desc.renderToExistingTexture){
                texture = desc.texture;
            }
            else {
                TextureDescriptor textureDescriptor{};
                textureDescriptor.storage_opts = GPUOnly;
                textureDescriptor.usage = GETexture::RenderTarget;
                textureDescriptor.pixelFormat = TexturePixelFormat::RGBA8Unorm;
                textureDescriptor.width = desc.region.w;
                textureDescriptor.height = desc.region.h;
                textureDescriptor.depth = desc.region.d;
                textureDescriptor.kind = TextureKind::Tex2D;

                texture = makeTexture(textureDescriptor);
            }
            return SharedHandle<GETextureRenderTarget>(new GEMetalTextureRenderTarget(texture));
        };
        SharedHandle<GETexture> makeTexture(const TextureDescriptor &desc) override{
            assert(desc.sampleCount >= 1 && "Can only create textures with 1 or more samples");
            metalDevice.assertExists();
            MTLTextureDescriptor *mtlDesc = [[MTLTextureDescriptor alloc] init];
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

            // §6.2 — drive MTLTextureType from the descriptor's kind.
            const TextureKind kind = desc.kind == TextureKind::Auto ? TextureKind::Tex2D : desc.kind;
            const unsigned arrayLayers = desc.arrayLayers > 0 ? desc.arrayLayers : 1;
            const bool isMS = (kind == TextureKind::Tex2DMS || kind == TextureKind::Tex2DMSArray);
            const unsigned effectiveSampleCount = isMS ? (desc.sampleCount > 1 ? desc.sampleCount : 1u) : 1u;
            const unsigned effectiveMips = isMS ? 1u : desc.mipLevels;

            MTLTextureType textureType = MTLTextureType2D;
            NSUInteger mtlArrayLength = 1;
            switch (kind) {
                case TextureKind::Tex1D:
                    textureType = MTLTextureType1D;
                    break;
                case TextureKind::Tex1DArray:
                    textureType = MTLTextureType1DArray;
                    mtlArrayLength = arrayLayers;
                    break;
                case TextureKind::Tex2D:
                    textureType = MTLTextureType2D;
                    break;
                case TextureKind::Tex2DArray:
                    textureType = MTLTextureType2DArray;
                    mtlArrayLength = arrayLayers;
                    break;
                case TextureKind::TexCube:
                    textureType = MTLTextureTypeCube;
                    break;
                case TextureKind::TexCubeArray:
                    textureType = MTLTextureTypeCubeArray;
                    mtlArrayLength = (arrayLayers >= 6 ? arrayLayers : 6) / 6;
                    break;
                case TextureKind::Tex2DMS:
                    textureType = MTLTextureType2DMultisample;
                    break;
                case TextureKind::Tex2DMSArray:
                    if (@available(macOS 11.0, iOS 14.0, *)) {
                        textureType = MTLTextureType2DMultisampleArray;
                    } else {
                        std::cerr << "[OmegaGTE] Tex2DMSArray requires macOS 11+ / iOS 14+" << std::endl;
                        return nullptr;
                    }
                    mtlArrayLength = arrayLayers;
                    break;
                case TextureKind::Tex3D:
                    textureType = MTLTextureType3D;
                    break;
                case TextureKind::Auto:
                    textureType = effectiveSampleCount > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
                    break;
            }

            mtlDesc.usage = usage;
            mtlDesc.textureType = textureType;
            mtlDesc.width = desc.width;
            mtlDesc.height = desc.height;
            mtlDesc.sampleCount = effectiveSampleCount;
            mtlDesc.depth = desc.depth;
            mtlDesc.arrayLength = mtlArrayLength;
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
            mtlDesc.mipmapLevelCount = effectiveMips;

            id<MTLTexture> baseTex = [NSOBJECT_OBJC_BRIDGE(id<MTLDevice>,metalDevice.handle()) newTextureWithDescriptor:mtlDesc];
            // Apply the descriptor's defaultSwizzle by replacing the primary
            // texture handle with a swizzled view. This makes every bind
            // without a runtime override pay zero per-frame cost (proposal §4 / Open Q1).
            id<MTLTexture> primary = baseTex;
            if(!desc.defaultSwizzle.isIdentity()){
                if(@available(macOS 10.15, iOS 13.0, *)){
                    auto swPos = [](TextureSwizzleChannel ch, MTLTextureSwizzle pos){
                        switch(ch){
                            case TextureSwizzleChannel::Identity: return pos;
                            case TextureSwizzleChannel::Red:      return MTLTextureSwizzleRed;
                            case TextureSwizzleChannel::Green:    return MTLTextureSwizzleGreen;
                            case TextureSwizzleChannel::Blue:     return MTLTextureSwizzleBlue;
                            case TextureSwizzleChannel::Alpha:    return MTLTextureSwizzleAlpha;
                            case TextureSwizzleChannel::Zero:     return MTLTextureSwizzleZero;
                            case TextureSwizzleChannel::One:      return MTLTextureSwizzleOne;
                        }
                        return pos;
                    };
                    MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(
                        swPos(desc.defaultSwizzle.r, MTLTextureSwizzleRed),
                        swPos(desc.defaultSwizzle.g, MTLTextureSwizzleGreen),
                        swPos(desc.defaultSwizzle.b, MTLTextureSwizzleBlue),
                        swPos(desc.defaultSwizzle.a, MTLTextureSwizzleAlpha)
                    );
                    primary = [baseTex newTextureViewWithPixelFormat:baseTex.pixelFormat
                                                          textureType:baseTex.textureType
                                                               levels:NSMakeRange(0, baseTex.mipmapLevelCount)
                                                               slices:NSMakeRange(0, baseTex.arrayLength)
                                                              swizzle:sw];
                }
            }
            NSSmartPtr texture = NSObjectHandle {NSOBJECT_CPP_BRIDGE primary};
            texture.assertExists();
            auto result = std::shared_ptr<GETexture>(new GEMetalTexture(kind,desc.usage,desc.pixelFormat,texture));
            result->setShape(kind, arrayLayers, effectiveSampleCount);
            return result;
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

    // Extension 3: OmegaSL source for the engine-supplied full-screen-triangle
    // vertex shader paired with every blit pipeline. See BlitPipelineDescriptor
    // doxygen for the rasterizer-output struct contract.
    static const char *kBlitFullscreenVsOmegaSL_Metal = R"(
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

    bool GEMetalEngine::ensureBlitFullscreenVs() {
        if (blitFullscreenVs) return true;
        try {
            auto compiler = OmegaSLCompiler::Create(gteDevice);
            if (!compiler) {
                DEBUG_STREAM("ensureBlitFullscreenVs: OmegaSLCompiler::Create returned null");
                return false;
            }
            OmegaCommon::String src(kBlitFullscreenVsOmegaSL_Metal);
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


    SharedHandle<OmegaGraphicsEngine> CreateMetalEngine(SharedHandle<GTEDevice> & device){
        return std::shared_ptr<OmegaGraphicsEngine>(new GEMetalEngine(device));
    };
_NAMESPACE_END_
