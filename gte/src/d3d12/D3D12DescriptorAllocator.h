#ifndef OMEGAGTE_D3D12_DESCRIPTOR_ALLOCATOR_H
#define OMEGAGTE_D3D12_DESCRIPTOR_ALLOCATOR_H

// `omegaGTE/GE.h` must come before any Windows / D3D12 / ATL header
// because `omegasl.h` (transitively included by GE.h) defines
// `using CString = const char *;` at file scope, which collides with the
// `CString` class ATL declares once `<atlstr.h>` is pulled in. GED3D12.h
// follows the same ordering; mirror it here so this header is safe to
// include in any TU regardless of what was included before us.
#include "omegaGTE/GE.h"

#include "d3dx12.h"
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>

_NAMESPACE_BEGIN_
    using Microsoft::WRL::ComPtr;

    /// One slot (or contiguous run of slots) inside a shader-visible
    /// descriptor heap owned by a `D3D12DescriptorAllocator`. `index ==
    /// UINT32_MAX` is the sentinel for an allocation that failed (capacity
    /// exhausted past the allocator's growth cap); callers may treat such
    /// a handle as "no descriptor" and fall back to whatever graceful-
    /// degradation path they use.
    ///
    /// `block` selects which of the allocator's underlying heaps this slot
    /// lives in. Single-block allocators always set this to 0; the bind
    /// path resolves the actual heap via `allocator->heap(handle.block)`.
    struct D3D12DescriptorHandle {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        std::uint32_t index = UINT32_MAX;
        std::uint32_t count = 0;
        std::uint32_t block = 0;

        bool valid() const { return index != UINT32_MAX; }
    };

    /// Growable free-list allocator over one or more shader-visible D3D12
    /// descriptor heaps of a single type. Resource creation (texture /
    /// sampler) suballocates slots; slots are returned via `free()`,
    /// gated externally (the engine's `Retention::Queue` ensures every
    /// command list that referenced a slot has retired before it's
    /// recycled).
    ///
    /// Growth strategy (safety valve — Phase 2 follow-on):
    ///
    /// - The allocator starts with one block whose size is `initialCapacity`.
    /// - On `allocate`, it iterates all blocks looking for free-list or
    ///   tail-bump space; if every block is full and the block count is
    ///   below `maxBlocks`, it allocates a new block (same `initialCapacity`)
    ///   and serves the request from it.
    /// - At `maxBlocks` with every block full, `allocate` returns an
    ///   invalid handle so the caller can fall back (typically `nullptr`).
    ///
    /// Caveat: D3D12 only permits one shader-visible heap of each type
    /// bound at a time. Once growth happens (block > 0 in any handle),
    /// a single draw / dispatch that binds resources from two different
    /// blocks would invalidate previously-recorded descriptor tables
    /// when the second block's heap is bound. The bind path emits a
    /// DEBUG_STREAM warning the first time a block != 0 handle is bound
    /// so the operator sees the boundary. In practice growth should be
    /// rare — 65536 starting capacity covers a tab-heavy WTK workload.
    ///
    /// Thread-safe: `allocate` / `free` may be called from any thread.
    class D3D12DescriptorAllocator {
    public:
        D3D12DescriptorAllocator(ID3D12Device *device,
                                 D3D12_DESCRIPTOR_HEAP_TYPE type,
                                 std::uint32_t initialCapacity,
                                 std::uint32_t maxBlocks = 4);
        ~D3D12DescriptorAllocator();

        D3D12DescriptorAllocator(const D3D12DescriptorAllocator &) = delete;
        D3D12DescriptorAllocator &operator=(const D3D12DescriptorAllocator &) = delete;

        /// Reserve `count` slots. Phase 2 callers always request `count == 1`
        /// — `count > 1` would need to fit in one block contiguously
        /// (no cross-block runs). Returns an invalid handle on
        /// exhaustion past the growth cap.
        D3D12DescriptorHandle allocate(std::uint32_t count = 1);

        /// Return slots to the free list of their owning block. Caller
        /// is responsible for GPU-side safety (every command list that
        /// referenced the slots must have retired). No-op for invalid
        /// handles.
        void free(const D3D12DescriptorHandle &handle);

        /// The underlying shader-visible heap for a specific block. The
        /// bind path queries this with `handle.block` so multi-block
        /// allocations resolve to the correct heap.
        ID3D12DescriptorHeap *heap(std::uint32_t blockIndex = 0) const;

        std::uint32_t blockCount() const;
        std::uint32_t blockCapacity() const { return blockCapacity_; }
        std::uint32_t descriptorIncrement() const { return incrementSize_; }

    private:
        struct Block {
            ComPtr<ID3D12DescriptorHeap> heap;
            D3D12_CPU_DESCRIPTOR_HANDLE  cpuStart{};
            D3D12_GPU_DESCRIPTOR_HANDLE  gpuStart{};
            std::uint32_t                nextFreshIndex = 0;
            std::vector<std::uint32_t>   freeIndices;
        };

        // Caller must hold mutex_. Returns nullptr on failure.
        std::unique_ptr<Block> createBlock();
        // Caller must hold mutex_. Tries to allocate from `block`;
        // returns invalid handle on failure (block full).
        D3D12DescriptorHandle tryAllocateFromBlock(Block &block,
                                                    std::uint32_t blockIndex,
                                                    std::uint32_t count);
        D3D12DescriptorHandle makeHandle(const Block &block,
                                          std::uint32_t blockIndex,
                                          std::uint32_t index,
                                          std::uint32_t count) const;

        mutable std::mutex                  mutex_;
        ID3D12Device *                       device_ = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE          type_   = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        std::uint32_t                       incrementSize_ = 0;
        std::uint32_t                       blockCapacity_ = 0;
        std::uint32_t                       maxBlocks_     = 0;
        std::vector<std::unique_ptr<Block>> blocks_;
    };

_NAMESPACE_END_

#endif // OMEGAGTE_D3D12_DESCRIPTOR_ALLOCATOR_H
