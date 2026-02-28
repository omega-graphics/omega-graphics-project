#ifndef OMEGAGTE_GERESOURCETRACKER_H
#define OMEGAGTE_GERESOURCETRACKER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace OmegaGTE::ResourceTracking {

enum class Backend : std::uint8_t {
    Common,
    Metal,
    D3D12,
    Vulkan
};

enum class EventType : std::uint8_t {
    Create,
    Destroy,
    Submit,
    Complete,
    Present,
    ResizeRebuild,
    Bind,
    Unbind,
    Marker
};

struct Event {
    std::uint64_t timestampMs = 0;
    Backend backend = Backend::Common;
    EventType eventType = EventType::Marker;
    std::string resourceType {};
    std::uint64_t resourceId = 0;
    std::uint64_t queueId = 0;
    std::uint64_t commandBufferId = 0;
    std::uint64_t nativeHandle = 0;
    std::uint64_t threadId = 0;
    float width = -1.f;
    float height = -1.f;
    float scale = -1.f;
};

struct TypeChurnMetrics {
    Backend backend = Backend::Common;
    std::string resourceType {};
    std::uint64_t createCount = 0;
    std::uint64_t destroyCount = 0;
    std::uint64_t aliveCount = 0;
    std::uint64_t shortLifetimeCount = 0;
    std::uint64_t startupCreateCount = 0;
    std::uint64_t startupDestroyCount = 0;
    std::uint64_t lifetimeSampleCount = 0;
    std::uint64_t totalLifetimeMs = 0;
    std::uint64_t minLifetimeMs = 0;
    std::uint64_t maxLifetimeMs = 0;
    double averageLifetimeMs = 0.0;
};

struct ChurnMetricsSnapshot {
    std::uint64_t trackerStartTimestampMs = 0;
    std::uint64_t snapshotTimestampMs = 0;
    std::uint64_t startupWindowMs = 0;
    std::uint64_t startupBucketMs = 0;
    std::uint64_t shortLifetimeThresholdMs = 0;
    std::uint64_t totalCreateCount = 0;
    std::uint64_t totalDestroyCount = 0;
    std::uint64_t totalShortLifetimeCount = 0;
    std::uint64_t peakStartupBurstCount = 0;
    std::uint64_t peakStartupBurstBucketOffsetMs = 0;
    std::vector<TypeChurnMetrics> perType {};
};

class Tracker {
public:
    static Tracker & instance();

    bool enabled() const;
    std::uint64_t nextResourceId();

    void emit(const Event & event);
    void emit(EventType eventType,
              Backend backend,
              const char *resourceType,
              std::uint64_t resourceId,
              const void *nativeHandle,
              float width = -1.f,
              float height = -1.f,
              float scale = -1.f);

    std::vector<Event> recentEvents(std::size_t maxEvents = 0) const;
    ChurnMetricsSnapshot churnMetricsSnapshot() const;
    std::string dumpRecentTimeline(std::size_t maxEvents = 0) const;
    std::string dumpChurnMetrics() const;
    void clear();

private:
    Tracker();
    ~Tracker() = default;
    Tracker(const Tracker &) = delete;
    Tracker & operator=(const Tracker &) = delete;
};

const char * backendName(Backend backend);
const char * eventTypeName(EventType eventType);

}

#endif
