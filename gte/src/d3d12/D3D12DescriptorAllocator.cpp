#include "D3D12DescriptorAllocator.h"

#include <cassert>
#include <cstdlib>

_NAMESPACE_BEGIN_

D3D12DescriptorAllocator::D3D12DescriptorAllocator(ID3D12Device *device,
                                                   D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                   std::uint32_t initialCapacity,
                                                   std::uint32_t maxBlocks)
    : device_(device), type_(type), blockCapacity_(initialCapacity), maxBlocks_(maxBlocks) {
    assert(device != nullptr && "D3D12DescriptorAllocator requires a non-null device");
    assert(initialCapacity > 0 && "D3D12DescriptorAllocator requires non-zero capacity");
    assert(maxBlocks > 0 && "D3D12DescriptorAllocator requires maxBlocks >= 1");

    incrementSize_ = device->GetDescriptorHandleIncrementSize(type);

    std::lock_guard<std::mutex> lk(mutex_);
    auto block0 = createBlock();
    if (!block0) {
        // The first block is engine-startup foundational. If the device
        // cannot hand it out, downstream resource creation would just
        // fail in the same way, so surface the failure here loud.
        DEBUG_STREAM("D3D12DescriptorAllocator: initial CreateDescriptorHeap failed for type="
                     << (int)type << " capacity=" << initialCapacity);
        std::exit(1);
    }
    blocks_.push_back(std::move(block0));
}

D3D12DescriptorAllocator::~D3D12DescriptorAllocator() = default;

std::unique_ptr<D3D12DescriptorAllocator::Block>
D3D12DescriptorAllocator::createBlock() {
    auto block = std::make_unique<Block>();
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type_;
    desc.NumDescriptors = blockCapacity_;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NodeMask = device_->GetNodeCount();
    HRESULT hr = device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&block->heap));
    if (FAILED(hr) || block->heap == nullptr) {
        DEBUG_STREAM("D3D12DescriptorAllocator: CreateDescriptorHeap failed (block "
                     << blocks_.size() << ") hr=0x" << std::hex << hr);
        return nullptr;
    }
    block->cpuStart = block->heap->GetCPUDescriptorHandleForHeapStart();
    block->gpuStart = block->heap->GetGPUDescriptorHandleForHeapStart();
    block->freeIndices.reserve(64);
    return block;
}

D3D12DescriptorHandle D3D12DescriptorAllocator::makeHandle(const Block &block,
                                                            std::uint32_t blockIndex,
                                                            std::uint32_t index,
                                                            std::uint32_t count) const {
    D3D12DescriptorHandle h;
    h.index = index;
    h.count = count;
    h.block = blockIndex;
    h.cpu.ptr = block.cpuStart.ptr + SIZE_T(index) * incrementSize_;
    h.gpu.ptr = block.gpuStart.ptr + UINT64(index) * incrementSize_;
    return h;
}

D3D12DescriptorHandle
D3D12DescriptorAllocator::tryAllocateFromBlock(Block &block,
                                                std::uint32_t blockIndex,
                                                std::uint32_t count) {
    if (count == 1) {
        if (!block.freeIndices.empty()) {
            std::uint32_t idx = block.freeIndices.back();
            block.freeIndices.pop_back();
            return makeHandle(block, blockIndex, idx, 1);
        }
    }
    // count > 1 (or empty free list) → bump from the fresh tail. count > 1
    // must fit contiguously within one block.
    if (UINT64(block.nextFreshIndex) + count <= blockCapacity_) {
        const std::uint32_t base = block.nextFreshIndex;
        block.nextFreshIndex += count;
        return makeHandle(block, blockIndex, base, count);
    }
    return {};
}

D3D12DescriptorHandle D3D12DescriptorAllocator::allocate(std::uint32_t count) {
    if (count == 0) return {};
    if (count > blockCapacity_) {
        // A single allocation larger than one block can never be served
        // contiguously; growth doesn't help.
        DEBUG_STREAM("D3D12DescriptorAllocator::allocate: requested count="
                     << count << " exceeds blockCapacity=" << blockCapacity_);
        return {};
    }

    std::lock_guard<std::mutex> lk(mutex_);

    // Walk existing blocks first — keeps allocations clustered in
    // low-index blocks, which matters for the cross-block bind warning.
    for (std::uint32_t i = 0; i < blocks_.size(); ++i) {
        auto h = tryAllocateFromBlock(*blocks_[i], i, count);
        if (h.valid()) return h;
    }

    // All existing blocks are full. Try growing.
    if (blocks_.size() >= maxBlocks_) {
        DEBUG_STREAM("D3D12DescriptorAllocator::allocate: every block (count="
                     << blocks_.size() << ", cap=" << maxBlocks_
                     << ") is full and growth cap reached");
        return {};
    }
    auto newBlock = createBlock();
    if (!newBlock) {
        DEBUG_STREAM("D3D12DescriptorAllocator::allocate: growth CreateDescriptorHeap failed");
        return {};
    }
    DEBUG_STREAM("D3D12DescriptorAllocator: growing to block " << blocks_.size()
                 << " (type=" << (int)type_ << " capacity=" << blockCapacity_
                 << "). A single draw that binds resources from multiple "
                    "blocks will invalidate descriptor tables — should be "
                    "rare given the starting capacity.");
    const std::uint32_t newBlockIndex = static_cast<std::uint32_t>(blocks_.size());
    blocks_.push_back(std::move(newBlock));
    return tryAllocateFromBlock(*blocks_[newBlockIndex], newBlockIndex, count);
}

void D3D12DescriptorAllocator::free(const D3D12DescriptorHandle &handle) {
    if (!handle.valid() || handle.count == 0) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (handle.block >= blocks_.size()) {
        DEBUG_STREAM("D3D12DescriptorAllocator::free: handle.block=" << handle.block
                     << " out of range (blocks=" << blocks_.size() << ")");
        return;
    }
    auto &block = *blocks_[handle.block];
    for (std::uint32_t i = 0; i < handle.count; ++i) {
        const std::uint32_t idx = handle.index + i;
        if (idx >= blockCapacity_) continue;
        block.freeIndices.push_back(idx);
    }
}

ID3D12DescriptorHeap *D3D12DescriptorAllocator::heap(std::uint32_t blockIndex) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (blockIndex >= blocks_.size()) return nullptr;
    return blocks_[blockIndex]->heap.Get();
}

std::uint32_t D3D12DescriptorAllocator::blockCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return static_cast<std::uint32_t>(blocks_.size());
}

_NAMESPACE_END_
