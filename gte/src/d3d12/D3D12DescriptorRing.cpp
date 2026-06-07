#include "D3D12DescriptorRing.h"

_NAMESPACE_BEGIN_

D3D12DescriptorRing::D3D12DescriptorRing(ID3D12Device *device,
                                         D3D12_DESCRIPTOR_HEAP_TYPE type,
                                         std::uint32_t capacity)
    : capacity_(capacity) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NodeMask = device->GetNodeCount();

    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_));
    if (FAILED(hr) || heap_ == nullptr) {
        // Per-queue transient ring is engine-startup foundational; if the
        // device cannot hand out 4096 transient descriptors, downstream
        // dispatches would just keep failing anyway. Fail loud.
        DEBUG_STREAM("D3D12DescriptorRing: CreateDescriptorHeap failed for type="
                     << (int)type << " capacity=" << capacity);
        std::exit(1);
    }
    cpuStart_ = heap_->GetCPUDescriptorHandleForHeapStart();
    gpuStart_ = heap_->GetGPUDescriptorHandleForHeapStart();
    incrementSize_ = device->GetDescriptorHandleIncrementSize(type);
}

D3D12DescriptorRing::~D3D12DescriptorRing() = default;

D3D12DescriptorHandle D3D12DescriptorRing::makeHandle(std::uint32_t physicalIndex,
                                                       std::uint32_t count) const {
    D3D12DescriptorHandle h;
    h.index = physicalIndex;
    h.count = count;
    // Phase 3 ring is single-block; block defaults to 0 (matches the
    // D3D12DescriptorAllocator's first block, which is what the bind
    // path keys on).
    h.block = 0;
    h.cpu.ptr = cpuStart_.ptr + SIZE_T(physicalIndex) * incrementSize_;
    h.gpu.ptr = gpuStart_.ptr + UINT64(physicalIndex) * incrementSize_;
    return h;
}

D3D12DescriptorHandle D3D12DescriptorRing::allocate(std::uint32_t count,
                                                     Retention::FenceGate gate) {
    if (count == 0 || count > capacity_) {
        return {};
    }
    std::lock_guard<std::mutex> lk(mutex_);

    const std::uint64_t inFlight = headLogical_ - tailLogical_;
    if (inFlight + count > capacity_) {
        // Ring full — nothing has retired and there isn't room for the
        // request. Caller falls back.
        return {};
    }

    const std::uint32_t physHead = static_cast<std::uint32_t>(headLogical_ % capacity_);
    std::uint32_t skipped = 0;
    if (physHead + count > capacity_) {
        // The contiguous block would straddle the wrap. Skip the
        // remaining slots in the current physical wrap to land at
        // index 0, but only if there's room for skipped + count.
        skipped = capacity_ - physHead;
        if (inFlight + skipped + count > capacity_) {
            return {};
        }
    }

    headLogical_ += skipped;
    const std::uint32_t physicalIndex = static_cast<std::uint32_t>(headLogical_ % capacity_);
    headLogical_ += count;

    pendingStamps_.push_back(PendingStamp{skipped + count, std::move(gate)});
    return makeHandle(physicalIndex, count);
}

void D3D12DescriptorRing::retire() {
    std::lock_guard<std::mutex> lk(mutex_);
    while (!pendingStamps_.empty()) {
        auto &front = pendingStamps_.front();
        if (!front.gate || !front.gate()) {
            // First-in, first-out — if the front isn't done yet, nothing
            // behind it can be either (stamps are monotonic in submit time).
            break;
        }
        tailLogical_ += front.count;
        pendingStamps_.pop_front();
    }
}

_NAMESPACE_END_
