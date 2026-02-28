#ifndef OMEGAWTK_COMPOSITION_BACKEND_RESOURCETRACE_H
#define OMEGAWTK_COMPOSITION_BACKEND_RESOURCETRACE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace OmegaWTK::Composition::ResourceTrace {

inline bool enabled(){
    static const bool traceEnabled = []{
        const char *raw = std::getenv("OMEGAWTK_GPU_RESOURCE_TRACE");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();
    return traceEnabled;
}

inline std::uint64_t timestampMs(){
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

inline std::uint64_t nextResourceId(){
    static std::atomic<std::uint64_t> nextId {1};
    return nextId.fetch_add(1,std::memory_order_relaxed);
}

inline void emit(const char *eventType,
                 const char *resourceType,
                 std::uint64_t resourceId,
                 const char *ownerKind,
                 const void *ownerKey,
                 float width = -1.f,
                 float height = -1.f,
                 float scale = -1.f){
    if(!enabled()){
        return;
    }
    std::ostringstream ss;
    ss << "[OmegaWTKGPU] ts=" << timestampMs()
       << " event=" << eventType
       << " type=" << resourceType
       << " id=" << resourceId
       << " ownerKind=" << ownerKind
       << " ownerKey=" << reinterpret_cast<std::uintptr_t>(ownerKey);
    if(width >= 0.f){
        ss << " width=" << width;
    }
    if(height >= 0.f){
        ss << " height=" << height;
    }
    if(scale >= 0.f){
        ss << " scale=" << scale;
    }
    std::cout << ss.str() << std::endl;
}

}

#endif
