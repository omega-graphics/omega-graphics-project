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

    /// Raytracing plan §6-M2/M3 — the MTLAccelerationStructureDescriptor this
    /// structure was SIZED against, retained (+1 from alloc/init, released in
    /// the destructor — this backend is MRR, not ARC). Metal derives an
    /// acceleration structure's size from its descriptor, so the build and refit
    /// commands must use the very same descriptor the allocation was sized for;
    /// keeping it here is what guarantees that. (Before this, allocate and build
    /// each hand-rolled their own EMPTY descriptor, so no geometry ever reached
    /// the GPU.)
    NSSmartPtr descriptor;

    /// TLAS only. The MTLAccelerationStructureInstanceDescriptor array backing
    /// `instanceDescriptorBuffer`. Owned here so it outlives the recorded build
    /// command — the same lifetime rule D3D12's Upload-heap instance buffer
    /// follows.
    SharedHandle<GEMetalBuffer> instanceBuffer;

    /// TLAS only. The BLAS this TLAS instances, in `instancedAccelerationStructures`
    /// order (an instance's `accelerationStructureIndex` indexes this). Held for
    /// two reasons: it keeps them alive for as long as the TLAS references them,
    /// and Metal requires every referenced BLAS to be made RESIDENT on the
    /// encoder (`useResource:`) before a shader can trace the TLAS — without that
    /// the traversal reads nothing and every ray misses.
    OmegaCommon::Vector<SharedHandle<GEAccelerationStruct>> blasRefs;

    bool isTopLevel = false;

    explicit GEMetalAccelerationStruct(NSSmartPtr & accelStruct,
    SharedHandle<GEMetalBuffer> & scratchBuffer);
    ~GEMetalAccelerationStruct() override;
};

/// Raytracing plan §6-M3 — write `desc`'s instances into `dst` as
/// MTLAccelerationStructureInstanceDescriptor records (3x4 row-major GE transform
/// transposed into Metal's column-major MTLPackedFloat4x3). Shared by the initial
/// fill at allocation and by refit, so an updated transform cannot drift from the
/// instance layout the TLAS was built with.
void fillMetalTLASInstances(const GEAccelerationStructDescriptor &desc,
                            GEMetalBuffer *dst);


SharedHandle<OmegaGraphicsEngine> CreateMetalEngine(SharedHandle<GTEDevice> & device);
_NAMESPACE_END_



#endif
