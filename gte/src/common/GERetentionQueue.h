#ifndef OMEGAGTE_GERETENTIONQUEUE_H
#define OMEGAGTE_GERETENTIONQUEUE_H

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include <omega-common/utils.h>

namespace OmegaGTE::Retention {

// A GPU-side gate. Backends supply a callable that returns true once the GPU
// has signaled the gate (D3D12: ID3D12Fence::GetCompletedValue >= V; Vulkan:
// vkGetSemaphoreCounterValue >= V on a timeline semaphore). Queried at drain
// time; must be cheap and side-effect-free.
using FenceGate = std::function<bool()>;

struct Entry {
    std::vector<FenceGate>  gates;
    std::function<void()>   release;
};

// Engine-owned, thread-safe deferred-release queue.
//
// Lifecycle:
//   1. Encoders call enqueue() / retainShared() when they record GPU work
//      that uses a resource, supplying the fence gate(s) the resource must
//      outlive.
//   2. Command-queue submit paths call drainCompleted() after submitting,
//      which runs releases for any entries whose gates have all signaled.
//   3. Engine shutdown waits for every command queue to go idle, then calls
//      drainAll() to release everything before tearing down backend
//      allocators (D3D12MA::Allocator, VmaAllocator).
//
// The internal mutex is dropped before user releases run, so a release that
// itself enqueues will not deadlock.
class Queue {
public:
    Queue() = default;
    ~Queue();

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    Queue(Queue&&) = delete;
    Queue& operator=(Queue&&) = delete;

    void enqueue(std::vector<FenceGate> gates,
                 std::function<void()> release);

    // Keeps `handle` alive until every gate signals; the captured shared_ptr
    // is dropped at drain time, running the wrapper's destructor on a
    // GPU-safe thread.
    template <class T>
    void retainShared(SharedHandle<T> handle,
                      std::vector<FenceGate> gates) {
        enqueue(std::move(gates),
                [h = std::move(handle)]() mutable { h.reset(); });
    }

    // Pop and run entries whose gates have all signaled. Returns release count.
    std::size_t drainCompleted();

    // Run every pending release, ignoring gates. Caller must guarantee the
    // GPU is idle (every gate would signal). Returns release count.
    std::size_t drainAll();

    // Diagnostic only.
    std::size_t pending() const;

private:
    mutable std::mutex   mtx_;
    std::deque<Entry>    entries_;
};

} // namespace OmegaGTE::Retention

#endif // OMEGAGTE_GERETENTIONQUEUE_H
