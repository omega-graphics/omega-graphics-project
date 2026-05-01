#include "GERetentionQueue.h"

#include <algorithm>
#include <cassert>

namespace OmegaGTE::Retention {

Queue::~Queue() {
    // By contract the engine destructor calls drainAll() before destroying
    // the queue. A non-empty queue here is a programmer bug — running the
    // releases now is unsafe (GPU may not be idle), and silently leaking is
    // the lesser evil. D3D12MA / VMA leak validation will surface anything
    // that gets dropped here.
    assert(entries_.empty() && "Retention::Queue destroyed with pending entries");
}

void Queue::enqueue(std::vector<FenceGate> gates,
                    std::function<void()> release) {
    Entry e;
    e.gates   = std::move(gates);
    e.release = std::move(release);
    std::lock_guard<std::mutex> lk(mtx_);
    entries_.push_back(std::move(e));
}

std::size_t Queue::drainCompleted() {
    std::vector<std::function<void()>> toRun;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = entries_.begin();
        while (it != entries_.end()) {
            const bool allSignaled = std::all_of(
                it->gates.begin(), it->gates.end(),
                [](const FenceGate& g) { return !g || g(); });
            if (allSignaled) {
                toRun.push_back(std::move(it->release));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& r : toRun) {
        if (r) r();
    }
    return toRun.size();
}

std::size_t Queue::drainAll() {
    std::deque<Entry> drained;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        drained.swap(entries_);
    }
    for (auto& e : drained) {
        if (e.release) e.release();
    }
    return drained.size();
}

std::size_t Queue::pending() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return entries_.size();
}

} // namespace OmegaGTE::Retention
