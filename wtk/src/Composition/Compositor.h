#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/CompositorClient.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "backend/RenderTarget.h"

#ifndef OMEGAWTK_COMPOSTION_COMPOSITOR_H
#define OMEGAWTK_COMPOSTION_COMPOSITOR_H

namespace OmegaWTK::Composition {
    struct CanvasEffect;
    // LaneEffectQuality removed (Phase 3).
    // template<class FnTy>
    // class WorkerFarm {    
    //     struct ThreadStatus {
    //         bool busy;
    //     };
    //     std::map<std::thread,ThreadStatus> threadsInFlight;
    //     void _makeThread(FnTy fn){
    //         threadsInFlight.insert(std::make_pair([&]{
    //             fn();
    //         },{false}));
    //     }
    // public:
    //     WorkerFarm<FnTy>(int startThreads){

    //     };
    //     void assign(FnTy fn){

    //     };
    // };
   
   class CompositorScheduler {
       Compositor *compositor;
   public:
        bool shutdown;

        void shutdownAndJoin();
        void processCommand(SharedHandle<CompositorCommand> & command,bool laneAdmissionBypassed = false);
        std::thread t;

       explicit CompositorScheduler(Compositor *compositor);
       ~CompositorScheduler();
   };


   struct CompareCommands {
        auto operator()(SharedHandle<Composition::CompositorCommand> & lhs,
                        SharedHandle<Composition::CompositorCommand> & rhs){
        // View commands first (highest structural priority)
        if(lhs->type == CompositorCommand::View && rhs->type != CompositorCommand::View) return true;
           if(rhs->type == CompositorCommand::View && lhs->type != CompositorCommand::View) return false;

           // Cancel next
           if(lhs->type == CompositorCommand::Cancel && rhs->type != CompositorCommand::Cancel) return true;
           if(rhs->type == CompositorCommand::Cancel && lhs->type != CompositorCommand::Cancel) return false;

           const bool lhsRenderLike =
                   lhs->type == CompositorCommand::Render ||
                   lhs->type == CompositorCommand::Packet;
           const bool rhsRenderLike =
                   rhs->type == CompositorCommand::Render ||
                   rhs->type == CompositorCommand::Packet;

           if(lhsRenderLike && rhsRenderLike) {
               // 1. Command priority: High before Low
               if(lhs->priority != rhs->priority)
                   return lhs->priority > rhs->priority; // High(1) > Low(0)
               // 2. Timestamp ordering
               if(lhs->thresholdParams.timeStamp != rhs->thresholdParams.timeStamp)
                   return lhs->thresholdParams.timeStamp < rhs->thresholdParams.timeStamp;
               // 3. Presence of explicit threshold
               if(lhs->thresholdParams.hasThreshold != rhs->thresholdParams.hasThreshold)
                   return lhs->thresholdParams.hasThreshold; // true first
               // 4. Sync lane grouping
               if(lhs->syncLaneId != rhs->syncLaneId){
                   return lhs->syncLaneId < rhs->syncLaneId;
               }
               // 5. Packet ordering within same lane
               if(lhs->syncPacketId != rhs->syncPacketId){
                   return lhs->syncPacketId < rhs->syncPacketId;
               }
               // 6. Global submission sequence (FIFO tie-breaker)
               return lhs->sequenceNumber < rhs->sequenceNumber;
           }
            // Fallback deterministic ordering: compare types
           return static_cast<int>(lhs->type) < static_cast<int>(rhs->type);
        };
    };

   
    /**
     OmegaWTK's Composition Engine Frontend Interface
     */
    class Compositor : public LayerTreeObserver {

        OmegaCommon::Vector<LayerTree *> targetLayerTrees;

        RenderTargetStore renderTargetStore;

        std::mutex mutex;

        bool queueIsReady;

        std::condition_variable queueCondition;
        OmegaCommon::PriorityQueueHeap<SharedHandle<CompositorCommand>,CompareCommands> commandQueue;

        friend class CompositorClientProxy;
        friend class CompositorScheduler;

        SharedHandle<CompositorCommand> currentCommand;

        bool allowUpdates = false;

        enum class PacketLifecyclePhase : std::uint8_t {
            Queued,
            Submitted,
            GPUCompleted,
            Presented,
            Dropped,
            Failed
        };

        enum class PacketDropReason : std::uint8_t {
            Generic,
            StaleCoalesced,
            NoOpTransparent,
            EpochSuperseded
        };

        struct PacketLifecycleRecord {
            PacketLifecyclePhase phase = PacketLifecyclePhase::Queued;
            std::chrono::steady_clock::time_point queuedTimeCpu {};
            std::chrono::steady_clock::time_point submitTimeCpu {};
            std::chrono::steady_clock::time_point gpuCompleteTimeCpu {};
            std::chrono::steady_clock::time_point presentTimeCpu {};
            double gpuStartTimeSec = 0.0;
            double gpuEndTimeSec = 0.0;
            BackendSubmissionStatus backendStatus = BackendSubmissionStatus::Completed;
            unsigned pendingSubmissions = 0;
            bool hasNonNoOpRender = false;
            bool layerTreeMirrorApplied = false;
        };

        struct LaneRuntimeState {
            unsigned inFlight = 0;
            unsigned maxInFlightObserved = 0;
            bool startupStabilized = false;
            std::uint64_t firstPresentedPacketId = 0;
            bool hasPresentedRenderableContent = false;

            std::uint64_t packetsQueued = 0;
            std::uint64_t packetsSubmitted = 0;
            std::uint64_t packetsGPUCompleted = 0;
            std::uint64_t packetsPresented = 0;
            std::uint64_t packetsDropped = 0;
            std::uint64_t packetsFailed = 0;
            std::uint64_t staleCoalescedCount = 0;
            std::uint64_t noOpTransparentDropCount = 0;
            std::uint64_t admissionWaitCount = 0;
        };

        struct PacketTelemetryState {
            std::mutex mutex;
            std::condition_variable *wakeCondition = nullptr;
            OmegaCommon::Map<std::uint64_t,LaneRuntimeState> laneRuntime;
            OmegaCommon::Map<std::uint64_t,OmegaCommon::Map<std::uint64_t,PacketLifecycleRecord>> lanes;
        };

        using RenderTargetEpoch = std::pair<const CompositionRenderTarget *,const Layer *>;

        std::shared_ptr<PacketTelemetryState> packetTelemetryState;

        struct QueueTelemetryState {
            std::array<std::uint64_t,5> queuedByType {};
            std::array<std::uint64_t,5> enqueuedByType {};
            std::array<std::uint64_t,5> dequeuedByType {};
            std::array<std::uint64_t,5> droppedByType {};
            std::chrono::steady_clock::time_point lastEmit {};
            std::uint64_t eventsSinceEmit = 0;
        };

        QueueTelemetryState queueTelemetryState {};

        // Layer tree delta/mirror/epoch system removed (Phase 2).
        // Native layer geometry is now managed on the main thread.

        static constexpr unsigned kMaxFramesInFlightNormal = 2;

        static void collectRenderTargetsForCommand(const SharedHandle<CompositorCommand> & command,
                                                   OmegaCommon::Vector<RenderTargetEpoch> & targets);
        static bool targetsOverlap(const OmegaCommon::Vector<RenderTargetEpoch> & lhs,
                                   const OmegaCommon::Vector<RenderTargetEpoch> & rhs);
        unsigned laneBudgetForNow(const LaneRuntimeState & laneState) const;
        bool isLaneStartupCriticalPacket(std::uint64_t syncLaneId,std::uint64_t syncPacketId) const;
        bool waitForLaneAdmission(std::uint64_t syncLaneId,std::uint64_t syncPacketId);
        bool commandHasNonNoOpRender(const SharedHandle<CompositorCommand> & command) const;
        void noteQueuePushLocked(const SharedHandle<CompositorCommand> & command);
        void noteQueuePopLocked(const SharedHandle<CompositorCommand> & command);
        void noteQueueDropLocked(const SharedHandle<CompositorCommand> & command);
        void maybeEmitQueueSnapshotLocked(const char *reason,
                                          std::uint64_t syncLaneId = 0,
                                          std::uint64_t syncPacketId = 0);
        void dropQueuedStaleForLaneLocked(std::uint64_t syncLaneId,
                                          const SharedHandle<CompositorCommand> & incoming);
        void markPacketQueued(std::uint64_t syncLaneId,
                              std::uint64_t syncPacketId,
                              const SharedHandle<CompositorCommand> & command);
        void markPacketSubmitted(std::uint64_t syncLaneId,std::uint64_t syncPacketId,std::chrono::steady_clock::time_point submitTimeCpu);
        void markPacketDropped(std::uint64_t syncLaneId,
                               std::uint64_t syncPacketId,
                               PacketDropReason reason = PacketDropReason::Generic);
        void markPacketFailed(std::uint64_t syncLaneId,std::uint64_t syncPacketId);
        bool shouldDropNoOpTransparentFrame(std::uint64_t syncLaneId,std::uint64_t syncPacketId) const;
        void completePacketWithoutGpu(std::uint64_t syncLaneId,std::uint64_t syncPacketId);
        static void onBackendSubmissionCompleted(std::weak_ptr<PacketTelemetryState> weakState,
                                                 const BackendSubmissionTelemetry & telemetry);
        std::weak_ptr<PacketTelemetryState> telemetryState() const;

        CompositorScheduler scheduler;

        friend class Layer;
        friend class LayerTree;
        friend class WindowLayer;

        void executeCurrentCommand();
        void onQueueDrained();

    public:
        // LayerTreeSyncSnapshot removed (Phase 2).

        struct LaneTelemetrySnapshot {
            std::uint64_t syncLaneId = 0;
            std::uint64_t firstPresentedPacketId = 0;
            std::uint64_t lastSubmittedPacketId = 0;
            std::uint64_t lastPresentedPacketId = 0;
            std::uint64_t droppedPacketCount = 0;
            std::uint64_t failedPacketCount = 0;
            unsigned inFlight = 0;
            bool startupStabilized = false;
        };

        struct LaneDiagnosticsSnapshot {
            std::uint64_t syncLaneId = 0;
            std::uint64_t firstPresentedPacketId = 0;
            std::uint64_t lastSubmittedPacketId = 0;
            std::uint64_t lastPresentedPacketId = 0;
            std::uint64_t queuedPacketCount = 0;
            std::uint64_t submittedPacketCount = 0;
            std::uint64_t presentedPacketCount = 0;
            std::uint64_t droppedPacketCount = 0;
            std::uint64_t failedPacketCount = 0;
            unsigned inFlight = 0;
            bool startupStabilized = false;
        };

        LaneTelemetrySnapshot getLaneTelemetrySnapshot(std::uint64_t syncLaneId) const;
        LaneDiagnosticsSnapshot getLaneDiagnosticsSnapshot(std::uint64_t syncLaneId) const;
        OmegaCommon::String dumpLaneDiagnostics(std::uint64_t syncLaneId) const;

        void observeLayerTree(LayerTree *tree,std::uint64_t syncLaneId = 0);
        void unobserveLayerTree(LayerTree *tree);

        void scheduleCommand(SharedHandle<CompositorCommand> & command);

        void hasDetached(LayerTree *tree) override;
        void layerHasResized(Layer *layer) override;
        void layerHasDisabled(Layer *layer) override;
        void layerHasEnabled(Layer *layer) override;
        
        
        Compositor();
        ~Compositor();
    };

    struct BackendExecutionContext {

    };
}

#endif
