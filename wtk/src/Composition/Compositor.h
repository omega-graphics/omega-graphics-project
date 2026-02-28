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
    enum class LaneEffectQuality : std::uint8_t {
        Full = 0,
        Reduced = 1,
        Minimal = 2
    };
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
               if(lhs->thresholdParams.timeStamp != rhs->thresholdParams.timeStamp)
                   return lhs->thresholdParams.timeStamp < rhs->thresholdParams.timeStamp;
               // tie-breaker: presence of explicit threshold
               if(lhs->thresholdParams.hasThreshold != rhs->thresholdParams.hasThreshold)
                   return lhs->thresholdParams.hasThreshold; // true first
               if(lhs->syncLaneId != rhs->syncLaneId){
                   return lhs->syncLaneId < rhs->syncLaneId;
               }
               if(lhs->syncPacketId != rhs->syncPacketId){
                   return lhs->syncPacketId < rhs->syncPacketId;
               }
               // final tie-breaker: id
               return lhs->id < rhs->id;
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
            bool hasRenderCommand = false;
            bool hasNonNoOpRender = false;
            bool hasStateMutation = false;
            bool hasEffectMutation = false;
            bool hasResizeMutation = false;
            bool layerTreeMirrorApplied = false;
            std::uint64_t requiredTreeEpoch = 0;
            ResizeGovernorMetadata resizeGovernor {};
            std::uint64_t resizeCoordinatorGeneration = 0;
        };

        struct LaneRuntimeState {
            unsigned inFlight = 0;
            std::chrono::steady_clock::time_point resizeModeUntil {};
            std::chrono::steady_clock::time_point lastAdmissionGrantTime {};
            bool startupStabilized = false;
            std::uint64_t firstPresentedPacketId = 0;
            bool hasPresentedRenderableContent = false;
            bool hasSubmitToPresentSample = false;
            bool hasGpuDurationSample = false;
            double submitToPresentEwmaMs = 0.0;
            double gpuDurationEwmaMs = 0.0;
            bool underPressure = false;
            LaneEffectQuality qualityLevel = LaneEffectQuality::Full;
            std::uint32_t qualityRecoveryStreak = 0;
            ResizeGovernorMetadata latestResizeGovernor {};
            std::uint64_t latestResizeCoordinatorGeneration = 0;

            std::uint64_t packetsQueued = 0;
            std::uint64_t packetsSubmitted = 0;
            std::uint64_t packetsGPUCompleted = 0;
            std::uint64_t packetsPresented = 0;
            std::uint64_t packetsDropped = 0;
            std::uint64_t packetsFailed = 0;
            std::uint64_t staleCoalescedCount = 0;
            std::uint64_t noOpTransparentDropCount = 0;
            std::uint64_t saturationCount = 0;
            std::uint64_t startupAdmissionHoldCount = 0;
            std::uint64_t admissionWaitCount = 0;
            std::chrono::microseconds totalAdmissionWait {0};
            std::uint64_t pacingWaitCount = 0;
            std::chrono::microseconds totalPacingWait {0};
            std::uint64_t epochWaitCount = 0;
            std::chrono::microseconds totalEpochWait {0};
            std::uint64_t epochDropCount = 0;
            unsigned maxInFlightObserved = 0;
        };

        struct GovernorTuningConfig {
            double pressureEnterLatencyMsResize = 18.0;
            double pressureExitLatencyMsResize = 12.0;
            double pressureEnterLatencyMsNormal = 28.0;
            double pressureExitLatencyMsNormal = 20.0;
            double latencyEwmaAlpha = 0.2;
            double gpuEwmaAlpha = 0.2;
            float velocityBudgetRelaxPxPerSec = 180.f;
            float velocityBudgetTightenPxPerSec = 1100.f;
            float velocityPacingMaxPxPerSec = 1800.f;
            double admissionSpacingFromLatencyFactor = 0.35;
            double admissionSpacingFromGpuFactor = 0.55;
            double admissionSpacingResizeMinMs = 1.0;
            double admissionSpacingResizeMaxMs = 9.0;
            double admissionSpacingPressureMs = 6.0;
        };

        struct PacketTelemetryState {
            std::mutex mutex;
            std::condition_variable *wakeCondition = nullptr;
            OmegaCommon::Map<std::uint64_t,LaneRuntimeState> laneRuntime;
            OmegaCommon::Map<std::uint64_t,OmegaCommon::Map<std::uint64_t,PacketLifecycleRecord>> lanes;
            GovernorTuningConfig tuning {};
        };

        using RenderTargetEpoch = std::pair<const CompositionRenderTarget *,const Layer *>;

        std::shared_ptr<PacketTelemetryState> packetTelemetryState;
        GovernorTuningConfig governorTuning {};

        struct QueueTelemetryState {
            std::array<std::uint64_t,5> queuedByType {};
            std::array<std::uint64_t,5> enqueuedByType {};
            std::array<std::uint64_t,5> dequeuedByType {};
            std::array<std::uint64_t,5> droppedByType {};
            std::chrono::steady_clock::time_point lastEmit {};
            std::uint64_t eventsSinceEmit = 0;
        };

        QueueTelemetryState queueTelemetryState {};

        enum class LayerTreeDeltaType : std::uint8_t {
            TreeAttached,
            TreeDetached,
            LayerResized,
            LayerEnabled,
            LayerDisabled
        };

        struct LayerTreeDelta {
            LayerTreeDeltaType type = LayerTreeDeltaType::LayerResized;
            LayerTree *tree = nullptr;
            Layer *layer = nullptr;
            Core::Rect rect {};
            std::uint64_t epoch = 0;
            std::chrono::steady_clock::time_point timestamp {};
        };

        struct LayerTreeSyncState {
            std::uint64_t lastIssuedEpoch = 0;
            std::uint64_t lastObservedEpoch = 0;
            OmegaCommon::Vector<LayerTreeDelta> pendingDeltas {};
        };

        OmegaCommon::Map<LayerTree *,LayerTreeSyncState> layerTreeSyncState;
        OmegaCommon::Map<LayerTree *,std::uint64_t> layerTreeLaneBinding;

        struct LayerTreePacketMetadata {
            std::uint64_t syncLaneId = 0;
            std::uint64_t syncPacketId = 0;
            OmegaCommon::Vector<LayerTreeDelta> deltas {};
            OmegaCommon::Map<LayerTree *,std::uint64_t> requiredEpochByTree {};
            bool mirrorApplied = false;
        };

        struct BackendLayerMirrorLayerState {
            Core::Rect rect {};
            bool enabled = true;
            std::uint64_t lastAppliedEpoch = 0;
        };

        struct BackendLayerMirrorTreeState {
            bool attached = false;
            std::uint64_t lastAppliedEpoch = 0;
            OmegaCommon::Map<Layer *,BackendLayerMirrorLayerState> layers {};
        };

        OmegaCommon::Map<std::uint64_t,OmegaCommon::Map<std::uint64_t,LayerTreePacketMetadata>> layerTreePacketMetadata;
        OmegaCommon::Map<LayerTree *,BackendLayerMirrorTreeState> backendLayerMirror;

        static constexpr unsigned kMaxFramesInFlightNormal = 2;
        static constexpr unsigned kMaxFramesInFlightResize = 1;
        static constexpr std::chrono::milliseconds kResizeModeHoldWindow {200};
        static constexpr float kAdaptiveShadowRadiusResize = 9.0f;
        static constexpr float kAdaptiveShadowBlurResize = 12.0f;
        static constexpr float kAdaptiveShadowRadiusPressure = 12.0f;
        static constexpr float kAdaptiveShadowBlurPressure = 16.0f;
        static constexpr float kAdaptiveCanvasBlurResize = 8.0f;
        static constexpr float kAdaptiveCanvasBlurPressure = 12.0f;
        static constexpr std::uint32_t kQualityRecoveryPresents = 6;
        static constexpr float kReducedEffectScale = 0.72f;
        static constexpr float kMinimalEffectScale = 0.5f;
        static constexpr float kReducedShadowOpacityScale = 0.9f;
        static constexpr float kMinimalShadowOpacityScale = 0.75f;
        static GovernorTuningConfig loadGovernorTuningConfig();

        static void collectRenderTargetsForCommand(const SharedHandle<CompositorCommand> & command,
                                                   OmegaCommon::Vector<RenderTargetEpoch> & targets);
        static bool targetsOverlap(const OmegaCommon::Vector<RenderTargetEpoch> & lhs,
                                   const OmegaCommon::Vector<RenderTargetEpoch> & rhs);
        bool isLaneSaturated(std::uint64_t syncLaneId) const;
        unsigned laneBudgetForNow(const LaneRuntimeState & laneState,
                                  std::chrono::steady_clock::time_point now) const;
        std::chrono::microseconds laneMinSubmitSpacingForNow(const LaneRuntimeState & laneState,
                                                             std::chrono::steady_clock::time_point now) const;
        static LaneEffectQuality desiredLaneQualityForNow(const LaneRuntimeState & laneState,
                                                          std::chrono::steady_clock::time_point now,
                                                          const GovernorTuningConfig & tuning);
        static void updateLaneQualityForPresentedPacket(LaneRuntimeState & laneState,
                                                        std::chrono::steady_clock::time_point now,
                                                        const GovernorTuningConfig & tuning);
        bool isLaneStartupCriticalPacket(std::uint64_t syncLaneId,std::uint64_t syncPacketId) const;
        bool waitForLaneAdmission(std::uint64_t syncLaneId,std::uint64_t syncPacketId);
        bool waitForRequiredTreeEpoch(std::uint64_t syncLaneId,
                                      std::uint64_t syncPacketId,
                                      std::uint64_t requiredTreeEpoch);
        bool arePacketEpochRequirementsSatisfiedLocked(std::uint64_t syncLaneId,
                                                       std::uint64_t syncPacketId,
                                                       std::uint64_t * maxObservedRequiredEpoch = nullptr,
                                                       std::uint64_t * maxMissingEpoch = nullptr) const;
        bool isPacketEpochSupersededLocked(std::uint64_t syncLaneId,
                                           std::uint64_t olderPacketId,
                                           std::uint64_t newerPacketId) const;
        bool packetMetadataContainsResizeDeltaLocked(std::uint64_t syncLaneId,
                                                     std::uint64_t syncPacketId) const;
        static std::uint64_t maxRequiredTreeEpoch(const LayerTreePacketMetadata & metadata);
        void stampCommandRequiredEpochLocked(SharedHandle<CompositorCommand> & command,
                                             std::uint64_t requiredEpoch) const;
        bool commandContainsResizeActivity(const SharedHandle<CompositorCommand> & command) const;
        bool commandContainsStateMutation(const SharedHandle<CompositorCommand> & command) const;
        bool commandContainsEffectMutation(const SharedHandle<CompositorCommand> & command) const;
        bool commandContainsRenderCommand(const SharedHandle<CompositorCommand> & command) const;
        bool commandHasNonNoOpRender(const SharedHandle<CompositorCommand> & command) const;
        void noteQueuePushLocked(const SharedHandle<CompositorCommand> & command);
        void noteQueuePopLocked(const SharedHandle<CompositorCommand> & command);
        void noteQueueDropLocked(const SharedHandle<CompositorCommand> & command);
        void maybeEmitQueueSnapshotLocked(const char *reason,
                                          std::uint64_t syncLaneId = 0,
                                          std::uint64_t syncPacketId = 0);
        void dropQueuedStaleForLaneLocked(std::uint64_t syncLaneId,
                                          const SharedHandle<CompositorCommand> & incoming);
        void markLaneResizeActivity(std::uint64_t syncLaneId,
                                    const ResizeGovernorMetadata * governorMetadata = nullptr);
        void markPacketQueued(std::uint64_t syncLaneId,
                              std::uint64_t syncPacketId,
                              const SharedHandle<CompositorCommand> & command);
        void markPacketSubmitted(std::uint64_t syncLaneId,std::uint64_t syncPacketId,std::chrono::steady_clock::time_point submitTimeCpu);
        void markPacketMirrorApplied(std::uint64_t syncLaneId,std::uint64_t syncPacketId);
        void markPacketDropped(std::uint64_t syncLaneId,
                               std::uint64_t syncPacketId,
                               PacketDropReason reason = PacketDropReason::Generic);
        void markPacketFailed(std::uint64_t syncLaneId,std::uint64_t syncPacketId);
        bool shouldDropNoOpTransparentFrame(std::uint64_t syncLaneId,std::uint64_t syncPacketId) const;
        void completePacketWithoutGpu(std::uint64_t syncLaneId,std::uint64_t syncPacketId);
        bool isLaneUnderPressure(std::uint64_t syncLaneId) const;
        LayerEffect::DropShadowParams adaptDropShadowForLane(std::uint64_t syncLaneId,
                                                              const LayerEffect::DropShadowParams & params) const;
        CanvasEffect adaptCanvasEffectForLane(std::uint64_t syncLaneId,
                                              const CanvasEffect & effect) const;
        static void onBackendSubmissionCompleted(std::weak_ptr<PacketTelemetryState> weakState,
                                                 const BackendSubmissionTelemetry & telemetry);
        std::weak_ptr<PacketTelemetryState> telemetryState() const;
        void coalesceLayerTreeDeltasLocked(OmegaCommon::Vector<LayerTreeDelta> & deltas) const;
        void bindPendingLayerTreeDeltasToPacketLocked(std::uint64_t syncLaneId,std::uint64_t syncPacketId);
        void applyLayerTreePacketDeltasToBackendMirror(std::uint64_t syncLaneId,
                                                       std::uint64_t syncPacketId,
                                                       BackendCompRenderTarget *target);
        void releaseLayerTreePacketMetadata(std::uint64_t syncLaneId,std::uint64_t syncPacketId);
        void enqueueLayerTreeDeltaLocked(LayerTree *tree,
                                         LayerTreeDeltaType type,
                                         Layer *layer,
                                         const Core::Rect *rect = nullptr);

        CompositorScheduler scheduler;

        friend class Layer;
        friend class LayerTree;
        friend class WindowLayer;

        void executeCurrentCommand();

    public:
        struct LayerTreeSyncSnapshot {
            bool observed = false;
            std::uint64_t lastIssuedEpoch = 0;
            std::uint64_t lastObservedEpoch = 0;
            std::size_t pendingDeltaCount = 0;
        };

        struct LaneTelemetrySnapshot {
            std::uint64_t syncLaneId = 0;
            std::uint64_t firstPresentedPacketId = 0;
            std::uint64_t lastSubmittedPacketId = 0;
            std::uint64_t lastPresentedPacketId = 0;
            std::uint64_t droppedPacketCount = 0;
            std::uint64_t failedPacketCount = 0;
            unsigned inFlight = 0;
            bool resizeBudgetActive = false;
            bool startupStabilized = false;
            bool underPressure = false;
            double submitToPresentEwmaMs = 0.0;
            double gpuDurationEwmaMs = 0.0;
            std::chrono::steady_clock::time_point lastPresentedTimeCpu {};
            LaneEffectQuality qualityLevel = LaneEffectQuality::Full;
            ResizeGovernorMetadata latestResizeGovernor {};
            std::uint64_t latestResizeCoordinatorGeneration = 0;
        };

        struct LaneDiagnosticsSnapshot {
            std::uint64_t syncLaneId = 0;
            std::uint64_t firstPresentedPacketId = 0;
            std::uint64_t lastSubmittedPacketId = 0;
            std::uint64_t lastPresentedPacketId = 0;
            std::uint64_t queuedPacketCount = 0;
            std::uint64_t submittedPacketCount = 0;
            std::uint64_t gpuCompletedPacketCount = 0;
            std::uint64_t presentedPacketCount = 0;
            std::uint64_t droppedPacketCount = 0;
            std::uint64_t failedPacketCount = 0;
            std::uint64_t staleCoalescedPacketCount = 0;
            std::uint64_t noOpTransparentDropCount = 0;
            std::uint64_t saturationCount = 0;
            std::uint64_t startupAdmissionHoldCount = 0;
            std::uint64_t admissionWaitCount = 0;
            double admissionWaitTotalMs = 0.0;
            std::uint64_t pacingWaitCount = 0;
            double pacingWaitTotalMs = 0.0;
            std::uint64_t epochWaitCount = 0;
            double epochWaitTotalMs = 0.0;
            std::uint64_t epochDropCount = 0;
            unsigned inFlight = 0;
            unsigned maxInFlightObserved = 0;
            bool resizeBudgetActive = false;
            bool startupStabilized = false;
            bool underPressure = false;
            double submitToPresentEwmaMs = 0.0;
            double gpuDurationEwmaMs = 0.0;
            std::chrono::steady_clock::time_point lastPresentedTimeCpu {};
            LaneEffectQuality qualityLevel = LaneEffectQuality::Full;
            ResizeGovernorMetadata latestResizeGovernor {};
            std::uint64_t latestResizeCoordinatorGeneration = 0;
            std::uint64_t staleCoordinatorGenerationPacketCount = 0;
        };

        LaneTelemetrySnapshot getLaneTelemetrySnapshot(std::uint64_t syncLaneId) const;
        LaneDiagnosticsSnapshot getLaneDiagnosticsSnapshot(std::uint64_t syncLaneId) const;
        LayerTreeSyncSnapshot getLayerTreeSyncSnapshot(LayerTree *tree);
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
