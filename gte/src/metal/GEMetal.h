#include "omegaGTE/GE.h"
#include <iostream>
#include <atomic>

#ifndef OMEGAGTE_METAL_GEMETAL_H
#define OMEGAGTE_METAL_GEMETAL_H

#if defined(TARGET_METAL) && defined(__OBJC__)
@protocol MTLBuffer;
@protocol MTLFence;
// GTE_NSLOG retired (Debug-Layer-Plan §4.3): the Metal command-queue logging
// now goes through the typed DEBUG_TRACE/DEBUG_INFO/DEBUG_ERROR/DEBUG_CRITICAL
// macros in omegaGTE/GE.h, which gate on the same debug-layer flag (and add
// level + domain filtering). Use those instead of NSLog for engine logging.
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
    std::uint64_t traceResourceId = 0;

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
private:
    std::atomic_uint64_t eventValue;
public:
    inline uint64_t currentEventValue() const {
        return eventValue.load(std::memory_order_acquire);
    }
    inline uint64_t reserveNextEventValue() {
        return eventValue.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
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


struct GEMetalAccelerationStruct : public GEAccelerationStruct {
    NSSmartPtr accelStruct;
    SharedHandle<GEMetalBuffer> scratchBuffer;

    explicit GEMetalAccelerationStruct(NSSmartPtr & accelStruct,
    SharedHandle<GEMetalBuffer> & scratchBuffer);
    ~GEMetalAccelerationStruct() override = default;
};


SharedHandle<OmegaGraphicsEngine> CreateMetalEngine(SharedHandle<GTEDevice> & device);
_NAMESPACE_END_



#endif
