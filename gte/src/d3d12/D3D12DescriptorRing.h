#ifndef OMEGAGTE_D3D12_DESCRIPTOR_RING_H
#define OMEGAGTE_D3D12_DESCRIPTOR_RING_H

// `omegaGTE/GE.h` must come before any Windows / D3D12 / ATL header
// because `omegasl.h` (transitively included) declares
// `using CString = const char *;` at file scope, which collides with
// ATL's `CString` class. Mirror D3D12DescriptorAllocator's ordering.
#include "omegaGTE/GE.h"

#include "D3D12DescriptorAllocator.h"
#include "../common/GERetentionQueue.h"

#include "d3dx12.h"
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

_NAMESPACE_BEGIN_
    using Microsoft::WRL::ComPtr;

    /// Bump-only, fence-gated transient descriptor allocator over a single
    /// shader-visible heap. Phase 3 of the Shared-Descriptor-Heap-Plan —
    /// one-shot dispatches (`D3D12TEContext` tessellation,
    /// `GED3D12CommandBuffer::generateMipmaps`) suballocate the N descriptor
    /// slots they need per dispatch instead of allocating a fresh
    /// ID3D12DescriptorHeap each time.
    ///
    /// Lifecycle:
    ///   1. Caller calls `allocate(count, gate)` to reserve `count`
    ///      consecutive slots; the returned handle's `cpu` / `gpu` point
    ///      into the ring's heap. The caller writes descriptors into
    ///      those slots, binds the ring's heap via `SetDescriptorHeaps`,
    ///      and issues its dispatch.
    ///   2. `gate` is a `Retention::FenceGate` that returns true once the
    ///      GPU has finished with the slots — typically a snapshot of the
    ///      upcoming submission's signal value (`gateForNextSubmit()` on
    ///      the command queue) or a closure over a private wait-fence for
    ///      synchronous paths (`D3D12TEContext`).
    ///   3. `retire()` is called periodically (the owning queue's
    ///      `drainCompleted()` hooks, plus directly after the synchronous
    ///      wait in `D3D12TEContext`). Each call walks the pending-stamp
    ///      deque from the front and advances the read cursor over any
    ///      stamp whose gate signals.
    ///
    /// Allocations are always contiguous. If a request would straddle the
    /// wrap boundary, the trailing unused slots are skipped (stamped as
    /// part of the same allocation so they retire together) and the
    /// allocation is placed at slot 0.
    ///
    /// On exhaustion (in-flight slots >= capacity), `allocate` returns an
    /// invalid handle. Callers fall back exactly as they do for
    /// `D3D12DescriptorAllocator::allocate` — typically by either skipping
    /// the work or, for paths that absolutely must run, allocating a
    /// one-off heap (the pre-Phase-3 behavior). Phase 3 sizes the ring
    /// generously (4096 slots) so exhaustion is rare.
    ///
    /// Thread-safety: `allocate` / `retire` are guarded by an internal
    /// mutex. The ring is conceptually owned by one command queue;
    /// callers on other threads work fine but the dispatch they record
    /// must subsequently be submitted by that queue.
    class D3D12DescriptorRing {
    public:
        D3D12DescriptorRing(ID3D12Device *device,
                            D3D12_DESCRIPTOR_HEAP_TYPE type,
                            std::uint32_t capacity);
        ~D3D12DescriptorRing();

        D3D12DescriptorRing(const D3D12DescriptorRing &) = delete;
        D3D12DescriptorRing &operator=(const D3D12DescriptorRing &) = delete;

        /// Reserve `count` contiguous slots from the ring. `gate` is
        /// queried at `retire()` time to decide when the slots are safe
        /// to recycle. Returns an invalid handle if the ring is full.
        D3D12DescriptorHandle allocate(std::uint32_t count,
                                       Retention::FenceGate gate);

        /// Walk pending stamps from the front; advance the read cursor
        /// over any whose gate signals. Cheap when nothing has completed.
        /// Safe to call at any rate — every per-queue drainCompleted hook
        /// in `GED3D12CommandQueue` calls this, and `D3D12TEContext`
        /// also calls it directly after its synchronous wait.
        void retire();

        /// The underlying shader-visible heap. Bind this once per
        /// dispatch via `SetDescriptorHeaps`; descriptor table calls
        /// pass the handle's GPU descriptor directly.
        ID3D12DescriptorHeap *heap() const { return heap_.Get(); }

        std::uint32_t capacity() const { return capacity_; }

    private:
        struct PendingStamp {
            std::uint32_t        count;   // includes any skipped wrap-pad
            Retention::FenceGate gate;
        };

        D3D12DescriptorHandle makeHandle(std::uint32_t physicalIndex,
                                          std::uint32_t count) const;

        mutable std::mutex             mutex_;
        ComPtr<ID3D12DescriptorHeap>   heap_;
        D3D12_CPU_DESCRIPTOR_HANDLE    cpuStart_{};
        D3D12_GPU_DESCRIPTOR_HANDLE    gpuStart_{};
        std::uint32_t                  incrementSize_ = 0;
        std::uint32_t                  capacity_      = 0;

        // Monotonic logical cursors. Physical index = cursor % capacity_.
        // headLogical_ - tailLogical_ is the live in-flight slot count.
        // 64-bit is far more than enough headroom: at 1B allocations per
        // second the counters wrap in ~580 years.
        std::uint64_t                  headLogical_ = 0;
        std::uint64_t                  tailLogical_ = 0;

        std::deque<PendingStamp>       pendingStamps_;
    };

_NAMESPACE_END_

#endif // OMEGAGTE_D3D12_DESCRIPTOR_RING_H
