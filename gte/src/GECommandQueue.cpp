#include "omegaGTE/GECommandQueue.h"
#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>

_NAMESPACE_BEGIN_

    namespace {
        /// Backend-neutral accumulator shared by every per-buffer completion
        /// handler in one committed batch. The per-buffer callbacks fire on
        /// backend-internal threads (Metal's completion queue, the
        /// Vulkan/D3D12 retention poller), so every mutation is mutex-guarded.
        /// Fires the user's commit handler exactly once, after the final buffer
        /// in the batch reports.
        struct CommitAggregator {
            std::mutex mtx;
            unsigned remaining;
            bool seededTimes = false;
            GECommitCompletionInfo info;
            GECommitCompletionHandler onComplete;

            CommitAggregator(unsigned count, GECommitCompletionHandler handler):
                remaining(count), onComplete(std::move(handler)) {
                info.commandBufferCount = count;
            }

            void report(const GECommandBufferCompletionInfo & cb) {
                GECommitCompletionHandler fire;
                GECommitCompletionInfo snapshot;
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    if(cb.status == GECommandBufferCompletionInfo::CompletionStatus::Error) {
                        info.status = GECommandBufferCompletionInfo::CompletionStatus::Error;
                    }
                    else {
                        // Fold the GPU span only from buffers that actually ran,
                        // so a 0.0 from an errored / untimed buffer can't poison
                        // min(start).
                        if(!seededTimes) {
                            info.gpuStartTimeSec = cb.gpuStartTimeSec;
                            info.gpuEndTimeSec   = cb.gpuEndTimeSec;
                            seededTimes = true;
                        }
                        else {
                            info.gpuStartTimeSec = std::min(info.gpuStartTimeSec, cb.gpuStartTimeSec);
                            info.gpuEndTimeSec   = std::max(info.gpuEndTimeSec, cb.gpuEndTimeSec);
                        }
                    }
                    if(remaining > 0) {
                        --remaining;
                        if(remaining == 0) {
                            fire = onComplete;
                            snapshot = info;
                        }
                    }
                }
                if(fire) {
                    fire(snapshot);
                }
            }
        };
    }

    void GECommandQueue::installCommitAggregator(
            const std::vector<SharedHandle<GECommandBuffer>> & batch,
            const GECommitCompletionHandler & onComplete) {
        if(!onComplete) {
            return;
        }
        // Count only buffers that can actually report a completion.
        unsigned count = 0;
        for(const auto & handle : batch) {
            if(handle.get() != nullptr) {
                ++count;
            }
        }
        if(count == 0) {
            // Nothing to time — still honor the "fires exactly once" contract.
            GECommitCompletionInfo info {};
            info.commandBufferCount = 0;
            onComplete(info);
            return;
        }
        auto agg = std::make_shared<CommitAggregator>(count, onComplete);
        for(const auto & handle : batch) {
            GECommandBuffer *cb = handle.get();
            if(cb == nullptr) {
                continue;
            }
            // Compose with any handler already on the buffer (e.g. the WTK
            // recycler's) instead of clobbering it.
            GECommandBufferCompletionHandler previous = cb->getCompletionHandler();
            cb->setCompletionHandler(
                [agg, previous](const GECommandBufferCompletionInfo & info) {
                    if(previous) {
                        previous(info);
                    }
                    agg->report(info);
                });
        }
    }

    void GECommandQueue::commitToGPU(const GECommitCompletionHandler & onComplete) {
        // Base fallback for backends that have not wired real per-commit GPU
        // timing yet (Vulkan / D3D12 until the GPU-Commit-Timing plan's P1
        // phase). Run the batch to completion, then fire the handler once with
        // status = Completed and zero timing (gpuDurationSec() == 0 signals
        // "timing unavailable on this backend"). Metal overrides this with the
        // real async, timestamp-backed path.
        commitToGPUAndWait();
        if(onComplete) {
            GECommitCompletionInfo info {};
            onComplete(info);
        }
    }

    GECommitCompletionInfo GECommandQueue::commitToGPUAndWaitTimed() {
        // Backend-neutral: drive the async timing path and block until it
        // reports. Works for any backend that implements
        // commitToGPU(handler) correctly; with the base fallback above it
        // returns immediately with zero timing.
        GECommitCompletionInfo result {};
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        commitToGPU([&result, &m, &cv, &done](const GECommitCompletionInfo & info) {
            {
                std::lock_guard<std::mutex> lock(m);
                result = info;
                done = true;
            }
            cv.notify_one();
        });
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&done]{ return done; });
        return result;
    }

    GECommandQueue::GECommandQueue(const GECommandQueueDesc & desc,
                                   GECommandQueueDesc::Type achievedType):
        size(desc.maxBufferCount), desc_(desc), requestedType_(desc.type) {
        // Record what the backend actually allocated (post-fallback) on the
        // queue's `desc_.type`, but keep the user's original request in
        // `requestedType_` so isDedicated() can compare.
        desc_.type = achievedType;
    };

    GERenderPassDescriptor::ColorAttachment::ClearColor::ClearColor(float r,float g,float b,float a):r(r),g(g),b(b),a(a){

    }

    GERenderPassDescriptor::ColorAttachment::ColorAttachment(ClearColor clearColor,LoadAction loadAction):
    clearColor(clearColor),loadAction(loadAction){

    };
    GERenderPassDescriptor::ColorAttachment::ColorAttachment(ClearColor clearColor,LoadAction loadAction,SharedHandle<GETextureRenderTarget> renderTarget):
    loadAction(loadAction),clearColor(clearColor),renderTarget(std::move(renderTarget)){

    };

    unsigned GECommandQueue::getSize(){
        return size;
    };
_NAMESPACE_END_
