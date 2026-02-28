#include "GEResourceTracker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace OmegaGTE::ResourceTracking {

namespace {
    class TrackerState {
    public:
        std::atomic<std::uint64_t> nextId {1};
        const bool traceEnabled;
        mutable std::mutex lock {};
        std::deque<Event> recent {};
        static constexpr std::size_t kMaxRecentEvents = 4096;

        TrackerState() : traceEnabled([]{
            const char *raw = std::getenv("OMEGAGTE_RESOURCE_TRACE");
            return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
        }()) {}
    };

    static TrackerState & state(){
        static TrackerState s {};
        return s;
    }

    static inline std::uint64_t nowMs(){
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
                duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }

    static inline std::uint64_t currentThreadHash(){
        return static_cast<std::uint64_t>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    static inline void pushRecentEvent(const Event &event){
        auto &s = state();
        std::lock_guard<std::mutex> guard(s.lock);
        if(s.recent.size() >= TrackerState::kMaxRecentEvents){
            s.recent.pop_front();
        }
        s.recent.push_back(event);
    }
}

const char * backendName(Backend backend){
    switch(backend){
        case Backend::Common:
            return "Common";
        case Backend::Metal:
            return "Metal";
        case Backend::D3D12:
            return "D3D12";
        case Backend::Vulkan:
            return "Vulkan";
        default:
            return "Unknown";
    }
}

const char * eventTypeName(EventType eventType){
    switch(eventType){
        case EventType::Create:
            return "Create";
        case EventType::Destroy:
            return "Destroy";
        case EventType::Submit:
            return "Submit";
        case EventType::Complete:
            return "Complete";
        case EventType::Present:
            return "Present";
        case EventType::ResizeRebuild:
            return "ResizeRebuild";
        case EventType::Bind:
            return "Bind";
        case EventType::Unbind:
            return "Unbind";
        case EventType::Marker:
            return "Marker";
        default:
            return "Unknown";
    }
}

Tracker & Tracker::instance(){
    static Tracker tracker {};
    return tracker;
}

Tracker::Tracker() = default;

bool Tracker::enabled() const{
    return state().traceEnabled;
}

std::uint64_t Tracker::nextResourceId(){
    return state().nextId.fetch_add(1,std::memory_order_relaxed);
}

void Tracker::emit(const Event &event){
    Event normalized = event;
    if(normalized.timestampMs == 0){
        normalized.timestampMs = nowMs();
    }
    if(normalized.threadId == 0){
        normalized.threadId = currentThreadHash();
    }
    if(normalized.resourceType.empty()){
        normalized.resourceType = "Unknown";
    }

    pushRecentEvent(normalized);

    if(!enabled()){
        return;
    }

    std::ostringstream ss;
    ss << "[OmegaGTEResource] ts=" << normalized.timestampMs
       << " backend=" << backendName(normalized.backend)
       << " event=" << eventTypeName(normalized.eventType)
       << " type=" << normalized.resourceType
       << " id=" << normalized.resourceId
       << " nativeHandle=" << normalized.nativeHandle
       << " threadId=" << normalized.threadId;
    if(normalized.width >= 0.f){
        ss << " width=" << normalized.width;
    }
    if(normalized.height >= 0.f){
        ss << " height=" << normalized.height;
    }
    if(normalized.scale >= 0.f){
        ss << " scale=" << normalized.scale;
    }
    std::cout << ss.str() << std::endl;
}

void Tracker::emit(EventType eventType,
                   Backend backend,
                   const char *resourceType,
                   std::uint64_t resourceId,
                   const void *nativeHandle,
                   float width,
                   float height,
                   float scale){
    Event event {};
    event.eventType = eventType;
    event.backend = backend;
    event.resourceType = resourceType != nullptr ? resourceType : "Unknown";
    event.resourceId = resourceId;
    event.nativeHandle = reinterpret_cast<std::uint64_t>(nativeHandle);
    event.width = width;
    event.height = height;
    event.scale = scale;
    emit(event);
}

std::vector<Event> Tracker::recentEvents(std::size_t maxEvents) const{
    auto &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    if(maxEvents == 0 || maxEvents >= s.recent.size()){
        return std::vector<Event>(s.recent.begin(),s.recent.end());
    }
    const auto firstIdx = s.recent.size() - maxEvents;
    return std::vector<Event>(s.recent.begin() + static_cast<std::ptrdiff_t>(firstIdx),s.recent.end());
}

void Tracker::clear(){
    auto &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.recent.clear();
}

}
