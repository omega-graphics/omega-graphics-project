#include "omegaGTE/GE.h"
#include <iostream>

#ifndef OMEGAGTE_METAL_GEMETAL_H
#define OMEGAGTE_METAL_GEMETAL_H

#if defined(TARGET_METAL) && defined(__OBJC__)
@protocol MTLBuffer;
@protocol MTLFence;
#endif

_NAMESPACE_BEGIN_

struct NSObjectHandle {
    const void *data;
};

class NSSmartPtr {
    const void * data = nullptr;
public:
    NSSmartPtr() = default;
    NSSmartPtr(const NSObjectHandle & handle);
    inline const void* handle() const { return data; }
    void assertExists();
};

#define NSOBJECT_OBJC_BRIDGE(t,o)((__bridge t)o) 
#define NSOBJECT_CPP_BRIDGE (__bridge void *)

class GEMetalBuffer : public GEBuffer {
public:
    NSSmartPtr metalBuffer;
    NSSmartPtr layoutDesc;

    NSSmartPtr resourceBarrier;

    bool needsBarrier = false;

    size_t size() override;
    void setName(OmegaCommon::StrRef name) override;
    void *native() override {
        return const_cast<void *>(metalBuffer.handle());
    }
    GEMetalBuffer(const BufferDescriptor::Usage & usage,NSSmartPtr & buffer,NSSmartPtr & layoutDesc);
    ~GEMetalBuffer() override;
};

class GEMetalFence : public GEFence {
public:
    NSSmartPtr metalEvent;
    void setName(OmegaCommon::StrRef name) override;
    void *native() override{
        return const_cast<void*>(metalEvent.handle());
    }
    explicit GEMetalFence(NSSmartPtr & event);
};

struct GEMetalSamplerState : public GESamplerState {
    NSSmartPtr samplerState;
    void *native() {
        return const_cast<void *>(samplerState.handle());
    }
    GEMetalSamplerState(NSSmartPtr & samplerState);
};

#ifdef OMEGAGTE_RAYTRACING_SUPPORTED

struct GEMetalAccelerationStruct : public GEAccelerationStruct {
    NSSmartPtr accelStruct;
    SharedHandle<GEMetalBuffer> scratchBuffer;

    explicit GEMetalAccelerationStruct(NSSmartPtr & accelStruct,
    SharedHandle<GEMetalBuffer> & scratchBuffer);
    ~GEMetalAccelerationStruct() override = default;
};

#endif


SharedHandle<OmegaGraphicsEngine> CreateMetalEngine(SharedHandle<GTEDevice> & device);
_NAMESPACE_END_



#endif

