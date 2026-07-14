#include "GED3D12CommandQueue.h"
#include "omegaGTE/GTEDevice.h"

#include "../common/GEResourceTracker.h"
#include "GED3D12Pipeline.h"
#include "GED3D12RenderTarget.h"
#include "GED3D12Texture.h"

#include <memory>
#include <cstring>

#include <d3d12.h>
_NAMESPACE_BEGIN_

#ifndef D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
#    define D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE                                                                   \
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
#endif

// Caller-contract guard for the command-buffer encoding methods — the D3D12
// twin of Metal's `metalRequireOrReturn` (see GEMetalCommandQueue.mm). On a
// contract violation it reports `DEBUG_CRITICAL` (which bypasses the master
// debug gate, so it surfaces even in a release build with the layer off; see
// Debug-Layer-Plan.md §4.1.6), trips the debug-build `assert`, then *returns
// from the calling encoder method*. The early return is what closes the
// release-build crash: with asserts compiled out, the old bare-`assert` sites
// fell through and dereferenced a null pipeline-state / encoder one line
// later. A contract violation that should be a clean no-op was instead a
// crash. `OrReturn` keeps that control flow legible at the call site.
//
// `ok` must be a side-effect-free predicate — it is evaluated twice (once for
// the branch, once inside the assert so the failed condition prints in debug).
// A macro, not a function, because only a macro can `return` from the caller;
// `do/while(0)` lets it be used as an ordinary `stmt;`. The
// `…Value` twin returns `ret` for the non-void encoding methods.
#define d3d12RequireOrReturn(ok, domain, what)                                 \
    do {                                                                       \
        if(!(ok)){                                                             \
            DEBUG_CRITICAL((domain), (what));                                  \
            assert((ok) && "GTE caller-contract violation; see the CRITICAL log line above"); \
            return;                                                            \
        }                                                                      \
    } while(0)

#define d3d12RequireOrReturnValue(ok, domain, what, ret)                       \
    do {                                                                       \
        if(!(ok)){                                                             \
            DEBUG_CRITICAL((domain), (what));                                  \
            assert((ok) && "GTE caller-contract violation; see the CRITICAL log line above"); \
            return (ret);                                                      \
        }                                                                      \
    } while(0)

// GED3D12CommandBuffer::GED3D12CommandBuffer(){};
// void GED3D12CommandBuffer::commitToBuffer(){};

/// Map `GECommandQueueDesc::Type` → `D3D12_COMMAND_LIST_TYPE`.
/// Universal and Graphics both alias to DIRECT — D3D12 has no notion of
/// "must also expose transfer," every DIRECT queue already does.
static D3D12_COMMAND_LIST_TYPE d3d12_listTypeFor(GECommandQueueDesc::Type t) {
    switch (t) {
        case GECommandQueueDesc::Type::Compute:  return D3D12_COMMAND_LIST_TYPE_COMPUTE;
        case GECommandQueueDesc::Type::Transfer: return D3D12_COMMAND_LIST_TYPE_COPY;
        case GECommandQueueDesc::Type::Graphics:
        case GECommandQueueDesc::Type::Universal:
        default:                                 return D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
}

/// Map `GECommandQueueDesc::Priority` → `D3D12_COMMAND_QUEUE_PRIORITY`.
/// D3D12 only exposes NORMAL / HIGH / GLOBAL_REALTIME — Low collapses into
/// NORMAL on this backend.
static D3D12_COMMAND_QUEUE_PRIORITY d3d12_priorityFor(GECommandQueueDesc::Priority p) {
    switch (p) {
        case GECommandQueueDesc::Priority::High:     return D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        case GECommandQueueDesc::Priority::Realtime: return D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
        case GECommandQueueDesc::Priority::Low:
        case GECommandQueueDesc::Priority::Normal:
        default:                                     return D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    }
}

GED3D12CommandQueue::GED3D12CommandQueue(GED3D12Engine *engine, const GECommandQueueDesc & desc)
    : GECommandQueue(desc, /*achievedType=*/desc.type),
      engine(engine), currentCount(0), initialBufferHint(desc.maxBufferCount) {
    HRESULT hr;

    hr = engine->d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

    if (FAILED(hr)) {
        exit(1);
    };

    hr = engine->d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retentionFence));
    if (FAILED(hr)) {
        exit(1);
    };

    cpuEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    // commitToGPUAndWait registers the completion event per-call against a
    // fresh monotonic value; no pre-registration against a fixed value here.

    D3D12_COMMAND_QUEUE_DESC d3dDesc;
    d3dDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    d3dDesc.NodeMask = engine->d3d12_device->GetNodeCount();
    d3dDesc.Priority = d3d12_priorityFor(desc.priority);
    d3dDesc.Type = d3d12_listTypeFor(desc.type);
    hr = engine->d3d12_device->CreateCommandQueue(&d3dDesc, IID_PPV_ARGS(&commandQueue));
    if (FAILED(hr) && d3dDesc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) {
        // Realtime requires the GPU-priority entitlement (Win10 Game Mode /
        // priority-class profile). The plan defines this as a silent
        // downgrade to HIGH so test rigs and dev machines without the
        // entitlement still get a functional queue. The user's requested
        // priority remains visible via priority() — it's the achieved type
        // that controls dedication, not priority.
        DEBUG_INFO(DEBUG_DOMAIN_QUEUE, "CreateCommandQueue: GLOBAL_REALTIME denied (no entitlement); retrying at HIGH");
        d3dDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        hr = engine->d3d12_device->CreateCommandQueue(&d3dDesc, IID_PPV_ARGS(&commandQueue));
    }
    if (FAILED(hr)) {
        MessageBoxA(GetForegroundWindow(), "Failed to Create Command Queue.", "NOTE", MB_OK);
        exit(1);
    };
    if (!desc.label.empty()) {
        ATL::CStringW wstr(desc.label.c_str());
        commandQueue->SetName(wstr);
    }

    // CommandQueue-Typed-Pool Phase 3 — pre-allocate the (allocator, list)
    // pool to `desc.maxBufferCount` slots. Each slot keeps its own
    // ID3D12CommandAllocator + ID3D12GraphicsCommandList6 owning ComPtrs;
    // `poolSubmissionIndex` tracks the retentionFence value the slot was
    // last submitted at so getAvailableBuffer can recycle it once the
    // GPU has caught up. Failing to create a slot at construction time
    // is treated as fatal — same posture as the queue-itself create
    // above — because the queue is then unusable.
    listType = d3d12_listTypeFor(desc.type);
    poolAllocators.reserve(desc.maxBufferCount);
    poolLists.reserve(desc.maxBufferCount);
    poolSubmissionIndex.reserve(desc.maxBufferCount);
    for (unsigned i = 0; i < desc.maxBufferCount; ++i) {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList6> list;
        if (FAILED(engine->d3d12_device->CreateCommandAllocator(listType, IID_PPV_ARGS(&alloc)))) {
            DEBUG_ERROR(DEBUG_DOMAIN_QUEUE, "GED3D12CommandQueue: CreateCommandAllocator (pool) failed; queue construction aborted");
            std::exit(1);
        }
        if (FAILED(engine->d3d12_device->CreateCommandList(engine->d3d12_device->GetNodeCount(),
                                                            listType, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) {
            DEBUG_ERROR(DEBUG_DOMAIN_QUEUE, "GED3D12CommandQueue: CreateCommandList (pool) failed; queue construction aborted");
            std::exit(1);
        }
        // CreateCommandList returns the list in recording state; close it now
        // so the first getAvailableBuffer() recycle path can Reset its
        // allocator without hitting "list currently being recorded".
        list->Close();
        poolAllocators.push_back(std::move(alloc));
        poolLists.push_back(std::move(list));
        poolSubmissionIndex.push_back(0);
    }

    // GPU Commit-Timing P1 — per-buffer GPU timestamp infrastructure. Only
    // DIRECT / COMPUTE queues can write graphics/compute TIMESTAMP queries; a
    // COPY queue needs the separate copy-queue timestamp path (out of scope),
    // so timing stays disabled there. Disable silently — keeping the documented
    // zero-timing fallback — if the device won't report a frequency or any
    // resource create fails. Sized to `kPoolCeiling` (2 queries per slot) once
    // so the heap never needs recreating as the pool grows.
    if (listType == D3D12_COMMAND_LIST_TYPE_DIRECT
        || listType == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        UINT64 freq = 0;
        if (SUCCEEDED(commandQueue->GetTimestampFrequency(&freq)) && freq > 0) {
            D3D12_QUERY_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            heapDesc.Count = kPoolCeiling * 2;
            heapDesc.NodeMask = engine->d3d12_device->GetNodeCount();
            ComPtr<ID3D12QueryHeap> heap;
            if (SUCCEEDED(engine->d3d12_device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap)))) {
                const UINT64 readbackBytes =
                    static_cast<UINT64>(kPoolCeiling) * 2 * sizeof(std::uint64_t);
                D3D12_HEAP_PROPERTIES rbProps{};
                rbProps.Type = D3D12_HEAP_TYPE_READBACK;
                rbProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                rbProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                rbProps.CreationNodeMask = engine->d3d12_device->GetNodeCount();
                rbProps.VisibleNodeMask = engine->d3d12_device->GetNodeCount();
                D3D12_RESOURCE_DESC rbDesc{};
                rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rbDesc.Alignment = 0;
                rbDesc.Width = readbackBytes;
                rbDesc.Height = 1;
                rbDesc.DepthOrArraySize = 1;
                rbDesc.MipLevels = 1;
                rbDesc.Format = DXGI_FORMAT_UNKNOWN;
                rbDesc.SampleDesc.Count = 1;
                rbDesc.SampleDesc.Quality = 0;
                rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                rbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
                ComPtr<ID3D12Resource> readback;
                if (SUCCEEDED(engine->d3d12_device->CreateCommittedResource(
                        &rbProps, D3D12_HEAP_FLAG_NONE, &rbDesc,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
                    void *mapped = nullptr;
                    // Map for the queue's lifetime; the read range spans the whole
                    // buffer — pollCompletions only reads a slot after its
                    // retention-fence value retires, so the GPU's resolve write is
                    // already visible.
                    D3D12_RANGE readRange{0, static_cast<SIZE_T>(readbackBytes)};
                    if (SUCCEEDED(readback->Map(0, &readRange, &mapped))) {
                        timestampHeap      = std::move(heap);
                        timestampReadback  = std::move(readback);
                        timestampMapped    = static_cast<std::uint64_t *>(mapped);
                        timestampFrequency = freq;
                        timestampsEnabled  = true;
                    }
                }
            }
        }
    }

    // Shared-Descriptor-Heap-Plan Phase 3 — per-queue transient descriptor
    // ring for one-shot dispatches (tessellation + mipmap generation).
    // Pre-Phase-3 these paths each called CreateDescriptorHeap per call;
    // the ring amortizes that into a single heap reused across submissions.
    transientRing = std::make_unique<D3D12DescriptorRing>(
        engine->d3d12_device.Get(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        /*capacity=*/4096u);

    traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
    ResourceTracking::Tracker::instance().emit(ResourceTracking::EventType::Create, ResourceTracking::Backend::D3D12,
                                               "CommandQueue", traceResourceId, commandQueue.Get());
    DEBUG_INFO(DEBUG_DOMAIN_QUEUE, "Queue created: queue=" << traceResourceId << " type=" << (int)desc.type);
};

std::uint32_t GED3D12CommandQueue::growPoolOnce() {
    if (poolAllocators.size() >= kPoolCeiling) {
        DEBUG_ERROR(DEBUG_DOMAIN_QUEUE, "GED3D12CommandQueue: pool at ceiling (" << kPoolCeiling
                  << ") and every slot is still in flight — refusing to grow; "
                     "check for missed commitToGPU / over-submission.");
        return UINT32_MAX;
    }
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList6> list;
    if (FAILED(engine->d3d12_device->CreateCommandAllocator(listType, IID_PPV_ARGS(&alloc)))) {
        DEBUG_ERROR(DEBUG_DOMAIN_QUEUE, "GED3D12CommandQueue: CreateCommandAllocator (grow) failed");
        return UINT32_MAX;
    }
    if (FAILED(engine->d3d12_device->CreateCommandList(engine->d3d12_device->GetNodeCount(),
                                                        listType, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        DEBUG_ERROR(DEBUG_DOMAIN_QUEUE, "GED3D12CommandQueue: CreateCommandList (grow) failed");
        return UINT32_MAX;
    }
    // Same close-after-create as the ctor: keep new slots in the closed
    // steady-state the recycle path assumes.
    list->Close();
    const std::uint32_t slot = static_cast<std::uint32_t>(poolAllocators.size());
    poolAllocators.push_back(std::move(alloc));
    poolLists.push_back(std::move(list));
    poolSubmissionIndex.push_back(0);
    if (!poolGrowthWarned && initialBufferHint > 0
        && poolAllocators.size() > 4ull * initialBufferHint) {
        DEBUG_INFO(DEBUG_DOMAIN_QUEUE, "GED3D12CommandQueue: pool grew to " << poolAllocators.size()
                  << " (initial hint=" << initialBufferHint
                  << "); consider raising desc.maxBufferCount on this queue.");
        poolGrowthWarned = true;
    }
    return slot;
}

void GED3D12CommandQueue::stampPendingSlots(std::uint64_t signalValue) {
    for (auto slot : pendingSlots) {
        if (slot != UINT32_MAX && slot < poolSubmissionIndex.size()) {
            poolSubmissionIndex[slot] = signalValue;
        }
    }
    pendingSlots.clear();
}

void GED3D12CommandQueue::writeStartTimestamp(ID3D12GraphicsCommandList6 *list,
                                              std::uint32_t slot) {
    // GPU Commit-Timing P1 — start of the buffer's GPU span. Written as the
    // first command after a list reset so it brackets the whole execution.
    if (!timestampsEnabled || list == nullptr
        || slot == UINT32_MAX || slot >= kPoolCeiling) {
        return;
    }
    list->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
}

void GED3D12CommandQueue::writeEndTimestampAndResolve(ID3D12GraphicsCommandList6 *list,
                                                      std::uint32_t slot) {
    // GPU Commit-Timing P1 — end of the buffer's GPU span plus a resolve of the
    // [start,end] pair into this slot's 16-byte region of the readback buffer.
    // Recorded just before Close(), so the resolve runs after both timestamps.
    if (!timestampsEnabled || list == nullptr
        || slot == UINT32_MAX || slot >= kPoolCeiling) {
        return;
    }
    list->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    list->ResolveQueryData(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                           slot * 2, 2, timestampReadback.Get(),
                           static_cast<UINT64>(slot) * 2 * sizeof(std::uint64_t));
}

bool GED3D12CommandQueue::resolveSlotTiming(std::uint32_t slot,
                                            double &startSec, double &endSec) const {
    if (!timestampsEnabled || timestampMapped == nullptr || timestampFrequency == 0
        || slot == UINT32_MAX || slot >= kPoolCeiling) {
        return false;
    }
    const std::uint64_t startTicks = timestampMapped[slot * 2];
    const std::uint64_t endTicks   = timestampMapped[slot * 2 + 1];
    // A slot whose list recorded no timestamps (a defensive ad-hoc buffer) or a
    // pair that hasn't been written this cycle leaves stale / zero ticks. Reject
    // any non-increasing pair so it can't poison the aggregator's min/max fold.
    if (endTicks <= startTicks) {
        return false;
    }
    const double inv = 1.0 / static_cast<double>(timestampFrequency);
    startSec = static_cast<double>(startTicks) * inv;
    endSec   = static_cast<double>(endTicks) * inv;
    return true;
}

Retention::FenceGate GED3D12CommandQueue::gateForNextSubmit() {
    const std::uint64_t v = nextSubmitValue + 1;
    ComPtr<ID3D12Fence> f = retentionFence;
    return [f, v]() { return f->GetCompletedValue() >= v; };
}

Retention::FenceGate GED3D12CommandQueue::gateForRetiredSubmissions() const {
    // Snapshot the value that the most-recent submit told the GPU to signal.
    // Once GetCompletedValue() catches up, every command list we've already
    // submitted has retired. The captured ComPtr keeps the fence alive even
    // if this queue is destroyed before the gate fires.
    const std::uint64_t v = nextSubmitValue;
    ComPtr<ID3D12Fence> f = retentionFence;
    return [f, v]() {
        if (!f) return true;   // queue went away → nothing left to wait on
        if (v == 0) return true;  // nothing submitted yet on this queue
        return f->GetCompletedValue() >= v;
    };
}

void GED3D12CommandQueue::flushPendingRetentionUnder(const Retention::FenceGate &gate) {
    for (auto &buf : retainedCommandBuffers) {
        engine->retentionQueue.retainShared(std::move(buf), {gate});
    }
    retainedCommandBuffers.clear();
    for (auto &heap : retainedDescriptorHeaps) {
        engine->retentionQueue.enqueue({gate},
                                       [h = std::move(heap)]() mutable { h.Reset(); });
    }
    retainedDescriptorHeaps.clear();
}

GED3D12CommandBuffer::GED3D12CommandBuffer(ID3D12GraphicsCommandList6 *commandList,
                                           ID3D12CommandAllocator *commandAllocator, GED3D12CommandQueue *parentQueue)
    : commandList(commandList), commandAllocator(commandAllocator), parentQueue(parentQueue), inComputePass(false),
      inBlitPass(false), traceResourceId(ResourceTracking::Tracker::instance().nextResourceId()) {

    ResourceTracking::Tracker::instance().emit(ResourceTracking::EventType::Create, ResourceTracking::Backend::D3D12,
                                               "CommandBuffer", traceResourceId, this->commandList.Get());
};

unsigned int GED3D12CommandBuffer::getRootParameterIndexOfResource(unsigned int id, omegasl_shader &shader) {
    bool isSRV = false, isUAV = false, isCBV = false, isRootConstants = false, isDescriptorTable = false, isSampler = false;
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout, shader.pLayout + shader.nLayout};

    unsigned relative_index = 0;
    for (auto &l : layoutArr) {
        if (l.location == id) {
            relative_index = l.gpu_relative_loc;
            if (l.type == OMEGASL_SHADER_BUFFER_DESC) {
                if (l.io_mode == OMEGASL_SHADER_DESC_IO_IN) {
                    isSRV = true;
                } else {
                    isUAV = true;
                }
            } else if (l.type == OMEGASL_SHADER_UNIFORM_DESC) {
                // §2.4 constant buffer — bound as a root CBV.
                isCBV = true;
            } else if (l.type == OMEGASL_SHADER_PUSH_CONSTANT_DESC) {
                // §2.2 push constant — bound as root 32-bit constants at the
                // same `b` register class as a CBV (independent of CBVs by
                // ParameterType, so a push constant at b0 and a uniform at b0
                // in different stages don't alias).
                isRootConstants = true;
            } else if (l.type == OMEGASL_SHADER_SAMPLER1D_DESC || l.type == OMEGASL_SHADER_SAMPLER2D_DESC
                       || l.type == OMEGASL_SHADER_SAMPLER3D_DESC || l.type == OMEGASL_SHADER_SAMPLERCUBE_DESC) {
                // Extension 8 — runtime sampler. A sampler is a descriptor
                // table, but its range type is SAMPLER, so it must not match a
                // texture's SRV/UAV table that happens to share the same
                // register number (HLSL `t#` and `s#` are independent register
                // classes — `texture2d t0` + `sampler s0` both have
                // gpu_relative_loc 0).
                isDescriptorTable = true;
                isSampler = true;
            } else {
                isDescriptorTable = true;
            }
            break;
        }
    }

    // Per-stage register space. MUST match both the HLSL codegen
    // (`HLSLTarget::emitResourceBinding`'s `registerSpace` lambda) and the root
    // signature builder (`createRootSignatureFromOmegaSLShaders`) — all three
    // read the same `omegasl_shader::type`, and a disagreement between any two
    // of them resolves a resource to the wrong root parameter (or to none).
    //
    //   0 — vertex / mesh / compute / hull / domain
    //   1 — fragment
    //   2 — amplification (§5): the first stage that COEXISTS with another
    //       space-0 stage (mesh) in one pipeline, so it needs its own space or a
    //       `constant<T>` declared `[in pc]` on both would collide at b0/space0.
    unsigned regSpace;
    if (shader.type == OMEGASL_SHADER_FRAGMENT) {
        regSpace = 1;
    } else if (shader.type == OMEGASL_SHADER_AMPLIFICATION) {
        regSpace = 2;
    } else {
        regSpace = 0;
    }

    unsigned idx = 0;
    for (; idx < currentRootSignature->NumParameters; idx++) {
        auto &param = currentRootSignature->pParameters[idx];
        // std::cout << "PARAM_TYPE:" << (int)param.ParameterType << std::endl;
        if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV && isSRV) {
            if (param.Descriptor.ShaderRegister == relative_index && param.Descriptor.RegisterSpace == regSpace) {
                break;
            }
        } else if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV && isUAV) {
            if (param.Descriptor.ShaderRegister == relative_index && param.Descriptor.RegisterSpace == regSpace) {
                break;
            }
        } else if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV && isCBV) {
            if (param.Descriptor.ShaderRegister == relative_index && param.Descriptor.RegisterSpace == regSpace) {
                break;
            }
        } else if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS && isRootConstants) {
            // §2.2 push constant — match the root-constants param by its `b`
            // register + space (Constants.ShaderRegister, not Descriptor).
            if (param.Constants.ShaderRegister == relative_index && param.Constants.RegisterSpace == regSpace) {
                break;
            }
        } else if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && isDescriptorTable) {
            // Descriptor-table match must include the range *type*.
            // The mipmap-gen pipeline (srcMip = SRV at t0, dstMip = UAV
            // at u0) lays out two descriptor tables whose first range
            // shares BaseShaderRegister=0 — they differ only by range
            // type (SRV vs UAV). Without the type check, the UAV
            // lookup matched the SRV table at parameter 0 and the
            // dispatch loop double-bound the same root parameter with
            // mismatched-type handles — D3D12 debug-layer error
            // INVALID_DESCRIPTOR_HANDLE.
            auto &range = param.DescriptorTable.pDescriptorRanges[0];
            const bool typeMatches =
                (isSRV && range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) ||
                (isUAV && range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) ||
                // Extension 8 — a runtime sampler must match only a SAMPLER
                // range, never a texture's SRV/UAV table that shares the
                // register number (independent HLSL register classes).
                (isSampler && range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) ||
                // CBV / other descriptor-table lookups don't set
                // isSRV/isUAV/isSampler; fall through to a register-only match.
                (!isSRV && !isUAV && !isSampler);
            if (typeMatches &&
                range.BaseShaderRegister == relative_index &&
                range.RegisterSpace == regSpace) {
                break;
            }
        }
    }
    return idx;
}

GED3D12CommandBuffer::BufferRootKind
GED3D12CommandBuffer::classifyBufferRootKind(unsigned id, omegasl_shader &shader) {
    // D3D12-CPU-Accessible-Buffer-Plan Phase 1 — mirror the SRV/UAV/CBV decision
    // getRootParameterIndexOfResource already makes from the shader layout, so a
    // buffer's binding is driven by what the shader declares, not by which heap
    // the resource happens to live on.
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout, shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.location == id) {
            if (l.type == OMEGASL_SHADER_UNIFORM_DESC) {
                return BufferRootKind::CBV;
            }
            if (l.type == OMEGASL_SHADER_BUFFER_DESC) {
                return (l.io_mode == OMEGASL_SHADER_DESC_IO_IN) ? BufferRootKind::SRV
                                                                : BufferRootKind::UAV;
            }
            break;
        }
    }
    // Unexpected / missing layout entry — default to a read-only SRV bind, the
    // safest fallback (never promotes a buffer to writable UAV access by accident).
    return BufferRootKind::SRV;
}

void GED3D12CommandBuffer::transitionBufferState(GED3D12Buffer *buf, D3D12_RESOURCE_STATES target) {
    if (buf == nullptr || buf->buffer == nullptr) {
        return;
    }
    // UPLOAD-heap buffers are permanently GENERIC_READ — they can't be
    // transitioned and already satisfy shader-read. Only DEFAULT-heap buffers
    // (the Readback outputs this plan introduces) transition.
    D3D12_HEAP_PROPERTIES heapProps;
    D3D12_HEAP_FLAGS heapFlags;
    buf->buffer->GetHeapProperties(&heapProps, &heapFlags);
    if (heapProps.Type != D3D12_HEAP_TYPE_DEFAULT || buf->currentState == target) {
        return;
    }
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(buf->buffer.Get(),
                                                        buf->currentState, target);
    commandList->ResourceBarrier(1, &barrier);
    buf->currentState = target;
}

void GED3D12CommandBuffer::flushPendingUpload(GED3D12Buffer *buf) {
    if (buf == nullptr || buf->uploadCompanion == nullptr || buf->uploadDirtyBytes == 0) {
        return;
    }
    // The UPLOAD companion is permanently GENERIC_READ (a valid copy source);
    // the primary transitions to COPY_DEST for the copy and back to whatever
    // the caller needs afterwards (the caller's own transitionBufferState both
    // restores the bind state and orders this copy before the dispatch).
    transitionBufferState(buf, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(buf->buffer.Get(), 0,
                                  buf->uploadCompanion.Get(), 0,
                                  buf->uploadDirtyBytes);
    buf->uploadDirtyBytes = 0;
}

// Pipeline-Completion-Extension-Plan §6.3 — locate the layout-desc that
// owns the given bind location and run the kind/sample-count check
// against the bound texture. Logs a diagnostic on mismatch and returns
// false; the caller may either skip the bind or carry on (we keep the
// command list valid but flag the user).
static bool checkTextureBindAgainstShader(unsigned int location,
                                          const omegasl_shader &shader,
                                          GETexture &tex) {
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout,
                                                                shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.location == location) {
            return validateTextureBindKind((int)l.type, tex.getKind(),
                                           tex.getSampleCount(), shader.name, location);
        }
    }
    return true;
}

// Extension 8 §8.5 — sampler-bind validation. Rejects static-sampler and
// non-sampler slots via validateSamplerBindKind().
static bool checkSamplerBindAgainstShader(unsigned int location,
                                          const omegasl_shader &shader) {
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout,
                                                                shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.location == location) {
            return validateSamplerBindKind((int)l.type, shader.name, location);
        }
    }
    return true;
}

void GED3D12CommandBuffer::rebindDescriptorHeaps() {
    // Bind BOTH engine-wide heaps together, defaulting whichever isn't bound yet
    // to its block-0 heap, and re-issue SetDescriptorHeaps ONLY when the bound
    // set actually changes. D3D12 invalidates all root descriptor-table bindings
    // whenever SetDescriptorHeaps changes the heap set: previously the texture
    // bind set [resource] and a later runtime-sampler bind set [resource,sampler],
    // so adding the sampler heap re-issued the call and silently invalidated the
    // texture's already-set table — the sampled texture then read nothing and the
    // RT kept its clear color (runtime sampler "renders nothing"; static samplers,
    // which need no heap, were unaffected). Binding both up front and skipping the
    // no-op keeps the heap set constant for the whole pass.
    auto *engine = parentQueue->engine;
    ID3D12DescriptorHeap *resourceHeap =
        currentResourceDescHeap ? currentResourceDescHeap
                                : engine->resourceDescriptorAllocator->heap(0);
    ID3D12DescriptorHeap *samplerHeap =
        currentSamplerDescHeap ? currentSamplerDescHeap
                               : engine->samplerDescriptorAllocator->heap(0);
    if (resourceHeap == boundResourceDescHeap && samplerHeap == boundSamplerDescHeap) {
        return;  // already bound this set — re-issuing would invalidate live tables
    }
    ID3D12DescriptorHeap *heaps[2];
    unsigned n = 0;
    if (resourceHeap) heaps[n++] = resourceHeap;
    if (samplerHeap)  heaps[n++] = samplerHeap;
    if (n) commandList->SetDescriptorHeaps(n, heaps);
    boundResourceDescHeap = resourceHeap;
    boundSamplerDescHeap  = samplerHeap;
}

D3D12_RESOURCE_STATES
GED3D12CommandBuffer::getRequiredResourceStateForResourceID(unsigned int &id, omegasl_shader &shader) {
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout, shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.location == id) {
            D3D12_RESOURCE_STATES state;
            if (l.type == OMEGASL_SHADER_TEXTURE1D_DESC || l.type == OMEGASL_SHADER_TEXTURE2D_DESC ||
                l.type == OMEGASL_SHADER_TEXTURE3D_DESC ||
                /// OmegaSL §2.1 Phase A — cube/array/MS layout types are
                /// emitted by the compiler. Phase B will pick the correct
                /// SRV view-dimension when the texture is bound. Until then
                /// the resource-state transition logic is identical to a
                /// plain texture (SRV vs UAV depending on IO direction),
                /// so they fall through to the same branch.
                l.type == OMEGASL_SHADER_TEXTURE1D_ARRAY_DESC ||
                l.type == OMEGASL_SHADER_TEXTURE2D_ARRAY_DESC ||
                l.type == OMEGASL_SHADER_TEXTURECUBE_DESC ||
                l.type == OMEGASL_SHADER_TEXTURECUBE_ARRAY_DESC ||
                l.type == OMEGASL_SHADER_TEXTURE2D_MS_DESC ||
                l.type == OMEGASL_SHADER_TEXTURE2D_MS_ARRAY_DESC) {
                if (l.io_mode == OMEGASL_SHADER_DESC_IO_IN) {
                    state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                } else {
                    state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                }
            } else if (l.type == OMEGASL_SHADER_BUFFER_DESC) {
                if (l.io_mode == OMEGASL_SHADER_DESC_IO_IN) {
                    state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                } else {
                    state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                }
            } else if (l.type == OMEGASL_SHADER_UNIFORM_DESC) {
                // §2.4 constant buffer — read-only on the GPU.
                state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            } else {
                DEBUG_ERROR(DEBUG_DOMAIN_RESOURCE, "This resource cannot be transitioned");
                exit(1);
            }
            return state;
        }
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

TextureSwizzle
GED3D12CommandBuffer::resolveEffectiveSwizzle(const TextureSwizzle & runtime,unsigned id,omegasl_shader &shader){
    if(!runtime.isIdentity()) return runtime;
    // Layout-desc encoding: 0=Identity, 1=R, 2=G, 3=B, 4=A, 5=Zero, 6=One.
    auto decode = [](unsigned char b) -> TextureSwizzleChannel {
        switch(b){
            case 1: return TextureSwizzleChannel::Red;
            case 2: return TextureSwizzleChannel::Green;
            case 3: return TextureSwizzleChannel::Blue;
            case 4: return TextureSwizzleChannel::Alpha;
            case 5: return TextureSwizzleChannel::Zero;
            case 6: return TextureSwizzleChannel::One;
            default: return TextureSwizzleChannel::Identity;
        }
    };
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout, shader.pLayout + shader.nLayout};
    for (auto & l : layoutArr) {
        if(l.location == id){
            if(l.swizzle_desc.r == 0 && l.swizzle_desc.g == 0
               && l.swizzle_desc.b == 0 && l.swizzle_desc.a == 0){
                return TextureSwizzle::identity();
            }
            return TextureSwizzle{
                decode(l.swizzle_desc.r),
                decode(l.swizzle_desc.g),
                decode(l.swizzle_desc.b),
                decode(l.swizzle_desc.a)
            };
        }
    }
    return TextureSwizzle::identity();
}

void GED3D12CommandBuffer::startBlitPass() {
    inBlitPass = true;
    DEBUG_TRACE(DEBUG_DOMAIN_QUEUE, "BlitPass begin");
};

void GED3D12CommandBuffer::copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest) {
    d3d12RequireOrReturn(inBlitPass, DEBUG_DOMAIN_RESOURCE, "copyTextureToTexture called outside a blit pass");
    auto *srcText = (GED3D12Texture *)src.get(), *destText = (GED3D12Texture *)dest.get();
    /// Resource Synchronization Checks
    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (srcText->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        if (srcText->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcText->resource.Get()));
        }

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(srcText->resource.Get(), srcText->currentState,
                                                                        D3D12_RESOURCE_STATE_COPY_SOURCE));
        srcText->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    if (destText->currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            destText->resource.Get(), destText->currentState, D3D12_RESOURCE_STATE_COPY_DEST));
        // Track the *dest's* new state (was erroneously assigning srcText's,
        // which both lost the dest's COPY_DEST and corrupted the src's state —
        // breaking any follow-on transition such as the readback download below).
        destText->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }
    commandList->CopyResource(destText->resource.Get(), srcText->resource.Get());

    // If the destination is a CPU-readback texture, getBytes reads its READBACK
    // companion — which CopyResource above does NOT touch (it writes the DEFAULT
    // primary). Populate the companion now via the texture→buffer download, or
    // the readback returns the companion's uninitialised zeros.
    if (destText->isReadbackTexture() && destText->cpuSideresource) {
        destText->downloadTextureToReadbackHeap(commandList.Get());
    }
}

void GED3D12CommandBuffer::copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest,
                                                const TextureRegion &region, const GPoint3D &destCoord) {
    d3d12RequireOrReturn(inBlitPass, DEBUG_DOMAIN_RESOURCE, "copyTextureToTexture (region) called outside a blit pass");
    auto *srcText = (GED3D12Texture *)src.get(), *destText = (GED3D12Texture *)dest.get();

    /// Resource Synchronization Checks
    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (srcText->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        if (srcText->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcText->resource.Get()));
        }

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(srcText->resource.Get(), srcText->currentState,
                                                                        D3D12_RESOURCE_STATE_COPY_SOURCE));
        srcText->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    if (destText->currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        if (destText->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(destText->resource.Get()));
        }

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            destText->resource.Get(), destText->currentState, D3D12_RESOURCE_STATE_COPY_DEST));
        destText->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }

    CD3DX12_TEXTURE_COPY_LOCATION srcLoc(srcText->resource.Get()), destLoc(destText->resource.Get());
    LONG top_pos = LONG(region.h) - LONG(region.y);
    CD3DX12_BOX _region((LONG)region.x, top_pos, LONG(region.x + region.w), LONG(top_pos + region.h));
    commandList->CopyTextureRegion(&destLoc, (UINT)destCoord.x, (UINT)destCoord.y, (UINT)destCoord.z, &srcLoc,
                                   &_region);

    // Keep a readback destination's CPU companion in sync (see the whole-texture
    // overload above).
    if (destText->isReadbackTexture() && destText->cpuSideresource) {
        destText->downloadTextureToReadbackHeap(commandList.Get());
    }
}

void GED3D12CommandBuffer::copyBufferToBuffer(SharedHandle<GEBuffer> &src, SharedHandle<GEBuffer> &dest,
                                              size_t size, size_t srcOffset, size_t destOffset) {
    d3d12RequireOrReturn(inBlitPass, DEBUG_DOMAIN_RESOURCE, "copyBufferToBuffer called outside a blit pass");
    auto *srcBuf = (GED3D12Buffer *)src.get();
    auto *destBuf = (GED3D12Buffer *)dest.get();

    // A Universal source must present its latest CPU write; a Universal
    // destination's pending CPU write is superseded by this copy (the copy
    // was requested after the write in program order).
    flushPendingUpload(srcBuf);
    destBuf->uploadDirtyBytes = 0;

    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (srcBuf->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE &&
        srcBuf->currentState != D3D12_RESOURCE_STATE_GENERIC_READ) {
        if (srcBuf->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcBuf->buffer.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            srcBuf->buffer.Get(), srcBuf->currentState, D3D12_RESOURCE_STATE_COPY_SOURCE));
        srcBuf->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    if (destBuf->currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        if (destBuf->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(destBuf->buffer.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            destBuf->buffer.Get(), destBuf->currentState, D3D12_RESOURCE_STATE_COPY_DEST));
        destBuf->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }

    UINT64 bytes = size == 0 ? srcBuf->buffer->GetDesc().Width - srcOffset : size;
    commandList->CopyBufferRegion(destBuf->buffer.Get(), destOffset, srcBuf->buffer.Get(), srcOffset, bytes);

    // Keep a Readback destination's CPU companion in sync (mirrors the texture
    // copy paths above): GEBufferReader maps the companion, so a CPU-staged
    // upload into the DEFAULT primary must land there too or a readback that
    // happens before any dispatch rewrites the region reads stale bytes.
    if (destBuf->cpuSideResource) {
        commandList->CopyBufferRegion(destBuf->cpuSideResource.Get(), destOffset,
                                      srcBuf->buffer.Get(), srcOffset, bytes);
    }
}

void GED3D12CommandBuffer::copyBufferToTexture(SharedHandle<GEBuffer> &src, SharedHandle<GETexture> &dest,
                                               size_t bytesPerRow, size_t bytesPerImage,
                                               const TextureRegion &destRegion, size_t srcBufferOffset) {
    (void)bytesPerImage;
    d3d12RequireOrReturn(inBlitPass, DEBUG_DOMAIN_RESOURCE, "copyBufferToTexture called outside a blit pass");
    auto *srcBuf = (GED3D12Buffer *)src.get();
    auto *destTex = (GED3D12Texture *)dest.get();

    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (srcBuf->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE &&
        srcBuf->currentState != D3D12_RESOURCE_STATE_GENERIC_READ) {
        if (srcBuf->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcBuf->buffer.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            srcBuf->buffer.Get(), srcBuf->currentState, D3D12_RESOURCE_STATE_COPY_SOURCE));
        srcBuf->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    if (destTex->currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        if (destTex->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(destTex->resource.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            destTex->resource.Get(), destTex->currentState, D3D12_RESOURCE_STATE_COPY_DEST));
        destTex->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    footprint.Offset = srcBufferOffset;
    footprint.Footprint.Format = destTex->resource->GetDesc().Format;
    footprint.Footprint.Width = destRegion.w;
    footprint.Footprint.Height = destRegion.h;
    footprint.Footprint.Depth = destRegion.d == 0 ? 1 : destRegion.d;
    footprint.Footprint.RowPitch = static_cast<UINT>(bytesPerRow);

    // §7.1: address the (mipLevel, arrayLayer) subresource rather than 0.
    const auto destDesc = destTex->resource->GetDesc();
    const UINT destSubresource = D3D12CalcSubresource(destRegion.mipLevel, destRegion.arrayLayer,
                                                      0, destDesc.MipLevels, destDesc.DepthOrArraySize);

    CD3DX12_TEXTURE_COPY_LOCATION srcLoc(srcBuf->buffer.Get(), footprint);
    CD3DX12_TEXTURE_COPY_LOCATION destLoc(destTex->resource.Get(), destSubresource);

    CD3DX12_BOX srcBox(0, 0, 0,
                       (LONG)destRegion.w,
                       (LONG)destRegion.h,
                       (LONG)(destRegion.d == 0 ? 1 : destRegion.d));
    commandList->CopyTextureRegion(&destLoc,
                                   destRegion.x, destRegion.y, destRegion.z,
                                   &srcLoc, &srcBox);
}

void GED3D12CommandBuffer::copyTextureToBuffer(SharedHandle<GETexture> &src, SharedHandle<GEBuffer> &dest,
                                               size_t bytesPerRow, size_t bytesPerImage,
                                               const TextureRegion &srcRegion, size_t destBufferOffset) {
    (void)bytesPerImage;
    d3d12RequireOrReturn(inBlitPass, DEBUG_DOMAIN_RESOURCE, "copyTextureToBuffer called outside a blit pass");
    auto *srcTex = (GED3D12Texture *)src.get();
    auto *destBuf = (GED3D12Buffer *)dest.get();

    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (srcTex->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        if (srcTex->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcTex->resource.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            srcTex->resource.Get(), srcTex->currentState, D3D12_RESOURCE_STATE_COPY_SOURCE));
        srcTex->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    if (destBuf->currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        if (destBuf->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(destBuf->buffer.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            destBuf->buffer.Get(), destBuf->currentState, D3D12_RESOURCE_STATE_COPY_DEST));
        destBuf->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    footprint.Offset = destBufferOffset;
    footprint.Footprint.Format = srcTex->resource->GetDesc().Format;
    footprint.Footprint.Width = srcRegion.w;
    footprint.Footprint.Height = srcRegion.h;
    footprint.Footprint.Depth = srcRegion.d == 0 ? 1 : srcRegion.d;
    footprint.Footprint.RowPitch = static_cast<UINT>(bytesPerRow);

    // §7.1: read from the (mipLevel, arrayLayer) subresource rather than 0.
    const auto srcDesc = srcTex->resource->GetDesc();
    const UINT srcSubresource = D3D12CalcSubresource(srcRegion.mipLevel, srcRegion.arrayLayer,
                                                     0, srcDesc.MipLevels, srcDesc.DepthOrArraySize);

    CD3DX12_TEXTURE_COPY_LOCATION srcLoc(srcTex->resource.Get(), srcSubresource);
    CD3DX12_TEXTURE_COPY_LOCATION destLoc(destBuf->buffer.Get(), footprint);

    CD3DX12_BOX srcBox((LONG)srcRegion.x,
                       (LONG)srcRegion.y,
                       (LONG)srcRegion.z,
                       (LONG)(srcRegion.x + srcRegion.w),
                       (LONG)(srcRegion.y + srcRegion.h),
                       (LONG)(srcRegion.z + (srcRegion.d == 0 ? 1 : srcRegion.d)));
    commandList->CopyTextureRegion(&destLoc, 0, 0, 0, &srcLoc, &srcBox);
}

void GED3D12CommandBuffer::generateMipmaps(SharedHandle<GETexture> &texture) {
    d3d12RequireOrReturn(inBlitPass, DEBUG_DOMAIN_RESOURCE, "generateMipmaps called outside a blit pass");
    auto *tex = (GED3D12Texture *)texture.get();
    const auto texDesc = tex->resource->GetDesc();
    if (texDesc.MipLevels <= 1) {
        return;
    }
    if (texDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        texDesc.DepthOrArraySize != 1) {
        DEBUG_ERROR(DEBUG_DOMAIN_RESOURCE, "GED3D12CommandBuffer::generateMipmaps: only 2D, single-slice "
                     "textures are supported by the box-filter compute kernel. tex="
                     << tex);
        return;
    }
    if ((texDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        DEBUG_ERROR(DEBUG_DOMAIN_RESOURCE, "GED3D12CommandBuffer::generateMipmaps: texture was not created "
                     "with ALLOW_UNORDERED_ACCESS; cannot bind mip levels as UAVs. tex="
                     << tex);
        return;
    }

    auto *engine = parentQueue->engine;
    if (!engine->ensureMipmapGenPipeline()) {
        DEBUG_ERROR(DEBUG_DOMAIN_PIPELINE, "GED3D12CommandBuffer::generateMipmaps: pipeline init failed.");
        return;
    }

    auto *device = engine->d3d12_device.Get();
    auto *pipeline = (GED3D12ComputePipelineState *)engine->mipmapGenPipeline.get();
    auto &shaderInternal = pipeline->computeShader->internal;

    // Point `currentRootSignature` at the mipmap-gen pipeline's root
    // signature *before* asking `getRootParameterIndexOfResource` to
    // walk it. `currentRootSignature` is normally assigned by
    // `setComputePipelineState`, but this function bypasses that path
    // (the per-mip loop drives the dispatch directly). Without this
    // assignment the lookup walks a null/stale root-signature pointer
    // and access-violates on `currentRootSignature->NumParameters`.
    // We assign here rather than calling `setComputePipelineState`
    // because the dispatch loop below issues its own
    // `SetPipelineState` / `SetComputeRootSignature` interleaved with
    // per-mip barriers, and routing through `setComputePipelineState`
    // would also flip the `inComputePass` guard which doesn't apply
    // inside a blit pass.
    currentRootSignature = &pipeline->rootSignatureDesc;

    // OmegaSL location 0 = srcMip (in / SRV), 1 = dstMip (out / UAV).
    unsigned srvId = 0, uavId = 1;
    const unsigned srvRoot = getRootParameterIndexOfResource(srvId, shaderInternal);
    const unsigned uavRoot = getRootParameterIndexOfResource(uavId, shaderInternal);

    const UINT mipCount = texDesc.MipLevels;
    const DXGI_FORMAT format = texDesc.Format;

    // Phase 3 — pull (mipCount-1)*2 contiguous slots from the per-queue
    // transient ring instead of creating a one-off descriptor heap per
    // call. Gated on the next submit's retentionFence value: once this
    // queue advances past it, the ring recycles the slots.
    const UINT slotsNeeded = (mipCount - 1) * 2;
    D3D12DescriptorHandle ringSlot = parentQueue->transientRing->allocate(
        slotsNeeded, parentQueue->gateForNextSubmit());
    if (!ringSlot.valid()) {
        DEBUG_ERROR(DEBUG_DOMAIN_RESOURCE, "GED3D12CommandBuffer::generateMipmaps: transient ring "
                     "exhausted (need=" << slotsNeeded
                     << ", capacity=" << parentQueue->transientRing->capacity() << ")");
        return;
    }

    const UINT incr = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuStart(ringSlot.cpu);

    for (UINT i = 0; i + 1 < mipCount; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = i;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(tex->resource.Get(), &srvDesc, cpuStart);
        cpuStart.Offset(1, incr);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = format;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = i + 1;
        device->CreateUnorderedAccessView(tex->resource.Get(), nullptr, &uavDesc, cpuStart);
        cpuStart.Offset(1, incr);
    }

    // Move the whole resource into UAV state so per-mip subresource transitions below are valid.
    if (tex->currentState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            tex->resource.Get(), tex->currentState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &barrier);
        tex->currentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    ID3D12DescriptorHeap *heaps[] = { parentQueue->transientRing->heap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(pipeline->rootSignature.Get());
    commandList->SetPipelineState(pipeline->pipelineState.Get());

    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuStart(ringSlot.gpu);

    for (UINT i = 0; i + 1 < mipCount; ++i) {
        const UINT64 dstW = std::max<UINT64>(1, texDesc.Width  >> (i + 1));
        const UINT   dstH = std::max<UINT>  (1, texDesc.Height >> (i + 1));

        // Transition src mip i → NON_PIXEL_SHADER_RESOURCE; dst mip i+1 stays in UAV.
        auto preBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            tex->resource.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            i);
        commandList->ResourceBarrier(1, &preBarrier);

        CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(gpuStart, i * 2,     incr);
        CD3DX12_GPU_DESCRIPTOR_HANDLE uavHandle(gpuStart, i * 2 + 1, incr);
        commandList->SetComputeRootDescriptorTable(srvRoot, srvHandle);
        commandList->SetComputeRootDescriptorTable(uavRoot, uavHandle);

        const UINT groupsX = static_cast<UINT>((dstW + 7) / 8);
        const UINT groupsY = static_cast<UINT>((dstH + 7) / 8);
        commandList->Dispatch(groupsX, groupsY, 1);

        // UAV barrier on dst mip + transition src mip i back to UAV so we end in a uniform state.
        D3D12_RESOURCE_BARRIER postBarriers[2];
        postBarriers[0] = CD3DX12_RESOURCE_BARRIER::UAV(tex->resource.Get());
        postBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            tex->resource.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            i);
        commandList->ResourceBarrier(2, postBarriers);
    }

    // Phase 3 — no per-call descriptor heap to retain; the ring slot is
    // gated on parentQueue->gateForNextSubmit() at allocate-time and the
    // ring retires it once that fence value signals.
}

void GED3D12CommandBuffer::fillBuffer(SharedHandle<GEBuffer> &buffer, uint32_t value,
                                      size_t offset, size_t size) {
    d3d12RequireOrReturn(inBlitPass, DEBUG_DOMAIN_RESOURCE, "fillBuffer called outside a blit pass");
    auto *buf = (GED3D12Buffer *)buffer.get();
    const auto bufDesc = buf->buffer->GetDesc();
    const UINT64 totalSize = bufDesc.Width;
    const UINT64 fillOffset = static_cast<UINT64>(offset);
    const UINT64 fillSize =
        size == 0 ? (totalSize - fillOffset) : static_cast<UINT64>(size);

    if ((bufDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        // ClearUnorderedAccessViewUint requires the buffer to have been created
        // as a UAV. Buffers without UAV access need either a staging upload path
        // or a compute shader fill; neither is currently wired up.
        DEBUG_ERROR(DEBUG_DOMAIN_RESOURCE, "GED3D12CommandBuffer::fillBuffer: buffer was not created "
                     "with ALLOW_UNORDERED_ACCESS; fill skipped. Requires UAV-"
                     "capable buffer or compute-shader path. buffer="
                     << buf);
        return;
    }

    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (buf->currentState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            buf->buffer.Get(), buf->currentState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        buf->currentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }

    // Phase 1 (Shared-Descriptor-Heap-Plan): buffers no longer own a
    // per-resource descriptor heap. ClearUnorderedAccessViewUint needs a
    // matched (CPU, GPU) UAV-handle pair, so we rewrite a raw-buffer UAV
    // into the engine's single-slot helper heap on every call. Safe while
    // fills are serialized per command queue — ClearUAV reads the
    // descriptor synchronously at record time, so the previous fill's
    // descriptor is no longer referenced once we return. If concurrent
    // fills across command buffers become possible, promote the helper
    // to a fence-keyed ring.
    ID3D12DescriptorHeap *heap = parentQueue->engine->clearUavHelperHeap.Get();
    if (heap == nullptr) {
        DEBUG_ERROR(DEBUG_DOMAIN_RESOURCE, "GED3D12CommandBuffer::fillBuffer: engine clearUavHelperHeap "
                     "is null; cannot resolve UAV handle.");
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = heap->GetGPUDescriptorHandleForHeapStart();

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = static_cast<UINT>(totalSize / 4);
    uavDesc.Buffer.StructureByteStride = 0;
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    parentQueue->engine->d3d12_device->CreateUnorderedAccessView(
        buf->buffer.Get(), nullptr, &uavDesc, cpuHandle);

    const UINT values[4] = {value, value, value, value};
    commandList->SetDescriptorHeaps(1, &heap);
    commandList->ClearUnorderedAccessViewUint(gpuHandle, cpuHandle,
                                              buf->buffer.Get(), values, 0, nullptr);
    (void)fillOffset;
    (void)fillSize;
}

void GED3D12CommandBuffer::finishBlitPass() {
    inBlitPass = false;
    DEBUG_TRACE(DEBUG_DOMAIN_QUEUE, "BlitPass end");
};

void GED3D12CommandBuffer::blitWithPipeline(SharedHandle<GEBlitPipelineState> &pipelineState,
                                            SharedHandle<GETexture> &src,
                                            SharedHandle<GETexture> &dest) {
    auto *dst = (GED3D12Texture *)dest.get();
    auto descD = dst->resource->GetDesc();
    TextureRegion srcRegion{0, 0, 0, (unsigned)descD.Width, descD.Height, 1};
    TextureRegion destRegion{0, 0, 0, (unsigned)descD.Width, descD.Height, 1};
    blitWithPipeline(pipelineState, src, dest, srcRegion, destRegion);
}

void GED3D12CommandBuffer::blitWithPipeline(SharedHandle<GEBlitPipelineState> &pipelineState,
                                            SharedHandle<GETexture> &src,
                                            SharedHandle<GETexture> &dest,
                                            const TextureRegion &srcRegion,
                                            const TextureRegion &destRegion) {
    (void)srcRegion;
    d3d12RequireOrReturn(!inRenderPass && !inBlitPass && !inComputePass, DEBUG_DOMAIN_RENDERTGT,
                         "blitWithPipeline must not be called inside an existing pass scope");
    d3d12RequireOrReturn(pipelineState, DEBUG_DOMAIN_PIPELINE, "blitWithPipeline: pipelineState is null");
    auto *blitPipe = (GED3D12BlitPipelineState *)pipelineState.get();
    if (!blitPipe->renderPipeline) {
        DEBUG_ERROR(DEBUG_DOMAIN_PIPELINE, "blitWithPipeline: underlying render pipeline is null");
        return;
    }

    // One-shot texture render target wrapping `dest`. The SharedHandle is
    // kept on the stack so the underlying object outlives the pass.
    TextureRenderTargetDescriptor trtDesc{};
    trtDesc.renderToExistingTexture = true;
    trtDesc.texture = dest;
    auto trtSh = parentQueue->engine->makeTextureRenderTarget(trtDesc);
    if (!trtSh) {
        DEBUG_ERROR(DEBUG_DOMAIN_RENDERTGT, "blitWithPipeline: makeTextureRenderTarget failed");
        return;
    }

    GERenderPassDescriptor rpDesc{};
    rpDesc.tRenderTarget = trtSh.get();
    rpDesc.colorAttachments.emplace_back(
        GERenderPassDescriptor::ColorAttachment::ClearColor(0.f, 0.f, 0.f, 0.f),
        GERenderPassDescriptor::ColorAttachment::Discard);
    rpDesc.depthStencilAttachment.disabled = true;

    startRenderPass(rpDesc);
    setRenderPipelineState(blitPipe->renderPipeline);
    bindResourceAtFragmentShader(src, 0, TextureSwizzle::identity());
    GEViewport vp{(float)destRegion.x, (float)destRegion.y,
                  (float)destRegion.w, (float)destRegion.h,
                  0.f, 1.f};
    setViewports({vp});
    GEScissorRect sr{(float)destRegion.x, (float)destRegion.y,
                     (float)destRegion.w, (float)destRegion.h};
    setScissorRects({sr});
    drawPolygons(GECommandBuffer::Triangle, 3, 0);
    finishRenderPass();
}

void GED3D12CommandBuffer::beginAccelStructPass() {}

static void fillGeometryDescsFromGE(const GEAccelerationStructDescriptor &desc,
                                    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> &out) {
    for (auto &g : desc.data) {
        D3D12_RAYTRACING_GEOMETRY_DESC gd{};
        gd.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        if (g.type == GEAccelerationStructDescriptor::Geometry::TRIANGLES) {
            gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            const auto &tl = g.getTriangleList();
            auto d3dBuf = std::dynamic_pointer_cast<GED3D12Buffer>(tl.buffer);
            if (d3dBuf) {
                /// Raytracing plan §6-M1 — an explicit stride/count traces a
                /// vertex buffer that interleaves more than position (positions
                /// are read from offset 0). Zero means the historical default:
                /// a tightly-packed float3 position buffer.
                const size_t stride = tl.vertexStride ? tl.vertexStride : (sizeof(float) * 3);
                const size_t count  = tl.vertexCount ? tl.vertexCount : (d3dBuf->size() / stride);
                gd.Triangles.VertexBuffer.StartAddress = d3dBuf->buffer->GetGPUVirtualAddress();
                gd.Triangles.VertexBuffer.StrideInBytes = stride;
                gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                gd.Triangles.VertexCount = static_cast<UINT>(count);
            }
        } else {
            gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
            auto d3dBuf = std::dynamic_pointer_cast<GED3D12Buffer>(g.getAabb().buffer);
            if (d3dBuf) {
                gd.AABBs.AABBs.StartAddress = d3dBuf->buffer->GetGPUVirtualAddress();
                gd.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
                gd.AABBs.AABBCount = d3dBuf->size() / sizeof(D3D12_RAYTRACING_AABB);
            }
        }
        out.push_back(gd);
    }
}

/// Raytracing plan §6.2 — translate the GE descriptor's TLAS instances into a
/// `D3D12_RAYTRACING_INSTANCE_DESC` array (field-by-field, since the GE bitfield
/// packing need not match D3D12's), upload it to an Upload-heap buffer the build
/// reads, store that buffer on `dst` (so it outlives the recorded command until
/// the GPU consumes it), and fill the build inputs' Type / NumDescs /
/// InstanceDescs. Shared by build and refit.
static void fillTLASInstancesFromGE(GED3D12Engine *engine,
                                    const GEAccelerationStructDescriptor &desc,
                                    GED3D12AccelerationStruct *dst,
                                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs) {
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
    instanceDescs.reserve(desc.instances.size());
    for (auto &inst : desc.instances) {
        D3D12_RAYTRACING_INSTANCE_DESC id{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                id.Transform[r][c] = inst.transform[r][c];
        id.InstanceID = inst.instanceID;
        id.InstanceMask = inst.instanceMask;
        id.InstanceContributionToHitGroupIndex = inst.instanceContributionToHitGroupIndex;
        id.Flags = inst.flags;
        auto blas = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(inst.blas);
        id.AccelerationStructure =
            blas ? blas->structBuffer->buffer->GetGPUVirtualAddress() : 0;
        instanceDescs.push_back(id);
    }

    size_t bytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instanceDescs.size();
    auto instBuf = std::dynamic_pointer_cast<GED3D12Buffer>(
        engine->makeBuffer({BufferDescriptor::Upload,
                            bytes ? bytes : sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                            sizeof(D3D12_RAYTRACING_INSTANCE_DESC)}));
    if (!instanceDescs.empty()) {
        CD3DX12_RANGE noRead(0, 0);
        void *dataPtr = nullptr;
        instBuf->buffer->Map(0, &noRead, &dataPtr);
        memmove(dataPtr, instanceDescs.data(), bytes);
        instBuf->buffer->Unmap(0, nullptr);
    }
    dst->instanceBuffer = instBuf;

    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.NumDescs = static_cast<UINT>(instanceDescs.size());
    inputs.InstanceDescs = instBuf->buffer->GetGPUVirtualAddress();
}

void GED3D12CommandBuffer::buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,
                                                      const GEAccelerationStructDescriptor &desc) {
    auto accel_struct = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(src);

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
    fillGeometryDescsFromGE(desc, geometryDescs);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.SourceAccelerationStructureData = NULL;
    d.DestAccelerationStructureData = accel_struct->structBuffer->buffer->GetGPUVirtualAddress();
    d.ScratchAccelerationStructureData = accel_struct->scratchBuffer->buffer->GetGPUVirtualAddress();
    d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    if (desc.level == GEAccelerationStructDescriptor::TopLevel) {
        fillTLASInstancesFromGE(parentQueue->engine, desc, accel_struct.get(), d.Inputs);
    } else {
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        d.Inputs.NumDescs = static_cast<UINT>(geometryDescs.size());
        d.Inputs.pGeometryDescs = geometryDescs.data();
    }

    commandList->BuildRaytracingAccelerationStructure(&d, 0, nullptr);

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(accel_struct->structBuffer->buffer.Get());
    commandList->ResourceBarrier(1, &uavBarrier);
}

void GED3D12CommandBuffer::copyAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,
                                                     SharedHandle<GEAccelerationStruct> &dest) {
    auto srcAS = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(src);
    auto destAS = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(dest);
    commandList->CopyRaytracingAccelerationStructure(destAS->structBuffer->buffer->GetGPUVirtualAddress(),
                                                     srcAS->structBuffer->buffer->GetGPUVirtualAddress(),
                                                     D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(destAS->structBuffer->buffer.Get());
    commandList->ResourceBarrier(1, &uavBarrier);
}

void GED3D12CommandBuffer::refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,
                                                      SharedHandle<GEAccelerationStruct> &dest,
                                                      const GEAccelerationStructDescriptor &desc) {
    auto accel_struct_src = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(src);
    auto accel_struct_dest = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(dest);

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
    fillGeometryDescsFromGE(desc, geometryDescs);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.SourceAccelerationStructureData = accel_struct_src->structBuffer->buffer->GetGPUVirtualAddress();
    d.DestAccelerationStructureData = accel_struct_dest->structBuffer->buffer->GetGPUVirtualAddress();
    d.ScratchAccelerationStructureData = accel_struct_dest->scratchBuffer->buffer->GetGPUVirtualAddress();
    d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    if (desc.level == GEAccelerationStructDescriptor::TopLevel) {
        /// Raytracing plan §6.2 — a TLAS update re-uploads its instance descs
        /// (transforms may have changed) onto the destination and points the
        /// update at them, same as the initial build.
        fillTLASInstancesFromGE(parentQueue->engine, desc, accel_struct_dest.get(), d.Inputs);
    } else {
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        d.Inputs.NumDescs = static_cast<UINT>(geometryDescs.size());
        d.Inputs.pGeometryDescs = geometryDescs.data();
    }

    commandList->BuildRaytracingAccelerationStructure(&d, 0, nullptr);

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(accel_struct_dest->structBuffer->buffer.Get());
    commandList->ResourceBarrier(1, &uavBarrier);
}

void GED3D12CommandBuffer::finishAccelStructPass() {
    
}

void GED3D12CommandBuffer::startRenderPass(const GERenderPassDescriptor &desc) {
    d3d12RequireOrReturn(!inComputePass, DEBUG_DOMAIN_RENDERTGT,
                         "startRenderPass called while a compute pass is active");
    inRenderPass = true;
    DEBUG_TRACE(DEBUG_DOMAIN_RENDERTGT, "RenderPass begin");
    static constexpr unsigned kMaxRT = 8;
    D3D12_RENDER_PASS_RENDER_TARGET_DESC rt_descs[kMaxRT] = {};
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds_desc;

    D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_PARAMETERS resolveParams;

    CD3DX12_CPU_DESCRIPTOR_HANDLE cpu_handle;
    CD3DX12_CPU_DESCRIPTOR_HANDLE ds_cpu_handle;

    // Depth is only bindable where a DSV actually exists: on a texture render
    // target (or an MSAA resolve source) that owns a depth surface. A native
    // target is color only, so this can be forced off below.
    bool depthEnabled = !desc.depthStencilAttachment.disabled;

    const auto rtvDescSize =
        parentQueue->engine->d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (desc.nRenderTarget) {
        auto *nativeRenderTarget = (GED3D12NativeRenderTarget *)desc.nRenderTarget;
        if (desc.multisampleResolve) {
            multisampleResolvePass = true;
            auto resolveTexture = (GED3D12Texture *)desc.resolveDesc.multiSampleTextureSrc.get();
            cpu_handle =
                CD3DX12_CPU_DESCRIPTOR_HANDLE(resolveTexture->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());

            if (depthEnabled) {
                ds_cpu_handle =
                    CD3DX12_CPU_DESCRIPTOR_HANDLE(resolveTexture->dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
            }

            // Resolve into the back buffer from its per-buffer tracked state
            // (see the non-MSAA branch below): a presented frame can span
            // several command buffers, so the per-command-buffer
            // `firstRenderPass` flag is the wrong source of truth for the back
            // buffer's current state. finishRenderPass records the post-resolve
            // RENDER_TARGET state; present() flips it to PRESENT.
            const auto frameIdx = nativeRenderTarget->frameIndex;
            const D3D12_RESOURCE_STATES resource_state =
                (frameIdx < nativeRenderTarget->renderTargetStates.size())
                    ? nativeRenderTarget->renderTargetStates[frameIdx]
                    : D3D12_RESOURCE_STATE_PRESENT;

            auto barrier =
                CD3DX12_RESOURCE_BARRIER::Transition(nativeRenderTarget->renderTargets[frameIdx],
                                                     resource_state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            commandList->ResourceBarrier(1, &barrier);
            if (frameIdx < nativeRenderTarget->renderTargetStates.size()) {
                nativeRenderTarget->renderTargetStates[frameIdx] = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            }

            auto dxgi_format = nativeRenderTarget->renderTargets[nativeRenderTarget->frameIndex]->GetDesc().Format;
            RECT rc;
            GetClientRect(nativeRenderTarget->hwnd, &rc);
            resolveParams.pSrcResource = resolveTexture->resource.Get();
            resolveParams.SubresourceCount = 1;
            resolveParams.PreserveResolveSource = TRUE;
            resolveParams.pDstResource = nativeRenderTarget->renderTargets[nativeRenderTarget->frameIndex];
            D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS params;
            params.DstSubresource = 0;
            params.SrcSubresource = 0;
            params.DstX = 0;
            params.DstY = 0;
            params.SrcRect = CD3DX12_RECT(rc.left, rc.top, rc.right, rc.bottom);
            resolveParams.pSubresourceParameters = &params;
            resolveParams.Format = dxgi_format;
            resolveParams.ResolveMode = D3D12_RESOLVE_MODE_MAX;
        } else {
            cpu_handle =
                CD3DX12_CPU_DESCRIPTOR_HANDLE(nativeRenderTarget->rtvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                                              nativeRenderTarget->frameIndex, rtvDescSize);
            /// The swap chain owns no depth surface, so there is no DSV to bind
            /// (this used to hand the pass DSV descriptors that aliased the COLOR
            /// back buffer — invalid, and silently broken depth). Depth against a
            /// drawable is a caller-contract violation: render 3D into a
            /// GETextureRenderTarget with a depthTexture, then blit here. Mirrors
            /// the Metal backend's guard.
            if (depthEnabled) {
                DEBUG_CRITICAL(DEBUG_DOMAIN_RENDERTGT,
                    "startRenderPass: depth/stencil is not supported on a native render target — "
                    "render 3D into a GETextureRenderTarget with a depthTexture, then blit to the drawable");
                depthEnabled = false;
            }
            // Move the back buffer to RENDER_TARGET, gating on the state
            // tracked per buffer on the native target rather than the
            // per-command-buffer `firstRenderPass` flag. A presented frame can
            // span several command buffers — the compositor suspends the frame
            // for each content-cache capture / blur scratch pass and resumes it
            // on a fresh buffer, each of which is `firstRenderPass`. Gating on
            // that flag re-emitted PRESENT->RENDER_TARGET on the resumed buffer
            // and tripped the D3D12 debug layer (back buffer already in
            // RENDER_TARGET). The tracked state makes the transition idempotent;
            // present() flips it back to PRESENT.
            const auto frameIdx = nativeRenderTarget->frameIndex;
            if (frameIdx < nativeRenderTarget->renderTargetStates.size() &&
                nativeRenderTarget->renderTargetStates[frameIdx] != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    nativeRenderTarget->renderTargets[frameIdx],
                    nativeRenderTarget->renderTargetStates[frameIdx],
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
                commandList->ResourceBarrier(1, &barrier);
                nativeRenderTarget->renderTargetStates[frameIdx] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
        }
        currentTarget.native = nativeRenderTarget;
    } else if (desc.tRenderTarget) {
        auto *textureRenderTarget = (GED3D12TextureRenderTarget *)desc.tRenderTarget;
        if (desc.multisampleResolve) {
            auto resolveTexture = (GED3D12Texture *)desc.resolveDesc.multiSampleTextureSrc.get();
            cpu_handle =
                CD3DX12_CPU_DESCRIPTOR_HANDLE(resolveTexture->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
            if (depthEnabled) {
                ds_cpu_handle =
                    CD3DX12_CPU_DESCRIPTOR_HANDLE(resolveTexture->dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
            }

            auto *targetTexture = textureRenderTarget->texture.get();
            if (targetTexture != nullptr) {
                auto currentState = targetTexture->currentState;
                auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(targetTexture->resource.Get(), currentState,
                                                                    D3D12_RESOURCE_STATE_RESOLVE_DEST);
                commandList->ResourceBarrier(1, &barrier);
                targetTexture->currentState = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            }

            auto desc = textureRenderTarget->texture->resource->GetDesc();
            auto dxgi_format = desc.Format;

            resolveParams.pSrcResource = resolveTexture->resource.Get();
            resolveParams.SubresourceCount = 1;
            resolveParams.PreserveResolveSource = TRUE;
            resolveParams.pDstResource = textureRenderTarget->texture->resource.Get();
            D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS params;
            params.DstSubresource = 0;
            params.SrcSubresource = 0;
            params.DstX = 0;
            params.DstY = 0;
            params.SrcRect = CD3DX12_RECT(0, 0, desc.Width, desc.Height);
            resolveParams.pSubresourceParameters = &params;
            resolveParams.Format = dxgi_format;
            resolveParams.ResolveMode = D3D12_RESOLVE_MODE_MAX;
        } else {
            auto *targetTexture = textureRenderTarget->texture.get();
            if (targetTexture != nullptr) {
                if (targetTexture->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                    auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(targetTexture->resource.Get());
                    commandList->ResourceBarrier(1, &barrier);
                }
                if (!(targetTexture->currentState & D3D12_RESOURCE_STATE_RENDER_TARGET)) {
                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                        targetTexture->resource.Get(), targetTexture->currentState, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    commandList->ResourceBarrier(1, &barrier);
                    targetTexture->currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
            }
            cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
                textureRenderTarget->texture->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
            if (depthEnabled) {
                ds_cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
                    textureRenderTarget->texture->dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
            }
        }
        currentTarget.texture = textureRenderTarget;
    };

    const unsigned attachmentCount =
        desc.colorAttachments.empty() ? 1u : (unsigned)std::min<size_t>(desc.colorAttachments.size(), kMaxRT);

    for (unsigned i = 0; i < attachmentCount; ++i) {
        D3D12_RENDER_PASS_RENDER_TARGET_DESC &rt_desc = rt_descs[i];
        CD3DX12_CPU_DESCRIPTOR_HANDLE attachmentHandle;
        const GERenderPassDescriptor::ColorAttachment *attachment =
            desc.colorAttachments.empty() ? nullptr : &desc.colorAttachments[i];

        if (i == 0 && (attachment == nullptr || attachment->texture == nullptr)) {
            attachmentHandle = cpu_handle;
        } else {
            d3d12RequireOrReturn(attachment != nullptr && attachment->texture != nullptr, DEBUG_DOMAIN_RENDERTGT,
                   "Color attachments beyond index 0 must supply an explicit texture.");
            auto *attachTexture = (GED3D12Texture *)attachment->texture.get();
            if (attachTexture->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(attachTexture->resource.Get());
                commandList->ResourceBarrier(1, &barrier);
            }
            if (!(attachTexture->currentState & D3D12_RESOURCE_STATE_RENDER_TARGET)) {
                auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    attachTexture->resource.Get(), attachTexture->currentState,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
                commandList->ResourceBarrier(1, &barrier);
                attachTexture->currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            attachmentHandle =
                CD3DX12_CPU_DESCRIPTOR_HANDLE(attachTexture->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
        }

        rt_desc.cpuDescriptor = attachmentHandle;

        if (i == 0 && desc.multisampleResolve) {
            rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
            rt_desc.EndingAccess.Resolve = resolveParams;
        }

        const auto loadAction = (attachment != nullptr)
                                    ? attachment->loadAction
                                    : GERenderPassDescriptor::ColorAttachment::Discard;
        const bool useResolveEnd = (i == 0 && desc.multisampleResolve);

        switch (loadAction) {
            case GERenderPassDescriptor::ColorAttachment::Load: {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if (!useResolveEnd)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::LoadPreserve: {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if (!useResolveEnd)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::Discard: {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
                if (!useResolveEnd)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::Clear: {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                const FLOAT colors[] = {
                    attachment ? attachment->clearColor.r : 0.f,
                    attachment ? attachment->clearColor.g : 0.f,
                    attachment ? attachment->clearColor.b : 0.f,
                    attachment ? attachment->clearColor.a : 0.f,
                };
                rt_desc.BeginningAccess.Clear.ClearValue =
                    CD3DX12_CLEAR_VALUE(DXGI_FORMAT_R8G8B8A8_UNORM, colors);
                if (!useResolveEnd)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
        }
    }

    if (!depthEnabled) {
        commandList->BeginRenderPass(attachmentCount, rt_descs, nullptr, D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES);
    } else {
        ds_desc.cpuDescriptor = ds_cpu_handle;


        if (desc.multisampleResolve) {
            ds_desc.DepthEndingAccess.Type = ds_desc.StencilEndingAccess.Type =
                D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
            resolveParams.Format = DXGI_FORMAT_UNKNOWN;
            ds_desc.DepthEndingAccess.Resolve = ds_desc.StencilEndingAccess.Resolve = resolveParams;
        }

        switch (desc.depthStencilAttachment.depthloadAction) {
            case GERenderPassDescriptor::DepthStencilAttachment::Discard : {
                   ds_desc.DepthBeginningAccess.Type = ds_desc.StencilBeginningAccess.Type =
                    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
                if (!desc.multisampleResolve)
                    ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Load: {
                ds_desc.DepthBeginningAccess.Type = ds_desc.StencilBeginningAccess.Type =
                    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                if (!desc.multisampleResolve)
                    ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::LoadPreserve: {
                ds_desc.DepthBeginningAccess.Type = ds_desc.StencilBeginningAccess.Type =
                    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if (!desc.multisampleResolve)
                    ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Clear: {
                ds_desc.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                ds_desc.DepthBeginningAccess.Clear.ClearValue =
                    CD3DX12_CLEAR_VALUE(DXGI_FORMAT_UNKNOWN, desc.depthStencilAttachment.clearDepth,
                                        desc.depthStencilAttachment.clearStencil);
                if (!desc.multisampleResolve)
                    ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
        }

        switch (desc.depthStencilAttachment.stencilLoadAction) {
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Discard: {
                ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
                if (!desc.multisampleResolve)
                    ds_desc.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Load: {
                ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if (!desc.multisampleResolve)
                    ds_desc.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::LoadPreserve: {
                ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if (!desc.multisampleResolve)
                    ds_desc.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Clear: {
                ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                ds_desc.StencilBeginningAccess.Clear.ClearValue =
                    CD3DX12_CLEAR_VALUE(DXGI_FORMAT_UNKNOWN, desc.depthStencilAttachment.clearDepth,
                                        desc.depthStencilAttachment.clearStencil);
                if (!desc.multisampleResolve)
                    ds_desc.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
        }

        commandList->BeginRenderPass(attachmentCount, rt_descs, &ds_desc, D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES);
    }
};

void GED3D12CommandBuffer::startTessRenderPass(const GERenderPassDescriptor &desc) {
    /// §16 Phase H — on D3D12 the HS/DS execute inside the one graphics
    /// pipeline, so a tessellated draw needs no separate compute pre-pass
    /// (unlike Metal). This is the normal render-pass setup plus a flag that
    /// (a) allows `drawPatches` and (b) is what makes plain `startRenderPass`
    /// and this one differ: a tessellation pipeline may only be bound inside a
    /// tess pass. The render-target setup is identical, so reusing
    /// `startRenderPass` keeps the two byte-compatible.
    startRenderPass(desc);
    tessPassActive = true;
}

void GED3D12CommandBuffer::setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState) {
    d3d12RequireOrReturn(!inComputePass, DEBUG_DOMAIN_PIPELINE, "setRenderPipelineState called while a compute pass is active");
    DEBUG_TRACE(DEBUG_DOMAIN_PIPELINE, "PSO set");
    auto *d3d12_pipeline_state = (GED3D12RenderPipelineState *)pipelineState.get();
    /// §16 Phase H — a tessellation pipeline may only be bound inside a
    /// `startTessRenderPass` scope; binding one in a plain render pass is a
    /// caller-contract violation (no patch topology / tessellator is set up).
    /// Refuse rather than hand the driver a patch-topology draw with no tess
    /// pass (mirrors the Vulkan backend's guard).
    if(d3d12_pipeline_state->isTess && !tessPassActive){
        DEBUG_CRITICAL(DEBUG_DOMAIN_PIPELINE, "setRenderPipelineState: a tessellation pipeline must be bound inside "
                       "startTessRenderPass, not startRenderPass.");
        assert(false && "tessellation pipeline bound outside a tess render pass");
        return;
    }
    commandList->SetPipelineState(d3d12_pipeline_state->pipelineState.Get());
    currentRenderPipeline = d3d12_pipeline_state;
    commandList->SetGraphicsRootSignature(d3d12_pipeline_state->rootSignature.Get());
    currentRootSignature = &d3d12_pipeline_state->rootSignatureDesc;
};

void GED3D12CommandBuffer::reportTransitionInsideRenderPass(const char *resourceKind,
                                                            D3D12_RESOURCE_STATES fromState,
                                                            D3D12_RESOURCE_STATES toState) const {
    DEBUG_CRITICAL(DEBUG_DOMAIN_RESOURCE, "[GED3D12CommandBuffer] " << resourceKind
              << " state transition requested inside an active render pass scope — skipping "
                 "(D3D12 forbids transition barriers between BeginRenderPass/EndRenderPass). from=0x"
              << std::hex << (unsigned)fromState << " to=0x" << (unsigned)toState << std::dec
              << ". The frontend must reach the required state before the pass begins.");
    assert(false && "D3D12 resource transition requested inside an active render pass; "
                    "reach the required state before binding this resource in the pass.");
}

void GED3D12CommandBuffer::bindResourceAtVertexShader(SharedHandle<GEBuffer> &buffer, unsigned int index) {
    d3d12RequireOrReturn(!inComputePass && !inBlitPass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtVertexShader(buffer) called outside a render pass");
    auto *d3d12_buffer = (GED3D12Buffer *)buffer.get();

    auto required_state = getRequiredResourceStateForResourceID(index, currentRenderPipeline->vertexShader->internal);

    if (d3d12_buffer->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_buffer->buffer.Get());
        commandList->ResourceBarrier(1, &barrier);
    }

    if (!(d3d12_buffer->currentState & required_state)) {
        if (inRenderPass) {
            reportTransitionInsideRenderPass("buffer", d3d12_buffer->currentState, required_state);
        } else {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                d3d12_buffer->buffer.Get(), d3d12_buffer->currentState, required_state);
            commandList->ResourceBarrier(1, &barrier);
            d3d12_buffer->currentState = required_state;
        }
    }

    // Phase 1 (Shared-Descriptor-Heap-Plan): the prior
    // SetDescriptorHeaps(bufferDescHeap) here was vestigial — every
    // buffer bind below goes through SetGraphics*View on the GPU
    // virtual address, which does not read a descriptor heap — and it
    // also clobbered whichever shader-visible heap a prior texture or
    // sampler bind required. Removing it is the fix for both.

    const auto rootParam = getRootParameterIndexOfResource(index, currentRenderPipeline->vertexShader->internal);

    if (d3d12_buffer->role == BufferDescriptor::Uniform) {
        // §2.4 constant buffer — root CBV (the root-param lookup already
        // resolved the matching CBV parameter from the shader layout).
        commandList->SetGraphicsRootConstantBufferView(rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else if (d3d12_buffer->currentState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        commandList->SetGraphicsRootShaderResourceView(rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else {
        commandList->SetGraphicsRootUnorderedAccessView(rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
    }
};

void GED3D12CommandBuffer::bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned int index,
                                                       const TextureSwizzle & swizzle) {
    d3d12RequireOrReturn(!inComputePass && !inBlitPass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtVertexShader(texture) called outside a render pass");
    auto *d3d12_texture = (GED3D12Texture *)texture.get();

    checkTextureBindAgainstShader(index, currentRenderPipeline->vertexShader->internal, *d3d12_texture);

    if (d3d12_texture->needsValidation()) {
        auto buffer = std::dynamic_pointer_cast<GED3D12CommandBuffer>(parentQueue->getAvailableBuffer());

        d3d12_texture->updateAndValidateStatus(buffer->commandList.Get());
        buffer->commandList->Close();
        parentQueue->commandQueue->ExecuteCommandLists(1,
                                                       (ID3D12CommandList *const *)buffer->commandList.GetAddressOf());
    }

    auto required_state = getRequiredResourceStateForResourceID(index, currentRenderPipeline->vertexShader->internal);

    if (d3d12_texture->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_texture->resource.Get());
        commandList->ResourceBarrier(1, &barrier);
    }

    if (!(d3d12_texture->currentState & required_state)) {
        if (inRenderPass) {
            reportTransitionInsideRenderPass("texture", d3d12_texture->currentState, required_state);
        } else {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                d3d12_texture->resource.Get(), d3d12_texture->currentState, required_state);
            commandList->ResourceBarrier(1, &barrier);
            d3d12_texture->currentState = required_state;
        }
    }

    // Phase 2 (Shared-Descriptor-Heap-Plan): pick the right slot for
    // the binding the shader expects, then bind the block-specific heap.
    // Phase 2 follow-on (growth) — handles carry their block index so
    // multi-heap allocators resolve to the right underlying heap.
    D3D12DescriptorHandle effHandle{};
    TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, index, currentRenderPipeline->vertexShader->internal);
    if (d3d12_texture->currentState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        effHandle = effective.isIdentity()
            ? d3d12_texture->srvHandle
            : d3d12_texture->getOrCreateSwizzledSrvHandle(parentQueue->engine, effective);
    } else {
        effHandle = d3d12_texture->uavHandle;
    }
    currentResourceDescHeap = parentQueue->engine->resourceDescriptorAllocator->heap(effHandle.block);
    rebindDescriptorHeaps();
    unsigned idx = getRootParameterIndexOfResource(index, currentRenderPipeline->vertexShader->internal);
    commandList->SetGraphicsRootDescriptorTable(idx, effHandle.gpu);
};

void GED3D12CommandBuffer::bindResourceAtVertexShader(SharedHandle<GESamplerState> &sampler, unsigned int id) {
    d3d12RequireOrReturn(!inComputePass && !inBlitPass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtVertexShader(sampler) called outside a render pass");
    auto *d3d12_sampler = (GED3D12SamplerState *)sampler.get();
    bool ok = checkSamplerBindAgainstShader(id, currentRenderPipeline->vertexShader->internal);
    d3d12RequireOrReturn(ok, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtVertexShader(sampler): sampler bound to a static or non-sampler slot");
    if (!ok) return;
    // Phase 2 — samplers live in the engine's shared SAMPLER heap;
    // each GED3D12SamplerState carries its slot's GPU handle and block.
    currentSamplerDescHeap = parentQueue->engine->samplerDescriptorAllocator->heap(d3d12_sampler->samplerHandle.block);
    rebindDescriptorHeaps();
    unsigned rootParam = getRootParameterIndexOfResource(id, currentRenderPipeline->vertexShader->internal);
    commandList->SetGraphicsRootDescriptorTable(rootParam, d3d12_sampler->samplerHandle.gpu);
};

void GED3D12CommandBuffer::bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned int index) {
    d3d12RequireOrReturn(!inComputePass && !inBlitPass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtFragmentShader(buffer) called outside a render pass");
    auto *d3d12_buffer = (GED3D12Buffer *)buffer.get();

    auto required_state = getRequiredResourceStateForResourceID(index, currentRenderPipeline->fragmentShader->internal);

    if (d3d12_buffer->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_buffer->buffer.Get());
        commandList->ResourceBarrier(1, &barrier);
    }

    if (!(d3d12_buffer->currentState & required_state)) {
        if (inRenderPass) {
            reportTransitionInsideRenderPass("buffer", d3d12_buffer->currentState, required_state);
        } else {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                d3d12_buffer->buffer.Get(), d3d12_buffer->currentState, required_state);
            commandList->ResourceBarrier(1, &barrier);
            d3d12_buffer->currentState = required_state;
        }
    }

    // Phase 1 (Shared-Descriptor-Heap-Plan): see bindResourceAtVertexShader
    // for why the per-buffer SetDescriptorHeaps call was removed.

    if (d3d12_buffer->role == BufferDescriptor::Uniform) {
        commandList->SetGraphicsRootConstantBufferView(
            getRootParameterIndexOfResource(index, currentRenderPipeline->fragmentShader->internal),
            d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else if (d3d12_buffer->currentState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        commandList->SetGraphicsRootShaderResourceView(
            getRootParameterIndexOfResource(index, currentRenderPipeline->fragmentShader->internal),
            d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else {
        commandList->SetGraphicsRootUnorderedAccessView(
            getRootParameterIndexOfResource(index, currentRenderPipeline->fragmentShader->internal),
            d3d12_buffer->buffer->GetGPUVirtualAddress());
    }
};

void GED3D12CommandBuffer::bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned int index,
                                                         const TextureSwizzle & swizzle) {
    d3d12RequireOrReturn(!inComputePass && !inBlitPass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtFragmentShader(texture) called outside a render pass");
    auto *d3d12_texture = (GED3D12Texture *)texture.get();

    checkTextureBindAgainstShader(index, currentRenderPipeline->fragmentShader->internal, *d3d12_texture);

    if (d3d12_texture->needsValidation()) {
        auto buffer = std::dynamic_pointer_cast<GED3D12CommandBuffer>(parentQueue->getAvailableBuffer());

        d3d12_texture->updateAndValidateStatus(buffer->commandList.Get());
        buffer->commandList->Close();
        parentQueue->commandQueue->ExecuteCommandLists(1,
                                                       (ID3D12CommandList *const *)buffer->commandList.GetAddressOf());
    }

    auto required_state = getRequiredResourceStateForResourceID(index, currentRenderPipeline->fragmentShader->internal);

    if (d3d12_texture->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_texture->resource.Get());
        commandList->ResourceBarrier(1, &barrier);
    }

    if (!(d3d12_texture->currentState & required_state)) {
        if (inRenderPass) {
            reportTransitionInsideRenderPass("texture", d3d12_texture->currentState, required_state);
        } else {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                d3d12_texture->resource.Get(), d3d12_texture->currentState, required_state);
            commandList->ResourceBarrier(1, &barrier);
            d3d12_texture->currentState = required_state;
        }
    }

    // Phase 2 (Shared-Descriptor-Heap-Plan): see the vertex peer.
    D3D12DescriptorHandle effHandle{};
    TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, index, currentRenderPipeline->fragmentShader->internal);
    if (d3d12_texture->currentState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        effHandle = effective.isIdentity()
            ? d3d12_texture->srvHandle
            : d3d12_texture->getOrCreateSwizzledSrvHandle(parentQueue->engine, effective);
    } else {
        effHandle = d3d12_texture->uavHandle;
    }
    currentResourceDescHeap = parentQueue->engine->resourceDescriptorAllocator->heap(effHandle.block);
    rebindDescriptorHeaps();
    unsigned rootParam = getRootParameterIndexOfResource(index, currentRenderPipeline->fragmentShader->internal);
    // DEBUG_STREAM("Root Param With Texture:" << rootParam);
    commandList->SetGraphicsRootDescriptorTable(rootParam, effHandle.gpu);
};

void GED3D12CommandBuffer::bindResourceAtFragmentShader(SharedHandle<GESamplerState> &sampler, unsigned int id) {
    d3d12RequireOrReturn(!inComputePass && !inBlitPass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtFragmentShader(sampler) called outside a render pass");
    auto *d3d12_sampler = (GED3D12SamplerState *)sampler.get();
    bool ok = checkSamplerBindAgainstShader(id, currentRenderPipeline->fragmentShader->internal);
    d3d12RequireOrReturn(ok, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtFragmentShader(sampler): sampler bound to a static or non-sampler slot");
    if (!ok) return;
    currentSamplerDescHeap = parentQueue->engine->samplerDescriptorAllocator->heap(d3d12_sampler->samplerHandle.block);
    rebindDescriptorHeaps();
    unsigned rootParam = getRootParameterIndexOfResource(id, currentRenderPipeline->fragmentShader->internal);
    commandList->SetGraphicsRootDescriptorTable(rootParam, d3d12_sampler->samplerHandle.gpu);
};

// ── §5 — amplification-stage resource binding ───────────────────────────────
//
// Structurally identical to the vertex/fragment binds above; the only thing
// that changes is WHICH shader's `omegasl_shader` the root-parameter lookup
// reads. D3D12's root signature is unified (D3D12_SHADER_VISIBILITY_ALL) and
// the amplification stage's resources live at register space 2, so
// `getRootParameterIndexOfResource` resolves them without any extra plumbing —
// the register space is what keeps them from aliasing the mesh stage's space-0
// registers.
//
// `amplificationValidForBind` is the shared guard: reaching any of these
// without an amplification stage bound means the caller thinks the pipeline has
// a stage it doesn't, and a resource that silently goes nowhere is worse than a
// loud failure — the amp would read an unwritten root parameter and dispatch a
// garbage child grid.

bool GED3D12CommandBuffer::amplificationValidForBind(const char *what) {
    const bool ok = currentRenderPipeline != nullptr
                    && currentRenderPipeline->isMesh
                    && currentRenderPipeline->amplificationShader != nullptr;
    d3d12RequireOrReturnValue(ok, DEBUG_DOMAIN_RESOURCE,
                              std::string(what) + ": no amplification stage on the bound pipeline "
                                 "(bind a mesh pipeline built with `amplificationFunc`)",
                              false);
    return ok;
}

void GED3D12CommandBuffer::bindResourceAtAmplificationShader(SharedHandle<GEBuffer> &buffer, unsigned int index) {
    d3d12RequireOrReturn(!inComputePass && !inBlitPass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtAmplificationShader(buffer) called outside a render pass");
    if (!amplificationValidForBind("bindResourceAtAmplificationShader(buffer)")) return;
    auto &ampShader = currentRenderPipeline->amplificationShader->internal;
    auto *d3d12_buffer = (GED3D12Buffer *)buffer.get();

    auto required_state = getRequiredResourceStateForResourceID(index, ampShader);

    if (d3d12_buffer->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_buffer->buffer.Get());
        commandList->ResourceBarrier(1, &barrier);
    }

    if (!(d3d12_buffer->currentState & required_state)) {
        if (inRenderPass) {
            reportTransitionInsideRenderPass("buffer", d3d12_buffer->currentState, required_state);
        } else {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                d3d12_buffer->buffer.Get(), d3d12_buffer->currentState, required_state);
            commandList->ResourceBarrier(1, &barrier);
            d3d12_buffer->currentState = required_state;
        }
    }

    const auto rootParam = getRootParameterIndexOfResource(index, ampShader);

    if (d3d12_buffer->role == BufferDescriptor::Uniform) {
        commandList->SetGraphicsRootConstantBufferView(rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else if (d3d12_buffer->currentState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        commandList->SetGraphicsRootShaderResourceView(rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else {
        commandList->SetGraphicsRootUnorderedAccessView(rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
    }
};

void GED3D12CommandBuffer::bindResourceAtAmplificationShader(SharedHandle<GETexture> &texture, unsigned int index,
                                                             const TextureSwizzle & swizzle) {
    d3d12RequireOrReturn(!inComputePass && !inBlitPass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtAmplificationShader(texture) called outside a render pass");
    if (!amplificationValidForBind("bindResourceAtAmplificationShader(texture)")) return;
    auto &ampShader = currentRenderPipeline->amplificationShader->internal;
    auto *d3d12_texture = (GED3D12Texture *)texture.get();

    checkTextureBindAgainstShader(index, ampShader, *d3d12_texture);

    if (d3d12_texture->needsValidation()) {
        auto buffer = std::dynamic_pointer_cast<GED3D12CommandBuffer>(parentQueue->getAvailableBuffer());
        d3d12_texture->updateAndValidateStatus(buffer->commandList.Get());
        buffer->commandList->Close();
        parentQueue->commandQueue->ExecuteCommandLists(1,
                                                       (ID3D12CommandList *const *)buffer->commandList.GetAddressOf());
    }

    auto required_state = getRequiredResourceStateForResourceID(index, ampShader);

    if (d3d12_texture->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_texture->resource.Get());
        commandList->ResourceBarrier(1, &barrier);
    }

    if (!(d3d12_texture->currentState & required_state)) {
        if (inRenderPass) {
            reportTransitionInsideRenderPass("texture", d3d12_texture->currentState, required_state);
        } else {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                d3d12_texture->resource.Get(), d3d12_texture->currentState, required_state);
            commandList->ResourceBarrier(1, &barrier);
            d3d12_texture->currentState = required_state;
        }
    }

    D3D12DescriptorHandle effHandle{};
    TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, index, ampShader);
    if (d3d12_texture->currentState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        effHandle = effective.isIdentity()
            ? d3d12_texture->srvHandle
            : d3d12_texture->getOrCreateSwizzledSrvHandle(parentQueue->engine, effective);
    } else {
        effHandle = d3d12_texture->uavHandle;
    }
    currentResourceDescHeap = parentQueue->engine->resourceDescriptorAllocator->heap(effHandle.block);
    rebindDescriptorHeaps();
    unsigned idx = getRootParameterIndexOfResource(index, ampShader);
    commandList->SetGraphicsRootDescriptorTable(idx, effHandle.gpu);
};

void GED3D12CommandBuffer::bindResourceAtAmplificationShader(SharedHandle<GESamplerState> &sampler, unsigned int id) {
    d3d12RequireOrReturn(!inComputePass && !inBlitPass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtAmplificationShader(sampler) called outside a render pass");
    if (!amplificationValidForBind("bindResourceAtAmplificationShader(sampler)")) return;
    auto &ampShader = currentRenderPipeline->amplificationShader->internal;
    auto *d3d12_sampler = (GED3D12SamplerState *)sampler.get();
    bool ok = checkSamplerBindAgainstShader(id, ampShader);
    d3d12RequireOrReturn(ok, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtAmplificationShader(sampler): sampler bound to a static or non-sampler slot");
    if (!ok) return;
    currentSamplerDescHeap = parentQueue->engine->samplerDescriptorAllocator->heap(d3d12_sampler->samplerHandle.block);
    rebindDescriptorHeaps();
    unsigned rootParam = getRootParameterIndexOfResource(id, ampShader);
    commandList->SetGraphicsRootDescriptorTable(rootParam, d3d12_sampler->samplerHandle.gpu);
};

void GED3D12CommandBuffer::setStencilRef(unsigned int ref) {
    commandList->OMSetStencilRef(ref);
}

// §2.2 push constant — a pipeline binds at most one, so locate the single
// OMEGASL_SHADER_PUSH_CONSTANT_DESC entry and return its OmegaSL `location`
// (the id getRootParameterIndexOfResource keys on). Returns false if the
// shader declares none.
static bool findPushConstantLocation(const omegasl_shader &shader, unsigned &outLocation) {
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout, shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.type == OMEGASL_SHADER_PUSH_CONSTANT_DESC) {
            outLocation = (unsigned)l.location;
            return true;
        }
    }
    return false;
}

void GED3D12CommandBuffer::setRenderConstants(const void *data, unsigned size, unsigned offset) {
    d3d12RequireOrReturn(currentRenderPipeline, DEBUG_DOMAIN_PIPELINE,
                         "setRenderConstants requires a bound render pipeline");
    // Root 32-bit constants are DWORD-granular.
    d3d12RequireOrReturn((size % 4) == 0 && (offset % 4) == 0, DEBUG_DOMAIN_PIPELINE,
                         "setRenderConstants: D3D12 root constants are 32-bit; size and offset must be 4-byte aligned");
    // Each stage that declared the push constant has its own root-constants
    // param (vertex at space0, fragment at space1 from HLSL codegen), so set
    // both with the same bytes — mirrors Metal's setVertexBytes/setFragmentBytes.
    unsigned loc = 0;
    bool any = false;
    if (findPushConstantLocation(currentRenderPipeline->vertexShader->internal, loc)) {
        commandList->SetGraphicsRoot32BitConstants(
            getRootParameterIndexOfResource(loc, currentRenderPipeline->vertexShader->internal),
            size / 4, data, offset / 4);
        any = true;
    }
    if (findPushConstantLocation(currentRenderPipeline->fragmentShader->internal, loc)) {
        commandList->SetGraphicsRoot32BitConstants(
            getRootParameterIndexOfResource(loc, currentRenderPipeline->fragmentShader->internal),
            size / 4, data, offset / 4);
        any = true;
    }
    // §5 — the amplification stage is an ADDITIONAL stage (it occupies neither
    // slot above), so it gets its own test and its own root-constants param
    // (space2). This is what lets one setRenderConstants call feed a
    // `constant<T>` declared `[in pc]` on both the amp and the mesh shader — the
    // normal shape for handing a batch count or an MVP to both halves of a mesh
    // pipeline.
    if (currentRenderPipeline->amplificationShader != nullptr
        && findPushConstantLocation(currentRenderPipeline->amplificationShader->internal, loc)) {
        commandList->SetGraphicsRoot32BitConstants(
            getRootParameterIndexOfResource(loc, currentRenderPipeline->amplificationShader->internal),
            size / 4, data, offset / 4);
        any = true;
    }
    d3d12RequireOrReturn(any, DEBUG_DOMAIN_PIPELINE,
                         "setRenderConstants: bound pipeline declares no `constant<T>` push constant");
    (void)any;
}

void GED3D12CommandBuffer::setViewports(std::vector<GEViewport> viewports) {
    std::vector<D3D12_VIEWPORT> d3d12_viewports;
    auto viewports_it = viewports.begin();
    while (viewports_it != viewports.end()) {
        GEViewport &viewport = *viewports_it;
        // GEViewport is top-left origin — the Phase-7 convention shared with
        // the Metal and Vulkan backends and the emit-side `1 - 2y/h` NDC math
        // — so viewport.y maps straight to D3D12_VIEWPORT.TopLeftY with no
        // Y-flip. The prior `targetHeight - (y + height)` flip was a remnant of
        // WTK's old bottom-left coordinate space: it happened to be a no-op for
        // full-target viewports (y==0, height==targetHeight) but mis-placed any
        // offset viewport — most visibly the content-cache capture pass, whose
        // window-sized viewport is offset onto a smaller texture target, which
        // pushed the captured geometry off the texture entirely on D3D12.
        CD3DX12_VIEWPORT v(viewport.x, viewport.y, viewport.width, viewport.height,
                           viewport.nearDepth, viewport.farDepth);
        d3d12_viewports.push_back(v);
        ++viewports_it;
    };
    commandList->RSSetViewports(d3d12_viewports.size(), d3d12_viewports.data());
};

void GED3D12CommandBuffer::setScissorRects(std::vector<GEScissorRect> scissorRects) {
    std::vector<D3D12_RECT> d3d12_rects;
    auto rects_it = scissorRects.begin();
    while (rects_it != scissorRects.end()) {
        GEScissorRect &_rect = *rects_it;

        // Top-left origin (Phase-7 convention): _rect.y is the top edge, so it
        // maps straight to D3D12_RECT.top with no Y-flip — same reasoning as
        // setViewports above. The prior `targetHeight - (height + y)` flip was
        // the same bottom-left-coordinate-space remnant: harmless for
        // full-target rects, wrong for the offset capture pass.
        CD3DX12_RECT r((LONG)_rect.x, (LONG)_rect.y,
                       LONG(_rect.x + _rect.width), LONG(_rect.y + _rect.height));
        d3d12_rects.push_back(r);
        ++rects_it;
    };
    commandList->RSSetScissorRects(d3d12_rects.size(), d3d12_rects.data());
};

void GED3D12CommandBuffer::setVertexBuffer(SharedHandle<GEBuffer> &buffer) {
    auto *b = (GED3D12Buffer *)buffer.get();
    D3D12_VERTEX_BUFFER_VIEW view;
    view.BufferLocation = b->buffer->GetGPUVirtualAddress();
    view.SizeInBytes = UINT(b->size());
    view.StrideInBytes = 1;
    commandList->IASetVertexBuffers(0, 1, &view);
};

static D3D12_PRIMITIVE_TOPOLOGY d3d12TopologyForPolygonType(GECommandBuffer::PolygonType polygonType) {
    switch (polygonType) {
        case GECommandBuffer::Triangle:
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case GECommandBuffer::TriangleStrip:
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case GECommandBuffer::Line:
            return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case GECommandBuffer::LineStrip:
            return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case GECommandBuffer::Point:
            return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    }
    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

void GED3D12CommandBuffer::drawPolygons(RenderPassDrawPolygonType polygonType, unsigned int vertexCount,
                                        size_t startIdx) {
    d3d12RequireOrReturn(!inComputePass, DEBUG_DOMAIN_QUEUE, "draw call issued while a compute pass is active");
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->DrawInstanced(vertexCount, 1, startIdx, 0);
};

void GED3D12CommandBuffer::drawPatches(unsigned patchCount,
                                       SharedHandle<GEBuffer> & controlPointBuffer,
                                       size_t startPatch) {
    d3d12RequireOrReturn(!inComputePass, DEBUG_DOMAIN_QUEUE, "draw call issued while a compute pass is active");
    /// §16 Phase H — must be inside a tess pass with a tessellation pipeline
    /// bound (mirrors the Vulkan guard).
    if(!tessPassActive || currentRenderPipeline == nullptr || !currentRenderPipeline->isTess){
        DEBUG_CRITICAL(DEBUG_DOMAIN_QUEUE, "drawPatches: no tessellation pipeline bound in a startTessRenderPass scope.");
        assert(false && "drawPatches outside a tessellation pass");
        return;
    }
    /// The `vertex(tess=true)` stage reads its per-control-point input from the
    /// control-point storage buffer (`buffer<CP> : 0` → StructuredBuffer SRV at
    /// t0), indexed by VertexID. Bind it at the vertex stage's control-point
    /// slot (0) so `controlPoints[SV_VertexID]` resolves; the SRV bind is
    /// recorded on the graphics root signature like any other vertex resource.
    bindResourceAtVertexShader(controlPointBuffer, 0);

    /// The PSO topology type is PATCH; the concrete IA topology is an
    /// N-control-point patch list, N = the per-patch control-point count. The
    /// D3D primitive-topology enum values are contiguous — `_1_CONTROL_POINT_`
    /// + (N-1) == `_N_CONTROL_POINT_PATCHLIST` — so offset from the
    /// 1-control-point base. The vertex stage runs once per control point, so
    /// the draw covers `patchCount * N` vertices and the tessellator groups them
    /// into `patchCount` patches.
    const unsigned n = currentRenderPipeline->patchControlPoints
                           ? currentRenderPipeline->patchControlPoints : 1u;
    const D3D12_PRIMITIVE_TOPOLOGY patchTopology =
        (D3D12_PRIMITIVE_TOPOLOGY)(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + (n - 1u));
    commandList->IASetPrimitiveTopology(patchTopology);

    const unsigned vertexCount = patchCount * n;
    const unsigned firstVertex = (unsigned)startPatch * n;
    commandList->DrawInstanced(vertexCount, 1, firstVertex, 0);
    DEBUG_TRACE(DEBUG_DOMAIN_QUEUE, "drawPatches: patchCount=" << patchCount
                << " controlPoints=" << n << " vertexCount=" << vertexCount);
};

void GED3D12CommandBuffer::setIndexBuffer(SharedHandle<GEBuffer> & buffer, RenderPassIndexType indexType) {
    auto *b = (GED3D12Buffer *)buffer.get();
    D3D12_INDEX_BUFFER_VIEW view;
    view.BufferLocation = b->buffer->GetGPUVirtualAddress();
    view.SizeInBytes = UINT(b->size());
    view.Format = (indexType == RenderPassIndexType::UInt16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    commandList->IASetIndexBuffer(&view);
}

void GED3D12CommandBuffer::drawIndexedPolygons(RenderPassDrawPolygonType polygonType,
                                               unsigned indexCount, size_t startIndex,
                                               int baseVertex) {
    d3d12RequireOrReturn(!inComputePass, DEBUG_DOMAIN_QUEUE, "draw call issued while a compute pass is active");
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->DrawIndexedInstanced(indexCount, 1, UINT(startIndex), baseVertex, 0);
}

void GED3D12CommandBuffer::drawPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                                 unsigned vertexCount, size_t startIdx,
                                                 unsigned instanceCount, unsigned firstInstance) {
    d3d12RequireOrReturn(!inComputePass, DEBUG_DOMAIN_QUEUE, "draw call issued while a compute pass is active");
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->DrawInstanced(vertexCount, instanceCount, UINT(startIdx), firstInstance);
}

void GED3D12CommandBuffer::drawIndexedPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                                        unsigned indexCount, size_t startIndex,
                                                        int baseVertex, unsigned instanceCount,
                                                        unsigned firstInstance) {
    d3d12RequireOrReturn(!inComputePass, DEBUG_DOMAIN_QUEUE, "draw call issued while a compute pass is active");
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->DrawIndexedInstanced(indexCount, instanceCount, UINT(startIndex), baseVertex, firstInstance);
}

static void transitionBufferForIndirectArgs(ID3D12GraphicsCommandList6 *commandList, GED3D12Buffer *argBuf) {
    if (argBuf->currentState == D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) {
        return;
    }
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(argBuf->buffer.Get(),
                                                        argBuf->currentState,
                                                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    commandList->ResourceBarrier(1, &barrier);
    argBuf->currentState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
}

void GED3D12CommandBuffer::drawPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                                SharedHandle<GEBuffer> & argumentBuffer,
                                                size_t argumentBufferOffset) {
    d3d12RequireOrReturn(!inComputePass, DEBUG_DOMAIN_QUEUE, "draw call issued while a compute pass is active");
    auto *argBuf = (GED3D12Buffer *)argumentBuffer.get();
    auto *sig = parentQueue->engine->getDrawIndirectSignature();
    if (sig == nullptr) {
        DEBUG_ERROR(DEBUG_DOMAIN_QUEUE, "drawPolygonsIndirect: draw indirect signature unavailable");
        return;
    }
    transitionBufferForIndirectArgs(commandList.Get(), argBuf);
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->ExecuteIndirect(sig, 1, argBuf->buffer.Get(),
                                 UINT64(argumentBufferOffset),
                                 nullptr, 0);
}

void GED3D12CommandBuffer::drawIndexedPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                                       SharedHandle<GEBuffer> & argumentBuffer,
                                                       size_t argumentBufferOffset) {
    d3d12RequireOrReturn(!inComputePass, DEBUG_DOMAIN_QUEUE, "draw call issued while a compute pass is active");
    auto *argBuf = (GED3D12Buffer *)argumentBuffer.get();
    auto *sig = parentQueue->engine->getDrawIndexedIndirectSignature();
    if (sig == nullptr) {
        DEBUG_ERROR(DEBUG_DOMAIN_QUEUE, "drawIndexedPolygonsIndirect: indexed draw indirect signature unavailable");
        return;
    }
    transitionBufferForIndirectArgs(commandList.Get(), argBuf);
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->ExecuteIndirect(sig, 1, argBuf->buffer.Get(),
                                 UINT64(argumentBufferOffset),
                                 nullptr, 0);
}

void GED3D12CommandBuffer::finishRenderPass() {
    d3d12RequireOrReturn(inRenderPass, DEBUG_DOMAIN_RENDERTGT, "finishRenderPass called with no active render pass");
    DEBUG_TRACE(DEBUG_DOMAIN_RENDERTGT, "RenderPass end");
    commandList->EndRenderPass();
    commandList->ClearState(nullptr);

    if (multisampleResolvePass) {
        ID3D12Resource *target;
        if (currentTarget.native != nullptr) {
            target = currentTarget.native->renderTargets[currentTarget.native->frameIndex];
        } else {
            target = currentTarget.texture->texture->resource.Get();
        }
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(target, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                                            D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList->ResourceBarrier(1, &barrier);
        // The resolve barrier above leaves the resource in RENDER_TARGET but
        // did not update the tracked state; record it so the sampleable
        // transition below (texture) or present() (native) uses the correct
        // StateBefore.
        if (currentTarget.texture != nullptr && currentTarget.texture->texture != nullptr) {
            currentTarget.texture->texture->currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        } else if (currentTarget.native != nullptr) {
            const auto frameIdx = currentTarget.native->frameIndex;
            if (frameIdx < currentTarget.native->renderTargetStates.size()) {
                currentTarget.native->renderTargetStates[frameIdx] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
        }
    }

    // D3D12 parity with the Vulkan backend: GEVulkanCommandBuffer::startRenderPass
    // sets a texture render target's render-pass finalLayout to
    // SHADER_READ_ONLY_OPTIMAL, so the pass itself leaves the texture sampleable.
    // D3D12 render passes do not auto-transition resource state, so without this
    // a texture render target stays in RENDER_TARGET and the next pass's bind
    // path — which cannot emit a barrier between Begin/EndRenderPass — hits
    // reportTransitionInsideRenderPass and asserts. A texture render target
    // exists to be sampled by a later pass (the compositor's content-cache blit
    // and blur composite), so move it to the shader-resource state now that
    // EndRenderPass has run and we are outside the pass scope. A re-render goes
    // back through startRenderPass, which transitions it to RENDER_TARGET again.
    // The native swapchain target (currentTarget.native) is excluded: it reaches
    // PRESENT via present().
    if (currentTarget.texture != nullptr && currentTarget.texture->texture != nullptr) {
        auto *targetTexture = currentTarget.texture->texture.get();
        const D3D12_RESOURCE_STATES sampleState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (targetTexture->currentState != sampleState) {
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                targetTexture->resource.Get(), targetTexture->currentState, sampleState);
            commandList->ResourceBarrier(1, &barrier);
            targetTexture->currentState = sampleState;
        }
    }

    // Per-pass flag: clear it so a pooled command buffer that previously ran an
    // MSAA-resolve pass does not misclassify a later non-resolve pass.
    multisampleResolvePass = false;
    tessPassActive = false;  // §16 Phase H — close any tess pass scope.
    currentTarget.texture = nullptr;
    currentTarget.native = nullptr;
    currentRenderPipeline = nullptr;
    currentRootSignature = nullptr;
    currentResourceDescHeap = nullptr;
    currentSamplerDescHeap = nullptr;
    // The command list's bound heaps are cleared here too (ClearState / list
    // reset), so the next pass's first rebindDescriptorHeaps re-establishes them.
    boundResourceDescHeap = nullptr;
    boundSamplerDescHeap = nullptr;
};

void GED3D12CommandBuffer::startComputePass(const GEComputePassDescriptor &desc) {
    inComputePass = true;
    DEBUG_TRACE(DEBUG_DOMAIN_QUEUE, "ComputePass begin");
};

void GED3D12CommandBuffer::setComputePipelineState(SharedHandle<GEComputePipelineState> &pipelineState) {
    d3d12RequireOrReturn(inComputePass, DEBUG_DOMAIN_PIPELINE,
                         "setComputePipelineState called outside an active compute pass");
    DEBUG_TRACE(DEBUG_DOMAIN_PIPELINE, "Compute PSO set");
    auto *d3d12_pipeline_state = (GED3D12ComputePipelineState *)pipelineState.get();
    commandList->SetPipelineState(d3d12_pipeline_state->pipelineState.Get());
    commandList->SetComputeRootSignature(d3d12_pipeline_state->rootSignature.Get());
    currentComputePipeline = d3d12_pipeline_state;
    currentRootSignature = &d3d12_pipeline_state->rootSignatureDesc;
};

void GED3D12CommandBuffer::bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned int id) {
    d3d12RequireOrReturn(inComputePass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtComputeShader(buffer) called outside a compute pass");
    auto *d3d12_buffer = (GED3D12Buffer *)buffer.get();
    auto &shader = currentComputePipeline->computeShader->internal;
    const unsigned rootParam = getRootParameterIndexOfResource(id, shader);
    // Universal buffers: land any pending CPU write in the primary before the
    // dispatch reads it. The SRV/UAV transition below then orders the copy
    // ahead of the dispatch.
    flushPendingUpload(d3d12_buffer);
    // D3D12-CPU-Accessible-Buffer-Plan Phase 1 — classify the bind from the
    // shader layout (in→SRV, out→UAV, Uniform→CBV), NOT the buffer's heap type.
    // The old heap-type heuristic bound a Storage `out` buffer that lived on an
    // UPLOAD heap as an SRV, contradicting the root signature's UAV parameter
    // (D3D12 [ERROR id=711]). Phase 1 (Shared-Descriptor-Heap-Plan): root-view
    // binds consult the GPU virtual address, not any descriptor heap.
    const BufferRootKind kind = (d3d12_buffer->role == BufferDescriptor::Uniform)
                                    ? BufferRootKind::CBV
                                    : classifyBufferRootKind(id, shader);
    switch (kind) {
        case BufferRootKind::CBV:
            // §2.4 constant buffer — root CBV.
            commandList->SetComputeRootConstantBufferView(
                rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
            break;
        case BufferRootKind::SRV:
            transitionBufferState(d3d12_buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            commandList->SetComputeRootShaderResourceView(
                rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
            break;
        case BufferRootKind::UAV:
            transitionBufferState(d3d12_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            uavBoundInComputePass = true;
            commandList->SetComputeRootUnorderedAccessView(
                rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
            // D3D12-CPU-Accessible-Buffer-Plan Phase 2 — a Readback output (has a
            // READBACK companion) needs its DEFAULT primary copied into the
            // companion at finishComputePass so the CPU can read the results.
            // Track it once per pass.
            if (d3d12_buffer->cpuSideResource) {
                bool already = false;
                for (auto *b : pendingReadbackBuffers) {
                    if (b == d3d12_buffer) { already = true; break; }
                }
                if (!already) {
                    pendingReadbackBuffers.push_back(d3d12_buffer);
                }
            }
            break;
    }
}

void GED3D12CommandBuffer::bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int id,
                                                        const TextureSwizzle & swizzle) {
    d3d12RequireOrReturn(inComputePass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtComputeShader(texture) called outside a compute pass");
    auto *d3d12_texture = (GED3D12Texture *)texture.get();

    checkTextureBindAgainstShader(id, currentComputePipeline->computeShader->internal, *d3d12_texture);

    if (d3d12_texture->needsValidation()) {
        d3d12_texture->updateAndValidateStatus(commandList.Get());
    }

    D3D12_HEAP_PROPERTIES heap_props;
    D3D12_HEAP_FLAGS heapFlags;
    d3d12_texture->resource->GetHeapProperties(&heap_props, &heapFlags);
    // Phase 2 (Shared-Descriptor-Heap-Plan): SRV-only compute bind path.
    TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, id, currentComputePipeline->computeShader->internal);
    D3D12DescriptorHandle h = effective.isIdentity()
        ? d3d12_texture->srvHandle
        : d3d12_texture->getOrCreateSwizzledSrvHandle(parentQueue->engine, effective);
    currentResourceDescHeap = parentQueue->engine->resourceDescriptorAllocator->heap(h.block);
    rebindDescriptorHeaps();
    if (heap_props.Type == D3D12_HEAP_TYPE_READBACK) {
        auto resource_barrier = CD3DX12_RESOURCE_BARRIER::Transition(d3d12_texture->resource.Get(),
                                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &resource_barrier);
    }
    commandList->SetComputeRootDescriptorTable(
        getRootParameterIndexOfResource(id, currentComputePipeline->computeShader->internal),
        h.gpu);
}

void GED3D12CommandBuffer::bindResourceAtComputeShader(SharedHandle<GESamplerState> &sampler, unsigned int id) {
    d3d12RequireOrReturn(inComputePass, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtComputeShader(sampler) called outside a compute pass");
    auto *d3d12_sampler = (GED3D12SamplerState *)sampler.get();
    bool ok = checkSamplerBindAgainstShader(id, currentComputePipeline->computeShader->internal);
    d3d12RequireOrReturn(ok, DEBUG_DOMAIN_RESOURCE,
                         "bindResourceAtComputeShader(sampler): sampler bound to a static or non-sampler slot");
    currentSamplerDescHeap = parentQueue->engine->samplerDescriptorAllocator->heap(d3d12_sampler->samplerHandle.block);
    rebindDescriptorHeaps();
    commandList->SetComputeRootDescriptorTable(
        getRootParameterIndexOfResource(id, currentComputePipeline->computeShader->internal),
        d3d12_sampler->samplerHandle.gpu);
}

void GED3D12CommandBuffer::bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> &accelStruct,
                                                       unsigned int idx) {
    auto d3d12_buffer = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(accelStruct);
    commandList->SetComputeRootShaderResourceView(
        getRootParameterIndexOfResource(idx, currentComputePipeline->computeShader->internal),
        d3d12_buffer->structBuffer->buffer->GetGPUVirtualAddress());
}

void GED3D12CommandBuffer::setComputeConstants(const void *data, unsigned size, unsigned offset) {
    d3d12RequireOrReturn(currentComputePipeline, DEBUG_DOMAIN_PIPELINE,
                         "setComputeConstants requires a bound compute pipeline");
    d3d12RequireOrReturn((size % 4) == 0 && (offset % 4) == 0, DEBUG_DOMAIN_PIPELINE,
                         "setComputeConstants: D3D12 root constants are 32-bit; size and offset must be 4-byte aligned");
    unsigned loc = 0;
    bool found = findPushConstantLocation(currentComputePipeline->computeShader->internal, loc);
    d3d12RequireOrReturn(found, DEBUG_DOMAIN_PIPELINE,
                         "setComputeConstants: bound pipeline declares no `constant<T>` push constant");
    commandList->SetComputeRoot32BitConstants(
        getRootParameterIndexOfResource(loc, currentComputePipeline->computeShader->internal),
        size / 4, data, offset / 4);
}

void GED3D12CommandBuffer::drawMeshTasks(uint32_t groupCountX,
                                         uint32_t groupCountY,
                                         uint32_t groupCountZ) {
    /// Mesh-Shader-Plan Phase 4b.3 — live `DispatchMesh`. The feature
    /// gate still fires up front (defensive — the matching gate at
    /// `makeMeshPipelineState` should already have prevented an
    /// unsupported device from producing a bindable mesh PSO). The PSO
    /// itself is bound via the existing `setRenderPipelineState`
    /// because mesh PSOs surface as `GERenderPipelineState`; the
    /// `isMesh` flag stamped at construction (Phase 4b.1) is what tells
    /// us the bound pipeline can dispatch mesh tasks rather than
    /// polygon draws. `commandList` is already `ID3D12GraphicsCommandList6`
    /// so `DispatchMesh` is in scope without a header bump.
    if(!parentQueue->engine->gteDevice->features.hasFeature(GTEDEVICE_FEATURE_MESH_SHADER)){
        DEBUG_CRITICAL(DEBUG_DOMAIN_PIPELINE, "drawMeshTasks: device does not advertise "
                     "GTEDEVICE_FEATURE_MESH_SHADER");
        return;
    }
    d3d12RequireOrReturn(inRenderPass, DEBUG_DOMAIN_QUEUE,
                         "drawMeshTasks: must be called inside a render pass");
    d3d12RequireOrReturn(currentRenderPipeline != nullptr, DEBUG_DOMAIN_QUEUE,
                         "drawMeshTasks: no pipeline bound (call setRenderPipelineState first)");
    d3d12RequireOrReturn(currentRenderPipeline->isMesh, DEBUG_DOMAIN_PIPELINE,
                         "drawMeshTasks: bound pipeline is a graphics pipeline, not a mesh pipeline. "
                         "Use makeMeshPipelineState to build a mesh-variant PSO.");
    commandList->DispatchMesh(groupCountX, groupCountY, groupCountZ);
}

void GED3D12CommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z) {
    d3d12RequireOrReturn(inComputePass, DEBUG_DOMAIN_QUEUE, "dispatchRays called outside a compute pass");
    D3D12_DISPATCH_RAYS_DESC rays{};
    rays.Width = x;
    rays.Height = y;
    rays.Depth = z;

    rays.RayGenerationShaderRecord.StartAddress = 0;
    rays.RayGenerationShaderRecord.SizeInBytes = 0;
    rays.MissShaderTable.StartAddress = 0;
    rays.MissShaderTable.SizeInBytes = 0;
    rays.MissShaderTable.StrideInBytes = 0;
    rays.HitGroupTable.StartAddress = 0;
    rays.HitGroupTable.SizeInBytes = 0;
    rays.HitGroupTable.StrideInBytes = 0;
    rays.CallableShaderTable.StartAddress = 0;
    rays.CallableShaderTable.SizeInBytes = 0;
    rays.CallableShaderTable.StrideInBytes = 0;

    commandList->DispatchRays(&rays);
}

void GED3D12CommandBuffer::dispatchThreadgroups(unsigned int x, unsigned int y, unsigned int z) {
    d3d12RequireOrReturn(inComputePass, DEBUG_DOMAIN_QUEUE, "dispatchThreadgroups called outside a compute pass");
    commandList->Dispatch(x, y, z);
}

void GED3D12CommandBuffer::dispatchThreads(unsigned int x, unsigned int y, unsigned int z) {
    d3d12RequireOrReturn(inComputePass, DEBUG_DOMAIN_QUEUE, "dispatchThreads called outside a compute pass");
    auto &tg = currentComputePipeline->computeShader->internal.threadgroupDesc;
    unsigned gx = (x + tg.x - 1) / tg.x;
    unsigned gy = (y + tg.y - 1) / tg.y;
    unsigned gz = (z + tg.z - 1) / tg.z;
    commandList->Dispatch(gx, gy, gz);
}

void GED3D12CommandBuffer::dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                                        size_t argumentBufferOffset) {
    d3d12RequireOrReturn(inComputePass, DEBUG_DOMAIN_QUEUE, "dispatchThreadgroupsIndirect called outside a compute pass");
    auto *argBuf = (GED3D12Buffer *)argumentBuffer.get();
    auto *sig = parentQueue->engine->getDispatchIndirectSignature();
    if (sig == nullptr) {
        DEBUG_ERROR(DEBUG_DOMAIN_QUEUE, "dispatchThreadgroupsIndirect: dispatch indirect signature unavailable");
        return;
    }
    transitionBufferForIndirectArgs(commandList.Get(), argBuf);
    commandList->ExecuteIndirect(sig, 1, argBuf->buffer.Get(),
                                 UINT64(argumentBufferOffset),
                                 nullptr, 0);
}

void GED3D12CommandBuffer::finishComputePass() {
    // Order this pass's UAV writes against whatever the NEXT pass does with the
    // same buffers. Per-resource transition barriers cover the SRV<->UAV flips,
    // but a buffer bound as a UAV in two consecutive passes keeps its state, so
    // no transition fires — a global UAV barrier is what serializes those. A
    // null UAV barrier orders ALL UAV accesses on the queue, which is exactly
    // the sequential compute-pass contract callers encode against.
    if (uavBoundInComputePass) {
        auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        commandList->ResourceBarrier(1, &uavBarrier);
        uavBoundInComputePass = false;
    }

    // D3D12-CPU-Accessible-Buffer-Plan Phase 2 — copy each Readback output's
    // DEFAULT primary into its READBACK companion so GEBufferReader (which maps
    // the companion) observes the dispatch's UAV writes. Recorded after the
    // dispatch(es) and outside any render pass; the UNORDERED_ACCESS→COPY_SOURCE
    // transition also flushes the UAV writes before the copy reads them. The
    // companion is COPY_DEST (its creation state), so no companion barrier is
    // needed. CopyBufferRegion is a flat buffer→buffer copy — no footprint
    // (unlike the texture readback path).
    for (auto *buf : pendingReadbackBuffers) {
        if (buf == nullptr || buf->cpuSideResource == nullptr) {
            continue;
        }
        transitionBufferState(buf, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->CopyBufferRegion(buf->cpuSideResource.Get(), 0,
                                      buf->buffer.Get(), 0,
                                      buf->buffer->GetDesc().Width);
    }
    pendingReadbackBuffers.clear();

    commandList->ClearState(nullptr);
    inComputePass = false;
    DEBUG_TRACE(DEBUG_DOMAIN_QUEUE, "ComputePass end");
    currentComputePipeline = nullptr;
    currentRootSignature = nullptr;
    currentResourceDescHeap = nullptr;
    currentSamplerDescHeap = nullptr;
    // The command list's bound heaps are cleared here too (ClearState / list
    // reset), so the next pass's first rebindDescriptorHeaps re-establishes them.
    boundResourceDescHeap = nullptr;
    boundSamplerDescHeap = nullptr;
};

//    void GED3D12CommandBuffer::waitForFence(SharedHandle<GEFence> &fence,unsigned val) {
////        auto _fence = (GED3D12Fence *)fence.get();
////
////        parentQueue->commandQueue->Wait(_fence->fence.Get(),val);
//
//    }
//
//    void GED3D12CommandBuffer::signalFence(SharedHandle<GEFence> &fence,unsigned val) {
////        auto _fence = (GED3D12Fence *)fence.get();
////        parentQueue->commandQueue->Signal(_fence->fence.Get(),val);
//    }

GED3D12CommandBuffer::~GED3D12CommandBuffer() {
    ResourceTracking::Tracker::instance().emit(ResourceTracking::EventType::Destroy, ResourceTracking::Backend::D3D12,
                                               "CommandBuffer", traceResourceId, commandList.Get());
}

void GED3D12CommandBuffer::setCompletionHandler(const GECommandBufferCompletionHandler & handler) {
    completionHandler = handler;
}

void GED3D12CommandQueue::stageCompletionHandlerFrom(GED3D12CommandBuffer *cb) {
    // G.5.1 D3D12 follow-up. Move the handler out of the command buffer so the
    // wrapper can be recycled/destroyed without taking the callback with it;
    // the queue now owns firing it. No-op for the common (non-frame) CB that
    // never had a handler registered.
    if (cb != nullptr && cb->completionHandler) {
        // GPU Commit-Timing P1 — carry the buffer's pool slot so pollCompletions
        // can read its resolved timestamps when this handler fires.
        PendingCompletion pending;
        pending.handler  = std::move(cb->completionHandler);
        pending.poolSlot = cb->poolSlot;
        {
            std::lock_guard<std::mutex> lock(completionMutex_);
            stagedCompletionHandlers_.push_back(std::move(pending));
        }
        cb->completionHandler = nullptr;
    }
}

void GED3D12CommandQueue::gateStagedCompletions(std::uint64_t signalValue) {
    // Bind every staged handler to the retentionFence value just signaled for
    // the ExecuteCommandLists that ran its command list. Once
    // GetCompletedValue() reaches `signalValue` the GPU is done with that
    // submission and pollCompletions can fire the handler.
    std::lock_guard<std::mutex> lock(completionMutex_);
    for (auto & pending : stagedCompletionHandlers_) {
        gatedCompletionHandlers_.emplace_back(signalValue, std::move(pending));
    }
    stagedCompletionHandlers_.clear();
}

void GED3D12CommandQueue::pollCompletions() {
    // Collect the ready handlers and compact the survivors in one stable pass
    // *under the lock*, then fire outside it. Holding the lock only for the
    // container mutation keeps the frame path cheap and — crucially — lets a
    // fired handler call back into the queue without self-deadlocking on the
    // non-recursive mutex (the waiter thread and the frame thread can both
    // reach here once an async commit has armed the waiter).
    std::vector<PendingCompletion> ready;
    bool deviceRemoved = false;
    {
        std::lock_guard<std::mutex> lock(completionMutex_);
        if (gatedCompletionHandlers_.empty()) {
            return;
        }
        const std::uint64_t completed = retentionFence ? retentionFence->GetCompletedValue() : 0;
        std::size_t writeIdx = 0;
        for (std::size_t readIdx = 0; readIdx < gatedCompletionHandlers_.size(); ++readIdx) {
            auto & entry = gatedCompletionHandlers_[readIdx];
            if (entry.first <= completed) {
                if (entry.second.handler) {
                    ready.push_back(std::move(entry.second));
                }
            } else {
                if (writeIdx != readIdx) {
                    gatedCompletionHandlers_[writeIdx] = std::move(gatedCompletionHandlers_[readIdx]);
                }
                ++writeIdx;
            }
        }
        gatedCompletionHandlers_.resize(writeIdx);
        if (ready.empty()) {
            return;
        }
        // D3D12 has no per-command-buffer success/fail status the way Metal
        // does; a failed GPU execution surfaces as device removal. Report Error
        // to the handlers fired this pass if the device was lost, else Completed.
        deviceRemoved = engine != nullptr && engine->d3d12_device != nullptr
                        && engine->d3d12_device->GetDeviceRemovedReason() != S_OK;
    }
    const auto status = deviceRemoved ? GECommandBufferCompletionInfo::CompletionStatus::Error
                                      : GECommandBufferCompletionInfo::CompletionStatus::Completed;
    for (auto & entry : ready) {
        // GPU Commit-Timing P1 — fold in this buffer's own resolved GPU span.
        // resolveSlotTiming leaves the fields at 0.0 (the degraded contract)
        // when timing is disabled, the slot was untracked, or its retention
        // fence retired but the buffer recorded no usable timestamp pair.
        GECommandBufferCompletionInfo info {};
        info.status = status;
        if (status == GECommandBufferCompletionInfo::CompletionStatus::Completed) {
            resolveSlotTiming(entry.poolSlot, info.gpuStartTimeSec, info.gpuEndTimeSec);
        }
        entry.handler(info);
    }
}

void GED3D12CommandQueue::notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer,
                                              SharedHandle<GEFence> &waitFence) {
    multiQueueSync = true;
    auto fence = (GED3D12Fence *)waitFence.get();
    if (fence->lastSignaledValue > 0) {
        commandQueue->Wait(fence->fence.Get(), fence->lastSignaledValue);
    }
    multiQueueSync = false;
};

void GED3D12CommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer) {
    HRESULT hr;
    auto d3d12_buffer = (GED3D12CommandBuffer *)commandBuffer.get();
    d3d12_buffer->closed = true;
    submittedTraceCommandBufferIds.push_back(d3d12_buffer->traceResourceId);
    ResourceTracking::Event submitEvent{};
    submitEvent.backend = ResourceTracking::Backend::D3D12;
    submitEvent.eventType = ResourceTracking::EventType::Submit;
    submitEvent.resourceType = "CommandBuffer";
    submitEvent.resourceId = d3d12_buffer->traceResourceId;
    submitEvent.queueId = traceResourceId;
    submitEvent.commandBufferId = d3d12_buffer->traceResourceId;
    submitEvent.nativeHandle = reinterpret_cast<std::uint64_t>(d3d12_buffer->commandList.Get());
    ResourceTracking::Tracker::instance().emit(submitEvent);
    DEBUG_TRACE(DEBUG_DOMAIN_QUEUE,
        "CB submit: queue=" << traceResourceId << " cb=" << d3d12_buffer->traceResourceId);
    retainedCommandBuffers.push_back(commandBuffer);
    commandLists.push_back(d3d12_buffer->commandList.Get());
    // G.5.1 D3D12 follow-up — stage this buffer's completion handler (if WTK
    // registered one). Its list is now queued in `commandLists`; the handler
    // is gated to a fence value when that batch is flushed via
    // ExecuteCommandLists (commitToGPU, or a two-arg submit's flush).
    stageCompletionHandlerFrom(d3d12_buffer);
    // CommandQueue-Typed-Pool Phase 3 — mark the pool slot busy at
    // submit time with a UINT64_MAX sentinel so the recycler in
    // getAvailableBuffer() won't hand the same slot back before
    // commitToGPU / Signal fires. The Signal+stamp pair re-stamps with
    // the actual retentionFence value so the GPU's GetCompletedValue()
    // eventually overtakes the slot.
    if (d3d12_buffer->poolSlot != UINT32_MAX
        && d3d12_buffer->poolSlot < poolSubmissionIndex.size()) {
        poolSubmissionIndex[d3d12_buffer->poolSlot] = UINT64_MAX;
    }
    pendingSlots.push_back(d3d12_buffer->poolSlot);
};

void GED3D12CommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer,
                                              SharedHandle<GEFence> &signalFence) {
    multiQueueSync = true;
    auto *d3d12_buffer = (GED3D12CommandBuffer *)commandBuffer.get();
    auto *fence = dynamic_cast<GED3D12Fence *>(signalFence.get());
    d3d12_buffer->closed = true;
    submittedTraceCommandBufferIds.push_back(d3d12_buffer->traceResourceId);
    ResourceTracking::Event submitEvent{};
    submitEvent.backend = ResourceTracking::Backend::D3D12;
    submitEvent.eventType = ResourceTracking::EventType::Submit;
    submitEvent.resourceType = "CommandBuffer";
    submitEvent.resourceId = d3d12_buffer->traceResourceId;
    submitEvent.queueId = traceResourceId;
    submitEvent.commandBufferId = d3d12_buffer->traceResourceId;
    submitEvent.nativeHandle = reinterpret_cast<std::uint64_t>(d3d12_buffer->commandList.Get());
    ResourceTracking::Tracker::instance().emit(submitEvent);
    DEBUG_TRACE(DEBUG_DOMAIN_QUEUE,
        "CB submit: queue=" << traceResourceId << " cb=" << d3d12_buffer->traceResourceId);

    // Preserve submission order: queued command lists must execute before the
    // fence signal command list so cross-queue waits observe rendered data.
    if (!commandLists.empty()) {
        for (std::size_t i = 0; i < commandLists.size(); ++i) {
            auto *cl = commandLists[i];
            if (cl != nullptr) {
                // GPU Commit-Timing P1 — end timestamp + resolve before Close;
                // `pendingSlots` is parallel to `commandLists`.
                const std::uint32_t slot = i < pendingSlots.size()
                                               ? pendingSlots[i] : UINT32_MAX;
                writeEndTimestampAndResolve(cl, slot);
                cl->Close();
            }
        }
        commandQueue->ExecuteCommandLists(commandLists.size(), (ID3D12CommandList *const *)commandLists.data());
        commandLists.clear();
        const auto pendingGate = gateForNextSubmit();
        ++nextSubmitValue;
        commandQueue->Signal(retentionFence.Get(), nextSubmitValue);
        // CommandQueue-Typed-Pool Phase 3 — stamp the slots of every list
        // we just submitted. The single-buffer Execute below stamps its
        // own slot separately.
        stampPendingSlots(nextSubmitValue);
        // G.5.1 D3D12 follow-up — the lists just flushed include any earlier
        // single-arg-submitted buffers (e.g. the frame CB) whose handlers were
        // staged; gate them to this signal value.
        gateStagedCompletions(nextSubmitValue);
        flushPendingRetentionUnder(pendingGate);
    }

    // G.5.1 D3D12 follow-up — stage this buffer's own handler before its
    // dedicated Execute so it gates to the signal value below, not the flush
    // above. (WTK only sets handlers on the frame CB, so this is normally a
    // no-op for the two-arg / scratch path, but keep the contract general.)
    stageCompletionHandlerFrom(d3d12_buffer);
    // GPU Commit-Timing P1 — end timestamp + resolve for this buffer's own
    // dedicated Execute before its Close().
    writeEndTimestampAndResolve(d3d12_buffer->commandList.Get(), d3d12_buffer->poolSlot);
    d3d12_buffer->commandList->Close();
    commandQueue->ExecuteCommandLists(1, (ID3D12CommandList *const *)d3d12_buffer->commandList.GetAddressOf());
    {
        const auto bufGate = gateForNextSubmit();
        ++nextSubmitValue;
        commandQueue->Signal(retentionFence.Get(), nextSubmitValue);
        // CommandQueue-Typed-Pool Phase 3 — stamp the single buffer's
        // own pool slot now that retentionFence has been signaled at
        // nextSubmitValue. UINT32_MAX is safe — the stamp helper skips
        // it.
        if (d3d12_buffer->poolSlot != UINT32_MAX
            && d3d12_buffer->poolSlot < poolSubmissionIndex.size()) {
            poolSubmissionIndex[d3d12_buffer->poolSlot] = nextSubmitValue;
        }
        gateStagedCompletions(nextSubmitValue);
        engine->retentionQueue.retainShared(SharedHandle<GECommandBuffer>(commandBuffer), {bufGate});
    }
    const auto signalValue = fence->nextSignalValue++;
    fence->lastSignaledValue = signalValue;
    commandQueue->Signal(fence->fence.Get(), signalValue);
    multiQueueSync = false;
    engine->retentionQueue.drainCompleted();
    // Phase 3 — the same fence advance that retires retention queue
    // entries also retires transient-ring slots stamped on prior submits.
    if (transientRing) transientRing->retire();
    // G.5.1 D3D12 follow-up — fire any handlers the GPU has already retired.
    pollCompletions();
}

void GED3D12CommandQueue::signalFence(SharedHandle<GEFence> &fence) {
    auto d3d12Fence = static_cast<GED3D12Fence *>(fence.get());
    const auto signalValue = d3d12Fence->nextSignalValue++;
    d3d12Fence->lastSignaledValue = signalValue;
    commandQueue->Signal(d3d12Fence->fence.Get(), signalValue);
}

void GED3D12CommandQueue::waitForFence(SharedHandle<GEFence> &fence, std::uint64_t value) {
    if (value == 0)
        return;
    auto d3d12Fence = static_cast<GED3D12Fence *>(fence.get());
    const UINT64 completed = d3d12Fence->fence->GetCompletedValue();
    if (completed >= value)
        return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr)
        return;
    HRESULT hr = d3d12Fence->fence->SetEventOnCompletion(value, ev);
    if (SUCCEEDED(hr))
        WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
}

void GED3D12CommandBuffer::reset() {
    closed = false;
    currentResourceDescHeap = nullptr;
    currentSamplerDescHeap = nullptr;
    // The command list's bound heaps are cleared here too (ClearState / list
    // reset), so the next pass's first rebindDescriptorHeaps re-establishes them.
    boundResourceDescHeap = nullptr;
    boundSamplerDescHeap = nullptr;
    commandList->Reset(commandAllocator.Get(), nullptr);
    commandAllocator->Reset();
    // GPU Commit-Timing P1 — re-recorded buffer: re-arm the start timestamp as
    // the first command on the reset list (mirrors getAvailableBuffer).
    parentQueue->writeStartTimestamp(commandList.Get(), poolSlot);
};

void GED3D12CommandQueue::commitToGPU() {
    if (!multiQueueSync) {
        for (std::size_t i = 0; i < commandLists.size(); ++i) {
            auto *cl = commandLists[i];
            if (cl != nullptr) {
                // GPU Commit-Timing P1 — close out this buffer's GPU span just
                // before Close(). `pendingSlots` is parallel to `commandLists`.
                const std::uint32_t slot = i < pendingSlots.size()
                                               ? pendingSlots[i] : UINT32_MAX;
                writeEndTimestampAndResolve(cl, slot);
                cl->Close();
            }
        }
        if (!commandLists.empty()) {
            commandQueue->ExecuteCommandLists(commandLists.size(), (ID3D12CommandList *const *)commandLists.data());
            const auto gate = gateForNextSubmit();
            ++nextSubmitValue;
            commandQueue->Signal(retentionFence.Get(), nextSubmitValue);
            // CommandQueue-Typed-Pool Phase 3 — stamp every pool slot
            // that contributed a list to this Execute. After this point
            // getAvailableBuffer() can recycle any slot whose
            // poolSubmissionIndex <= retentionFence->GetCompletedValue().
            stampPendingSlots(nextSubmitValue);
            commandLists.clear();
            flushPendingRetentionUnder(gate);
        } else {
            // Nothing to execute; any items pushed during this batch (e.g.
            // descriptor heaps from a generateMipmaps that was followed by no
            // submit) have no GPU work to gate against. Clearing them here
            // matches the historical behavior — the GPU was never going to
            // touch them anyway.
            retainedCommandBuffers.clear();
            retainedDescriptorHeaps.clear();
            // CommandQueue-Typed-Pool Phase 3 — also drop any stale slot
            // flags collected without ever reaching Execute (would otherwise
            // leak into the next commitToGPU and over-stamp unrelated work).
            pendingSlots.clear();
        }
        // G.5.1 D3D12 follow-up — gate handlers staged by single-arg submits.
        // Non-empty branch: nextSubmitValue is the value just signaled for the
        // frame's Execute. Empty branch: there was no GPU work this commit, so
        // nextSubmitValue is the last signaled value and these handlers retire
        // on the next poll (normally there are none — a staged handler implies
        // its list was in `commandLists`, which makes the branch non-empty).
        gateStagedCompletions(nextSubmitValue);
    }
    engine->retentionQueue.drainCompleted();
    // Phase 3 — the same fence advance that retires retention queue
    // entries also retires transient-ring slots stamped on prior submits.
    if (transientRing) transientRing->retire();
    // G.5.1 D3D12 follow-up — fire any handlers the GPU has already retired.
    // Called every present()→commitToGPU, so the WTK BufferPool batches gated
    // here are recycled within a couple of frames (pending stays bounded).
    pollCompletions();
};

void GED3D12CommandQueue::commitToGPUAndWait() {
    commitToGPU();
    // Wait on a FRESH, monotonically-increasing fence value. The GPU signals
    // it only after the batch just flushed by commitToGPU() completes, so the
    // wait cannot be satisfied by a stale value from a prior call (the old
    // toggle-to-1-then-reset-to-0 scheme raced here, letting readbacks run
    // while the GPU was still executing — see the header note on `fence`).
    const std::uint64_t target = ++waitFenceValue;
    commandQueue->Signal(fence.Get(), target);
    if (fence->GetCompletedValue() < target) {
        fence->SetEventOnCompletion(target, cpuEvent);
        WaitForSingleObject(cpuEvent, INFINITE);
    }
    for (const auto traceId : submittedTraceCommandBufferIds) {
        ResourceTracking::Event completeEvent{};
        completeEvent.backend = ResourceTracking::Backend::D3D12;
        completeEvent.eventType = ResourceTracking::EventType::Complete;
        completeEvent.resourceType = "CommandBuffer";
        completeEvent.resourceId = traceId;
        completeEvent.queueId = traceResourceId;
        completeEvent.commandBufferId = traceId;
        completeEvent.nativeHandle = reinterpret_cast<std::uint64_t>(commandQueue.Get());
        ResourceTracking::Tracker::instance().emit(completeEvent);
        DEBUG_TRACE(DEBUG_DOMAIN_QUEUE, "CB complete: cb=" << traceId << " queue=" << traceResourceId);
    }
    submittedTraceCommandBufferIds.clear();
    // Wait above guarantees every prior submit's retention-fence value has
    // been reached, so any retention entries gated on this queue are
    // releasable now.
    engine->retentionQueue.drainCompleted();
    // Phase 3 — the same fence advance that retires retention queue
    // entries also retires transient-ring slots stamped on prior submits.
    if (transientRing) transientRing->retire();
    // G.5.1 D3D12 follow-up — the GPU is idle here, so every gated handler's
    // fence value has been reached; fire them all now.
    pollCompletions();
}

void GED3D12CommandQueue::commitTimedAsyncImpl(const GECommitCompletionHandler & onComplete,
                                              bool autonomousWaiter) {
    // GPU Commit-Timing P1 — async commit timing. The P2 aggregator
    // (installCommitAggregator) sets a fold handler on each buffer in the
    // pending batch; we then re-stage those buffers so the freshly-set handler
    // is staged with the buffer's pool slot and gated to this commit's Execute.
    // pollCompletions later fires each with its resolved per-buffer GPU span,
    // and the aggregator fires `onComplete` once with the whole-batch span.
    //
    // The handlers are staged at *commit* time here, deliberately separate from
    // the WTK recycler handler that submitCommandBuffer already staged at submit
    // time — both gate to the same Execute and fire independently, so this
    // composes with the recycler rather than clobbering it.
    if (onComplete) {
        installCommitAggregator(retainedCommandBuffers, onComplete);
        for (auto & handle : retainedCommandBuffers) {
            stageCompletionHandlerFrom(static_cast<GED3D12CommandBuffer *>(handle.get()));
        }
    }
    commitToGPU();
    // P1 structural fix #3 — when there is no frame loop to re-pump the poller
    // (standalone async commit), arm the waiter thread so the gated handler
    // fires once the GPU retires this commit. The synchronous form passes
    // false: its commitToGPUAndWait() drains and polls on the calling thread.
    if (onComplete && autonomousWaiter) {
        armCompletionWaiter();
    }
}

void GED3D12CommandQueue::commitToGPU(const GECommitCompletionHandler & onComplete) {
    commitTimedAsyncImpl(onComplete, /*autonomousWaiter=*/true);
}

GECommitCompletionInfo GED3D12CommandQueue::commitToGPUAndWaitTimed() {
    // GPU Commit-Timing P1 — sync counterpart. The base implementation drives
    // the async commitToGPU(handler) and blocks on a condition variable that
    // only pollCompletions can signal — but nothing pumps the poller while it
    // blocks, so it would deadlock on this backend. Instead: issue the timed
    // commit *without* the waiter (capturing its single fire), then
    // commitToGPUAndWait(), whose fence wait drains the batch and whose terminal
    // pollCompletions fires the gated aggregator handler on this thread with the
    // GPU idle. Suppressing the waiter avoids spinning an idle thread for the
    // synchronous one-shot path.
    GECommitCompletionInfo result {};
    commitTimedAsyncImpl([&result](const GECommitCompletionInfo & info) { result = info; },
                         /*autonomousWaiter=*/false);
    commitToGPUAndWait();
    return result;
}

void GED3D12CommandQueue::ensureCompletionWaiter() {
    std::lock_guard<std::mutex> lock(completionMutex_);
    if (waiterStarted_) {
        return;
    }
    // Manual-reset stop event: once set it stays set, so a teardown can't race
    // a fresh fence wait the loop is about to enter.
    waiterStopEvent_ = CreateEvent(NULL, TRUE, FALSE, NULL);
    waiterStarted_   = true;
    completionWaiter_ = std::thread(&GED3D12CommandQueue::completionWaiterLoop, this);
}

void GED3D12CommandQueue::armCompletionWaiter() {
    {
        // Nothing gated means the handler already fired synchronously — an empty
        // batch (installCommitAggregator's count==0 fast path) or a GPU that
        // finished before commitToGPU()'s own pollCompletions ran. Don't start a
        // thread we'd never need.
        std::lock_guard<std::mutex> lock(completionMutex_);
        if (gatedCompletionHandlers_.empty()) {
            return;
        }
    }
    ensureCompletionWaiter();
    {
        std::lock_guard<std::mutex> lock(completionMutex_);
        if (nextSubmitValue > waiterTargetValue_) {
            waiterTargetValue_ = nextSubmitValue;
        }
        waiterArmed_ = true;
    }
    waiterCv_.notify_one();
}

void GED3D12CommandQueue::completionWaiterLoop() {
    // Auto-reset event reused across iterations for each SetEventOnCompletion.
    HANDLE fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    for (;;) {
        std::uint64_t target;
        {
            std::unique_lock<std::mutex> lock(completionMutex_);
            waiterCv_.wait(lock, [this] { return waiterStop_ || waiterArmed_; });
            if (waiterStop_) {
                break;
            }
            waiterArmed_ = false;
            target = waiterTargetValue_;
        }
        // Wait (lock released) for the GPU to retire `target` or for teardown.
        // ID3D12Fence::GetCompletedValue / SetEventOnCompletion are free-threaded.
        if (retentionFence && retentionFence->GetCompletedValue() < target) {
            retentionFence->SetEventOnCompletion(target, fenceEvent);
            HANDLE handles[2] = { fenceEvent, waiterStopEvent_ };
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        }
        {
            std::lock_guard<std::mutex> lock(completionMutex_);
            if (waiterStop_) {
                break;
            }
        }
        // pollCompletions takes completionMutex_ itself, so it must be called
        // without holding it here.
        pollCompletions();
    }
    if (fenceEvent) {
        CloseHandle(fenceEvent);
    }
}

SharedHandle<GECommandBuffer> GED3D12CommandQueue::getAvailableBuffer() {
    // G.5.1 D3D12 follow-up — fire any frame-completion handlers the GPU has
    // retired. This runs at the start of every frame (FrameRenderPass::begin),
    // just before the WTK compositor's drainCompletedBufferReleases, so the
    // prior frames' `done` flags are flipped in time for their pooled buffers
    // to be recycled here.
    pollCompletions();
    // CommandQueue-Typed-Pool Phase 3 — growable pool with per-slot
    // in-flight tracking via the existing retentionFence. Walk the pool
    // from currentBufferIndex for locality; recycle the first slot whose
    // last submission has been completed by the GPU. On miss, grow the
    // pool by one (up to the hard 256 ceiling). When the pool first
    // crosses 4× the initial hint, log a one-shot warning so a user who
    // under-sized the hint sees the signal but isn't spammed each frame.
    if (poolAllocators.empty()) {
        // Defensive — desc.maxBufferCount == 0 would skip ctor preallocation;
        // grow once so the queue is still usable instead of returning null.
        const std::uint32_t fresh = growPoolOnce();
        if (fresh == UINT32_MAX) {
            return nullptr;
        }
    }

    const std::uint64_t completed = retentionFence ? retentionFence->GetCompletedValue() : 0;
    const std::uint32_t poolSize = static_cast<std::uint32_t>(poolAllocators.size());
    if (currentBufferIndex >= poolSize) {
        currentBufferIndex = 0;
    }

    std::uint32_t chosenSlot = UINT32_MAX;
    for (std::uint32_t step = 0; step < poolSize; ++step) {
        const std::uint32_t slot = (currentBufferIndex + step) % poolSize;
        if (poolSubmissionIndex[slot] <= completed) {
            chosenSlot = slot;
            break;
        }
    }
    if (chosenSlot == UINT32_MAX) {
        chosenSlot = growPoolOnce();
        if (chosenSlot == UINT32_MAX) {
            return nullptr;
        }
    }

    // Reset both allocator and list so the slot is ready for fresh
    // recording. GED3D12CommandBuffer::reset() does the same thing on a
    // re-used wrapper; we replicate it here for the very first use after
    // recycling.
    auto & alloc = poolAllocators[chosenSlot];
    auto & list  = poolLists[chosenSlot];
    if (FAILED(alloc->Reset())) {
        DEBUG_ERROR(DEBUG_DOMAIN_QUEUE, "GED3D12CommandQueue::getAvailableBuffer: allocator reset failed on slot " << chosenSlot);
        return nullptr;
    }
    if (FAILED(list->Reset(alloc.Get(), nullptr))) {
        DEBUG_ERROR(DEBUG_DOMAIN_QUEUE, "GED3D12CommandQueue::getAvailableBuffer: list reset failed on slot " << chosenSlot);
        return nullptr;
    }

    auto wrapper = SharedHandle<GECommandBuffer>(new GED3D12CommandBuffer(list.Get(), alloc.Get(), this));
    auto * d3d12_buffer = static_cast<GED3D12CommandBuffer *>(wrapper.get());
    d3d12_buffer->poolSlot = chosenSlot;
    // GPU Commit-Timing P1 — bracket this buffer's GPU span: write the start
    // timestamp now, as the first command on the freshly-reset list.
    writeStartTimestamp(list.Get(), chosenSlot);
    currentBufferIndex = (chosenSlot + 1) % static_cast<std::uint32_t>(poolAllocators.size());
    return wrapper;
};

ID3D12GraphicsCommandList6 *GED3D12CommandQueue::getLastCommandList() {
    // Returns nullptr when no command list is pending submission.
    // Callers (notably `GED3D12NativeRenderTarget::present`) treat null
    // as "queue already committed" and allocate a fresh barrier CB.
    // Pre queue-decoupling this never returned null because the render
    // target itself owned a queue and kept lists alive; post-decoupling
    // a caller can commitToGPU before present and leave the queue empty.
    if(commandLists.empty()) return nullptr;
    return commandLists.back();
}

GED3D12CommandQueue::~GED3D12CommandQueue() {
    ResourceTracking::Tracker::instance().emit(ResourceTracking::EventType::Destroy, ResourceTracking::Backend::D3D12,
                                               "CommandQueue", traceResourceId, commandQueue.Get());
    DEBUG_INFO(DEBUG_DOMAIN_QUEUE, "Queue destroyed: queue=" << traceResourceId);
    // GPU Commit-Timing P1 structural fix #3 — stop the async-completion waiter
    // before tearing down the fence / containers it reads. The manual-reset stop
    // event unblocks any in-flight WaitForMultipleObjects; the CV wakes a waiter
    // idling between arms; join guarantees no pollCompletions runs after this.
    if (waiterStarted_) {
        {
            std::lock_guard<std::mutex> lock(completionMutex_);
            waiterStop_ = true;
        }
        if (waiterStopEvent_) {
            SetEvent(waiterStopEvent_);
        }
        waiterCv_.notify_all();
        if (completionWaiter_.joinable()) {
            completionWaiter_.join();
        }
        if (waiterStopEvent_) {
            CloseHandle(waiterStopEvent_);
            waiterStopEvent_ = nullptr;
        }
    }
    // G.5.1 D3D12 follow-up — any staged/gated completion handlers that never
    // fired are dropped here (their std::functions destruct, releasing the
    // captured shared_ptr<atomic<bool>>). We intentionally do NOT fire them:
    // a handler must only fire once the GPU has reached its fence value, and
    // we don't force a GPU drain in this destructor. The WTK side does not
    // depend on the teardown firing — BackendRenderTargetContext's destructor
    // returns every still-pending PendingReleaseBatch directly to the
    // BufferPool (which outlives the per-window context), so no buffer leaks.
    // GPU Commit-Timing P1 — release the persistent readback mapping before the
    // ComPtr drops the resource. CPU never wrote to it, so an empty written
    // range is correct.
    if (timestampReadback && timestampMapped != nullptr) {
        timestampReadback->Unmap(0, nullptr);
        timestampMapped = nullptr;
    }
    CloseHandle(cpuEvent);
}

_NAMESPACE_END_
