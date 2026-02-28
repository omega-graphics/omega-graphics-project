#include "GEResourceTracker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace OmegaGTE::ResourceTracking {

namespace {
    static constexpr std::uint64_t kShortLifetimeThresholdMs = 250;
    static constexpr std::uint64_t kStartupWindowMs = 5000;
    static constexpr std::uint64_t kStartupBucketMs = 100;

    static inline std::uint64_t nowMs(){
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
                duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }

    static inline std::uint64_t currentThreadHash(){
        return static_cast<std::uint64_t>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    struct TypeKey {
        Backend backend = Backend::Common;
        std::string resourceType {};

        bool operator==(const TypeKey &other) const {
            return backend == other.backend && resourceType == other.resourceType;
        }
    };

    struct TypeKeyHash {
        std::size_t operator()(const TypeKey &key) const {
            std::size_t seed = std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(key.backend));
            seed ^= std::hash<std::string>{}(key.resourceType) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    struct TypeChurnAccumulator {
        std::uint64_t createCount = 0;
        std::uint64_t destroyCount = 0;
        std::uint64_t aliveCount = 0;
        std::uint64_t shortLifetimeCount = 0;
        std::uint64_t startupCreateCount = 0;
        std::uint64_t startupDestroyCount = 0;
        std::uint64_t lifetimeSampleCount = 0;
        std::uint64_t totalLifetimeMs = 0;
        std::uint64_t minLifetimeMs = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t maxLifetimeMs = 0;
    };

    struct LiveResourceRecord {
        TypeKey typeKey {};
        std::uint64_t createdAtMs = 0;
    };

    class TrackerState {
    public:
        std::atomic<std::uint64_t> nextId {1};
        const bool traceEnabled;
        mutable std::mutex lock {};
        std::deque<Event> recent {};
        std::unordered_map<TypeKey,TypeChurnAccumulator,TypeKeyHash> typeChurn {};
        std::unordered_map<std::uint64_t,LiveResourceRecord> liveResources {};
        std::uint64_t trackerStartTimestampMs = 0;
        std::uint64_t totalCreateCount = 0;
        std::uint64_t totalDestroyCount = 0;
        std::uint64_t totalShortLifetimeCount = 0;
        std::vector<std::uint64_t> startupBurstBuckets {};
        std::uint64_t peakStartupBurstCount = 0;
        std::uint64_t peakStartupBurstBucketOffsetMs = 0;

        static constexpr std::size_t kMaxRecentEvents = 4096;

        TrackerState() : traceEnabled([]{
            const char *raw = std::getenv("OMEGAGTE_RESOURCE_TRACE");
            return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
        }()) {
            resetMetrics();
        }

        void resetMetrics(){
            typeChurn.clear();
            liveResources.clear();
            totalCreateCount = 0;
            totalDestroyCount = 0;
            totalShortLifetimeCount = 0;
            trackerStartTimestampMs = nowMs();
            const std::size_t startupBucketCount =
                    static_cast<std::size_t>((kStartupWindowMs / kStartupBucketMs) + 1);
            startupBurstBuckets.assign(startupBucketCount,0);
            peakStartupBurstCount = 0;
            peakStartupBurstBucketOffsetMs = 0;
        }
    };

    static TrackerState &state(){
        static TrackerState s {};
        return s;
    }

    static inline void updateStartupBurst(TrackerState &s,const Event &event){
        if(event.eventType != EventType::Create){
            return;
        }
        if(event.timestampMs < s.trackerStartTimestampMs){
            return;
        }
        const auto elapsedMs = event.timestampMs - s.trackerStartTimestampMs;
        if(elapsedMs > kStartupWindowMs || s.startupBurstBuckets.empty()){
            return;
        }
        const std::size_t bucketIndex = std::min<std::size_t>(
                static_cast<std::size_t>(elapsedMs / kStartupBucketMs),
                s.startupBurstBuckets.size() - 1);
        const auto bucketCount = ++s.startupBurstBuckets[bucketIndex];
        if(bucketCount > s.peakStartupBurstCount){
            s.peakStartupBurstCount = bucketCount;
            s.peakStartupBurstBucketOffsetMs = static_cast<std::uint64_t>(bucketIndex) * kStartupBucketMs;
        }
    }

    static inline bool isWithinStartupWindow(const TrackerState &s,const Event &event){
        if(event.timestampMs < s.trackerStartTimestampMs){
            return false;
        }
        return (event.timestampMs - s.trackerStartTimestampMs) <= kStartupWindowMs;
    }

    static inline void updateMetrics(TrackerState &s,const Event &event){
        if(event.resourceType.empty()){
            return;
        }

        const TypeKey key {event.backend,event.resourceType};
        auto &metrics = s.typeChurn[key];

        switch(event.eventType){
            case EventType::Create: {
                ++metrics.createCount;
                ++metrics.aliveCount;
                ++s.totalCreateCount;
                if(isWithinStartupWindow(s,event)){
                    ++metrics.startupCreateCount;
                }
                updateStartupBurst(s,event);
                if(event.resourceId != 0){
                    s.liveResources[event.resourceId] = LiveResourceRecord {key,event.timestampMs};
                }
                break;
            }
            case EventType::Destroy: {
                ++metrics.destroyCount;
                if(metrics.aliveCount > 0){
                    --metrics.aliveCount;
                }
                ++s.totalDestroyCount;
                if(isWithinStartupWindow(s,event)){
                    ++metrics.startupDestroyCount;
                }

                if(event.resourceId != 0){
                    const auto liveIt = s.liveResources.find(event.resourceId);
                    if(liveIt != s.liveResources.end()){
                        std::uint64_t lifetimeMs = 0;
                        if(event.timestampMs > liveIt->second.createdAtMs){
                            lifetimeMs = event.timestampMs - liveIt->second.createdAtMs;
                        }
                        ++metrics.lifetimeSampleCount;
                        metrics.totalLifetimeMs += lifetimeMs;
                        metrics.minLifetimeMs = std::min(metrics.minLifetimeMs,lifetimeMs);
                        metrics.maxLifetimeMs = std::max(metrics.maxLifetimeMs,lifetimeMs);

                        if(lifetimeMs <= kShortLifetimeThresholdMs){
                            ++metrics.shortLifetimeCount;
                            ++s.totalShortLifetimeCount;
                        }
                        s.liveResources.erase(liveIt);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    static inline void pushRecentEventAndMetrics(const Event &event){
        auto &s = state();
        std::lock_guard<std::mutex> guard(s.lock);

        if(s.recent.size() >= TrackerState::kMaxRecentEvents){
            s.recent.pop_front();
        }
        s.recent.push_back(event);
        updateMetrics(s,event);
    }
}

const char *backendName(Backend backend){
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

const char *eventTypeName(EventType eventType){
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

Tracker &Tracker::instance(){
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

    pushRecentEventAndMetrics(normalized);

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
    if(normalized.queueId != 0){
        ss << " queueId=" << normalized.queueId;
    }
    if(normalized.commandBufferId != 0){
        ss << " cmdBufferId=" << normalized.commandBufferId;
    }
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
    return std::vector<Event>(
            s.recent.begin() + static_cast<std::ptrdiff_t>(firstIdx),
            s.recent.end());
}

ChurnMetricsSnapshot Tracker::churnMetricsSnapshot() const{
    auto &s = state();
    std::lock_guard<std::mutex> guard(s.lock);

    ChurnMetricsSnapshot snapshot {};
    snapshot.trackerStartTimestampMs = s.trackerStartTimestampMs;
    snapshot.snapshotTimestampMs = nowMs();
    snapshot.startupWindowMs = kStartupWindowMs;
    snapshot.startupBucketMs = kStartupBucketMs;
    snapshot.shortLifetimeThresholdMs = kShortLifetimeThresholdMs;
    snapshot.totalCreateCount = s.totalCreateCount;
    snapshot.totalDestroyCount = s.totalDestroyCount;
    snapshot.totalShortLifetimeCount = s.totalShortLifetimeCount;
    snapshot.peakStartupBurstCount = s.peakStartupBurstCount;
    snapshot.peakStartupBurstBucketOffsetMs = s.peakStartupBurstBucketOffsetMs;
    snapshot.perType.reserve(s.typeChurn.size());

    for(const auto &[key,acc] : s.typeChurn){
        TypeChurnMetrics metrics {};
        metrics.backend = key.backend;
        metrics.resourceType = key.resourceType;
        metrics.createCount = acc.createCount;
        metrics.destroyCount = acc.destroyCount;
        metrics.aliveCount = acc.aliveCount;
        metrics.shortLifetimeCount = acc.shortLifetimeCount;
        metrics.startupCreateCount = acc.startupCreateCount;
        metrics.startupDestroyCount = acc.startupDestroyCount;
        metrics.lifetimeSampleCount = acc.lifetimeSampleCount;
        metrics.totalLifetimeMs = acc.totalLifetimeMs;
        metrics.minLifetimeMs =
                acc.lifetimeSampleCount > 0 ? acc.minLifetimeMs : 0;
        metrics.maxLifetimeMs = acc.maxLifetimeMs;
        metrics.averageLifetimeMs =
                acc.lifetimeSampleCount > 0
                        ? static_cast<double>(acc.totalLifetimeMs) /
                                  static_cast<double>(acc.lifetimeSampleCount)
                        : 0.0;
        snapshot.perType.push_back(std::move(metrics));
    }

    std::sort(snapshot.perType.begin(),snapshot.perType.end(),
              [](const TypeChurnMetrics &lhs,const TypeChurnMetrics &rhs){
                  if(lhs.backend != rhs.backend){
                      return static_cast<std::uint8_t>(lhs.backend) <
                             static_cast<std::uint8_t>(rhs.backend);
                  }
                  return lhs.resourceType < rhs.resourceType;
              });

    return snapshot;
}

std::string Tracker::dumpRecentTimeline(std::size_t maxEvents) const{
    const auto events = recentEvents(maxEvents);
    std::ostringstream ss;
    ss << "OmegaGTE Resource Timeline (events=" << events.size() << ")\n";
    for(const auto &event : events){
        ss << "ts=" << event.timestampMs
           << " backend=" << backendName(event.backend)
           << " event=" << eventTypeName(event.eventType)
           << " type=" << event.resourceType
           << " id=" << event.resourceId;
        if(event.queueId != 0){
            ss << " queueId=" << event.queueId;
        }
        if(event.commandBufferId != 0){
            ss << " cmdBufferId=" << event.commandBufferId;
        }
        if(event.nativeHandle != 0){
            ss << " native=0x" << std::hex << event.nativeHandle << std::dec;
        }
        ss << " threadId=" << event.threadId;
        if(event.width >= 0.f || event.height >= 0.f || event.scale >= 0.f){
            ss << " dims=(" << event.width << "," << event.height
               << "," << event.scale << ")";
        }
        ss << "\n";
    }
    return ss.str();
}

std::string Tracker::dumpChurnMetrics() const{
    const auto snapshot = churnMetricsSnapshot();
    std::ostringstream ss;
    ss << "OmegaGTE Resource Churn Metrics\n";
    ss << "trackerStartTs=" << snapshot.trackerStartTimestampMs
       << " snapshotTs=" << snapshot.snapshotTimestampMs
       << " startupWindowMs=" << snapshot.startupWindowMs
       << " startupBucketMs=" << snapshot.startupBucketMs
       << " shortLifetimeThresholdMs=" << snapshot.shortLifetimeThresholdMs
       << "\n";

    ss << "totalCreates=" << snapshot.totalCreateCount
       << " totalDestroys=" << snapshot.totalDestroyCount
       << " totalShortLifetimes=" << snapshot.totalShortLifetimeCount
       << " peakStartupBurst=" << snapshot.peakStartupBurstCount
       << " peakStartupOffsetMs=" << snapshot.peakStartupBurstBucketOffsetMs
       << "\n";

    for(const auto &entry : snapshot.perType){
        ss << "type=" << backendName(entry.backend) << ":" << entry.resourceType
           << " creates=" << entry.createCount
           << " destroys=" << entry.destroyCount
           << " alive=" << entry.aliveCount
           << " shortLifetimes=" << entry.shortLifetimeCount
           << " startupCreates=" << entry.startupCreateCount
           << " startupDestroys=" << entry.startupDestroyCount
           << " lifetimeSamples=" << entry.lifetimeSampleCount
           << " lifetimeMinMs=" << entry.minLifetimeMs
           << " lifetimeMaxMs=" << entry.maxLifetimeMs
           << " lifetimeAvgMs=" << std::fixed << std::setprecision(2)
           << entry.averageLifetimeMs
           << "\n";
    }

    return ss.str();
}

void Tracker::clear(){
    auto &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.recent.clear();
    s.resetMetrics();
}

}
