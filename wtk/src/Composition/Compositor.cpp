#include "Compositor.h"
#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/Canvas.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>

#if defined(TARGET_MACOS)
#include <dispatch/dispatch.h>
#include <pthread.h>
#endif

namespace OmegaWTK::Composition {

namespace {
    /// Global monotonic counter for command sequence numbers.
    /// Assigned in scheduleCommand() to capture the true submission order
    /// of commands arriving at the compositor queue.
    static std::atomic<uint64_t> g_commandSequenceSeed {1};

    static constexpr std::size_t kQueueTypeCount = 5;

    static inline bool syncTraceEnabled(){
        static const bool enabled = []{
            const char *raw = std::getenv("OMEGAWTK_SYNC_TRACE");
            return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
        }();
        return enabled;
    }

    static inline void emitSyncTrace(const std::string & message){
        if(syncTraceEnabled()){
            std::cout << "[OmegaWTKSync] " << message << std::endl;
        }
    }

    static inline bool queueTraceEnabled(){
        static const bool enabled = []{
            const char *raw = std::getenv("OMEGAWTK_QUEUE_TRACE");
            return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
        }();
        return enabled;
    }

    static inline void emitQueueTrace(const std::string & message){
        if(queueTraceEnabled()){
            std::cout << "[OmegaWTKQueue] " << message << std::endl;
        }
    }

    static inline bool governorTuningTraceEnabled(){
        static const bool enabled = []{
            const char *raw = std::getenv("OMEGAWTK_RESIZE_GOVERNOR_TUNING_TRACE");
            return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
        }();
        return enabled;
    }

    static inline void emitGovernorTuningTrace(const std::string & message){
        if(governorTuningTraceEnabled()){
            std::cout << "[OmegaWTKGovernor] " << message << std::endl;
        }
    }

    static inline double readEnvDoubleClamp(const char *name,double fallback,double minValue,double maxValue){
        const char *raw = std::getenv(name);
        if(raw == nullptr || raw[0] == '\0'){
            return fallback;
        }
        char *endPtr = nullptr;
        const auto parsed = std::strtod(raw,&endPtr);
        if(endPtr == raw || !std::isfinite(parsed)){
            return fallback;
        }
        return std::clamp(parsed,minValue,maxValue);
    }

    static inline float readEnvFloatClamp(const char *name,float fallback,float minValue,float maxValue){
        const char *raw = std::getenv(name);
        if(raw == nullptr || raw[0] == '\0'){
            return fallback;
        }
        char *endPtr = nullptr;
        const auto parsed = std::strtof(raw,&endPtr);
        if(endPtr == raw || !std::isfinite(parsed)){
            return fallback;
        }
        return std::clamp(parsed,minValue,maxValue);
    }

    static inline std::size_t queueCommandTypeIndex(CompositorCommand::Type type){
        switch(type){
            case CompositorCommand::Render:
                return 0;
            case CompositorCommand::View:
                return 1;
            case CompositorCommand::Layer:
                return 2;
            case CompositorCommand::Cancel:
                return 3;
            case CompositorCommand::Packet:
                return 4;
            default:
                return 0;
        }
    }

    static inline const char *queueCommandTypeName(std::size_t idx){
        switch(idx){
            case 0:
                return "render";
            case 1:
                return "view";
            case 2:
                return "layer";
            case 3:
                return "cancel";
            case 4:
                return "packet";
            default:
                return "unknown";
        }
    }

    static inline std::string queueCountSummary(const std::array<std::uint64_t,kQueueTypeCount> &counts){
        std::ostringstream ss;
        for(std::size_t idx = 0; idx < counts.size(); ++idx){
            if(idx != 0){
                ss << ",";
            }
            ss << queueCommandTypeName(idx) << "=" << counts[idx];
        }
        return ss.str();
    }


    static inline bool isRenderLikeCommand(const SharedHandle<CompositorCommand> & command){
        return command != nullptr &&
               (command->type == CompositorCommand::Render ||
                command->type == CompositorCommand::Packet);
    }

    static inline bool commandContainsStateMutationRecursive(const SharedHandle<CompositorCommand> & command){
        if(command == nullptr){
            return false;
        }
        if(command->type == CompositorCommand::Layer ||
           command->type == CompositorCommand::View ||
           command->type == CompositorCommand::Cancel){
            return true;
        }
        if(command->type == CompositorCommand::Packet){
            auto packet = std::dynamic_pointer_cast<CompositorPacketCommand>(command);
            if(packet == nullptr){
                return false;
            }
            for(auto & child : packet->commands){
                if(commandContainsStateMutationRecursive(child)){
                    return true;
                }
            }
        }
        return false;
    }

    static inline bool commandContainsEffectMutationRecursive(const SharedHandle<CompositorCommand> & command){
        if(command == nullptr){
            return false;
        }
        if(command->type == CompositorCommand::Layer){
            auto layerCommand = std::dynamic_pointer_cast<CompositorLayerCommand>(command);
            return layerCommand != nullptr && layerCommand->subtype == CompositorLayerCommand::Effect;
        }
        if(command->type == CompositorCommand::Render){
            auto render = std::dynamic_pointer_cast<CompositionRenderCommand>(command);
            return render != nullptr &&
                   render->frame != nullptr &&
                   !render->frame->currentEffects.empty();
        }
        if(command->type == CompositorCommand::Packet){
            auto packet = std::dynamic_pointer_cast<CompositorPacketCommand>(command);
            if(packet == nullptr){
                return false;
            }
            for(auto & child : packet->commands){
                if(commandContainsEffectMutationRecursive(child)){
                    return true;
                }
            }
        }
        return false;
    }

    static inline bool commandContainsRenderCommandRecursive(const SharedHandle<CompositorCommand> & command){
        if(command == nullptr){
            return false;
        }
        if(command->type == CompositorCommand::Render){
            return true;
        }
        if(command->type == CompositorCommand::Packet){
            auto packet = std::dynamic_pointer_cast<CompositorPacketCommand>(command);
            if(packet == nullptr){
                return false;
            }
            for(auto & child : packet->commands){
                if(commandContainsRenderCommandRecursive(child)){
                    return true;
                }
            }
        }
        return false;
    }

    static inline bool hasSyncPacketMetadata(const SharedHandle<CompositorCommand> & command){
        return isRenderLikeCommand(command) &&
               command->syncLaneId != 0 &&
               command->syncPacketId != 0;
    }

    static inline bool isNoOpTransparentFrame(const SharedHandle<CanvasFrame> & frame){
        if(frame == nullptr){
            return true;
        }
        auto &bkgrd = frame->background;
        return frame->currentVisuals.empty() &&
               frame->currentEffects.empty() &&
               bkgrd.r == 0.f &&
               bkgrd.g == 0.f &&
               bkgrd.b == 0.f &&
               bkgrd.a == 0.f;
    }

    static inline double durationToMs(const std::chrono::steady_clock::duration & duration){
        return std::chrono::duration<double,std::milli>(duration).count();
    }

    static void collectLayersForTree(LayerTree *tree,
                                     OmegaCommon::Vector<Layer *> &layers){
        if(tree == nullptr){
            return;
        }
        tree->collectAllLayers(layers);
    }

}


void Compositor::observeLayerTree(LayerTree *tree,std::uint64_t syncLaneId){
    if(tree == nullptr){
        return;
    }
    bool alreadyObserved = false;
    {
        std::lock_guard<std::mutex> lk(mutex);
        for(auto *targetTree : targetLayerTrees){
            if(targetTree == tree){
                alreadyObserved = true;
                break;
            }
        }
        if(!alreadyObserved){
            targetLayerTrees.push_back(tree);
        }
    }
    if(!alreadyObserved){
        tree->addObserver(this);
    }
}

void Compositor::unobserveLayerTree(LayerTree *tree){
    if(tree == nullptr){
        return;
    }
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(mutex);
        for(auto it = targetLayerTrees.begin(); it != targetLayerTrees.end(); ++it){
            if(*it == tree){
                targetLayerTrees.erase(it);
                removed = true;
                break;
            }
        }
        // Delta/mirror tracking removed (Phase 2).
    }
    if(removed){
        tree->removeObserver(this);
    }
}

void Compositor::hasDetached(LayerTree *tree){
    renderTargetStore.cleanTreeTargets(tree);
    unobserveLayerTree(tree);
}

void Compositor::layerHasResized(Layer *){ }
void Compositor::layerHasDisabled(Layer *){ }
void Compositor::layerHasEnabled(Layer *){ }

void CompositorScheduler::processCommand(SharedHandle<CompositorCommand> & command,bool laneAdmissionBypassed){
    if(command == nullptr){
        return;
    }
    if(hasSyncPacketMetadata(command) && !laneAdmissionBypassed){
        if(!compositor->waitForLaneAdmission(command->syncLaneId,command->syncPacketId)){
            compositor->markPacketFailed(command->syncLaneId,command->syncPacketId);
            command->status.set(CommandStatus::Failed);
            return;
        }
    }
    if(command->type == CompositorCommand::Packet){
        auto packet = std::dynamic_pointer_cast<CompositorPacketCommand>(command);
        if(packet == nullptr){
            compositor->markPacketFailed(command->syncLaneId,command->syncPacketId);
            command->status.set(CommandStatus::Failed);
            return;
        }
        bool hasRenderChild = false;
        for(auto &childCommand : packet->commands){
            if(childCommand == nullptr){
                continue;
            }
            if(childCommand->type == CompositorCommand::Render){
                hasRenderChild = true;
            }
            {
                std::lock_guard<std::mutex> lk(compositor->mutex);
                compositor->currentCommand = childCommand;
            }
            processCommand(childCommand,true);
        }

        if(!hasRenderChild){
            compositor->completePacketWithoutGpu(command->syncLaneId,command->syncPacketId);
        }
        command->status.set(CommandStatus::Ok);
        return;
    }
    auto _now = std::chrono::high_resolution_clock::now();
    std::cout << "Processing Command:" << command->id << std::endl;
    auto executeCurrentCommand = [&](){
        compositor->executeCurrentCommand();
    };

    if(command->thresholdParams.hasThreshold) {
        if(command->thresholdParams.threshold >= _now){
            /// Command will execute on time.
            std::unique_lock<std::mutex> lk(compositor->mutex);
            compositor->queueCondition.wait_until(lk,command->thresholdParams.threshold,[&]{
                return shutdown;
            });
            if(shutdown){
                compositor->markPacketFailed(command->syncLaneId,command->syncPacketId);
                if(!laneAdmissionBypassed){
                }
                command->status.set(CommandStatus::Failed);
                return;
            }
            lk.unlock();
            
            executeCurrentCommand();
            
        }
        else {
            // Command is late!!
            executeCurrentCommand();
        };
    }
    else {
        /// Command will be executed right away.
        executeCurrentCommand();
    }
    if(!laneAdmissionBypassed){
    }
};




CompositorScheduler::CompositorScheduler(Compositor * compositor):compositor(compositor),shutdown(false),t([this](Compositor *compositor){
//        std::cout << "--> Starting Up" << std::endl;
        while(true){
            SharedHandle<CompositorCommand> command;
            {
                std::unique_lock<std::mutex> lk(compositor->mutex);
                compositor->queueCondition.wait(lk,[&]{
                    return shutdown || !compositor->commandQueue.empty();
                });
                if(shutdown){
                    while(!compositor->commandQueue.empty()){
                        auto pending = compositor->commandQueue.first();
                        compositor->commandQueue.pop();
                        compositor->noteQueueDropLocked(pending);
                        compositor->markPacketFailed(pending->syncLaneId,pending->syncPacketId);
                        pending->status.set(CommandStatus::Failed);
                    }
                    compositor->maybeEmitQueueSnapshotLocked("shutdown");
                    break;
                }
                command = compositor->commandQueue.first();
                compositor->commandQueue.pop();
                compositor->noteQueuePopLocked(command);
                compositor->currentCommand = command;
            }
            processCommand(command);
            // Present completed render targets after each command so that
            // lane admission (inFlight budget) can advance for the next command.
            // Previously this only ran on queue drain, causing a circular
            // dependency when packets from different Views shared a lane.
            compositor->renderTargetStore.presentAllPending();
            // Decrement inFlight after present so the next frame can be
            // admitted.  We bypass recordGPUCompletion / onBackendSubmissionCompleted
            // because those update the submit-to-present EWMA — and using
            // wall-clock time here (which includes queue wait) would inflate
            // the EWMA and trigger false pressure / quality degradation.
            if(command != nullptr &&
               command->syncLaneId != 0 && command->syncPacketId != 0 &&
               compositor->packetTelemetryState != nullptr){
                std::lock_guard<std::mutex> telemetryLk(compositor->packetTelemetryState->mutex);
                auto & laneState = compositor->packetTelemetryState->laneRuntime[command->syncLaneId];
                auto & entry = compositor->packetTelemetryState->lanes[command->syncLaneId][command->syncPacketId];
                if(entry.pendingSubmissions > 0){
                    entry.pendingSubmissions -= 1;
                }
                if(entry.pendingSubmissions == 0 && laneState.inFlight > 0){
                    laneState.inFlight -= 1;
                    if(entry.phase != Compositor::PacketLifecyclePhase::Presented){
                        entry.phase = Compositor::PacketLifecyclePhase::Presented;
                        laneState.packetsPresented += 1;
                        if(!laneState.startupStabilized){
                            laneState.startupStabilized = true;
                            laneState.firstPresentedPacketId = command->syncPacketId;
                        }
                        if(entry.hasNonNoOpRender){
                            laneState.hasPresentedRenderableContent = true;
                        }
                    }
                }
                if(compositor->packetTelemetryState->wakeCondition != nullptr){
                    compositor->packetTelemetryState->wakeCondition->notify_all();
                }
            }
            {
                std::lock_guard<std::mutex> lk(compositor->mutex);
                if(compositor->commandQueue.empty()){
                    compositor->onQueueDrained();
                }
            }
        };

        {
            std::lock_guard<std::mutex> lk(compositor->mutex);
            if(!compositor->commandQueue.empty()){
                std::cout << "--> Unfinished Jobs:" << compositor->commandQueue.length() << std::endl;
            };
        }
        
        std::cout << "--> Shutting Down" << std::endl;
    },compositor){

};

void CompositorScheduler::shutdownAndJoin(){
    {
        std::lock_guard<std::mutex> lk(compositor->mutex); 
        shutdown = true;
    }
    compositor->queueCondition.notify_all();
    if(t.joinable()){
        if(t.get_id() == std::this_thread::get_id()){
            t.detach();
        }
        else {
            t.join();
        }
    }
}

CompositorScheduler::~CompositorScheduler(){
    std::cout << "close" << std::endl;
    shutdownAndJoin();
};


Compositor::Compositor():
queueIsReady(false),
queueCondition(),
commandQueue(200),
packetTelemetryState(std::make_shared<PacketTelemetryState>()),
scheduler(this){
    packetTelemetryState->wakeCondition = &queueCondition;
};

Compositor::~Compositor(){
     OmegaCommon::Vector<LayerTree *> observedTrees {};
     {
         std::lock_guard<std::mutex> lk(mutex);
         observedTrees = targetLayerTrees;
         targetLayerTrees.clear();
     }
     for(auto *tree : observedTrees){
         if(tree != nullptr){
             tree->removeObserver(this);
         }
     }
     scheduler.shutdownAndJoin();
     // Wait for all GPU work to complete before releasing render targets.
     if(gte.graphicsEngine != nullptr){
         gte.graphicsEngine->waitForGPUIdle();
     }
     renderTargetStore.store.clear();
     std::cout << "~Compositor()" << std::endl;
};

void Compositor::scheduleCommand(SharedHandle<CompositorCommand> & command){
    if(command == nullptr){
        return;
    }
    const auto traceLaneId = command->syncLaneId;
    const auto tracePacketId = command->syncPacketId;
    const auto traceCommandId = command->id;
    {
        std::lock_guard<std::mutex> lk(mutex);
        if(command->syncLaneId != 0){

        }

        if(hasSyncPacketMetadata(command)){
            markPacketQueued(command->syncLaneId,command->syncPacketId,command);
        }
        // Assign a globally monotonic sequence number so the priority queue
        // can preserve FIFO submission order as a final tie-breaker.
        const uint64_t seq = g_commandSequenceSeed.fetch_add(1,std::memory_order_relaxed);
        command->sequenceNumber = seq;
        if(command->type == CompositorCommand::Packet){
            auto packet = std::dynamic_pointer_cast<CompositorPacketCommand>(command);
            if(packet != nullptr){
                for(auto & child : packet->commands){
                    if(child != nullptr){
                        child->sequenceNumber = seq;
                    }
                }
            }
        }
        auto queuedCommand = command;
        commandQueue.push(std::move(command));
        noteQueuePushLocked(queuedCommand);
    }
    if(traceLaneId != 0 && tracePacketId != 0){
        emitSyncTrace(
                "queue lane=" + std::to_string(traceLaneId) +
                " packet=" + std::to_string(tracePacketId) +
                " cmdId=" + std::to_string(traceCommandId));
    }
    queueCondition.notify_one();
};

void Compositor::collectRenderTargetsForCommand(const SharedHandle<CompositorCommand> & command,
                                                OmegaCommon::Vector<RenderTargetEpoch> & targets){
    if(command == nullptr){
        return;
    }
    if(command->type == CompositorCommand::Render){
        auto render = std::dynamic_pointer_cast<CompositionRenderCommand>(command);
        if(render != nullptr && render->frame != nullptr && render->frame->targetLayer != nullptr){
            targets.push_back({render->renderTarget.get(),render->frame->targetLayer});
        }
        return;
    }
    if(command->type == CompositorCommand::Packet){
        auto packet = std::dynamic_pointer_cast<CompositorPacketCommand>(command);
        if(packet == nullptr){
            return;
        }
        for(auto & child : packet->commands){
            collectRenderTargetsForCommand(child,targets);
        }
    }
}

bool Compositor::targetsOverlap(const OmegaCommon::Vector<RenderTargetEpoch> & lhs,
                                const OmegaCommon::Vector<RenderTargetEpoch> & rhs){
    for(auto & leftTarget : lhs){
        for(auto & rightTarget : rhs){
            if(leftTarget.first == rightTarget.first &&
               leftTarget.second == rightTarget.second){
                return true;
            }
        }
    }
    return false;
}

unsigned Compositor::laneBudgetForNow(const LaneRuntimeState & laneState) const {
    if(!laneState.startupStabilized){
        return 1;
    }
    return kMaxFramesInFlightNormal;
}

bool Compositor::isLaneStartupCriticalPacket(std::uint64_t syncLaneId,std::uint64_t syncPacketId) const {
    if(syncLaneId == 0 || syncPacketId == 0 || packetTelemetryState == nullptr){
        return false;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto laneIt = packetTelemetryState->lanes.find(syncLaneId);
    if(laneIt == packetTelemetryState->lanes.end()){
        return false;
    }
    auto entryIt = laneIt->second.find(syncPacketId);
    if(entryIt == laneIt->second.end()){
        return false;
    }
    auto runtimeIt = packetTelemetryState->laneRuntime.find(syncLaneId);
    const bool startupStabilized = runtimeIt != packetTelemetryState->laneRuntime.end() &&
                                   runtimeIt->second.startupStabilized;
    if(startupStabilized){
        return false;
    }
    const auto & entry = entryIt->second;
    return entry.hasNonNoOpRender;
}

bool Compositor::waitForLaneAdmission(std::uint64_t syncLaneId,std::uint64_t syncPacketId){
    if(syncLaneId == 0 || syncPacketId == 0 || packetTelemetryState == nullptr){
        return true;
    }
    while(true){
        {
            std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
            auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
            auto & entry = packetTelemetryState->lanes[syncLaneId][syncPacketId];
            if(entry.pendingSubmissions > 0){
                return true;
            }
            const unsigned budget = laneBudgetForNow(laneState);
            if(laneState.inFlight < budget){
                return true;
            }
        }
        std::unique_lock<std::mutex> lk(mutex);
        queueCondition.wait_for(lk,std::chrono::milliseconds(1),[&]{
            return scheduler.shutdown;
        });
        if(scheduler.shutdown){
            return false;
        }
    }
}


bool Compositor::commandHasNonNoOpRender(const SharedHandle<CompositorCommand> & command) const {
    if(command == nullptr){
        return false;
    }
    if(command->type == CompositorCommand::Render){
        auto render = std::dynamic_pointer_cast<CompositionRenderCommand>(command);
        return render != nullptr &&
               render->frame != nullptr &&
               !isNoOpTransparentFrame(render->frame);
    }
    if(command->type == CompositorCommand::Packet){
        auto packet = std::dynamic_pointer_cast<CompositorPacketCommand>(command);
        if(packet == nullptr){
            return false;
        }
        for(auto & child : packet->commands){
            if(commandHasNonNoOpRender(child)){
                return true;
            }
        }
    }
    return false;
}

void Compositor::noteQueuePushLocked(const SharedHandle<CompositorCommand> & command){
    if(command == nullptr){
        return;
    }
    const auto idx = queueCommandTypeIndex(command->type);
    queueTelemetryState.queuedByType[idx] += 1;
    queueTelemetryState.enqueuedByType[idx] += 1;
    queueTelemetryState.eventsSinceEmit += 1;
    maybeEmitQueueSnapshotLocked("push",command->syncLaneId,command->syncPacketId);
}

void Compositor::noteQueuePopLocked(const SharedHandle<CompositorCommand> & command){
    if(command == nullptr){
        return;
    }
    const auto idx = queueCommandTypeIndex(command->type);
    if(queueTelemetryState.queuedByType[idx] > 0){
        queueTelemetryState.queuedByType[idx] -= 1;
    }
    queueTelemetryState.dequeuedByType[idx] += 1;
    queueTelemetryState.eventsSinceEmit += 1;
    maybeEmitQueueSnapshotLocked("pop",command->syncLaneId,command->syncPacketId);
}

void Compositor::noteQueueDropLocked(const SharedHandle<CompositorCommand> & command){
    if(command == nullptr){
        return;
    }
    const auto idx = queueCommandTypeIndex(command->type);
    if(queueTelemetryState.queuedByType[idx] > 0){
        queueTelemetryState.queuedByType[idx] -= 1;
    }
    queueTelemetryState.droppedByType[idx] += 1;
    queueTelemetryState.eventsSinceEmit += 1;
    maybeEmitQueueSnapshotLocked("drop",command->syncLaneId,command->syncPacketId);
}

void Compositor::maybeEmitQueueSnapshotLocked(const char *reason,
                                              std::uint64_t syncLaneId,
                                              std::uint64_t syncPacketId){
    if(!queueTraceEnabled()){
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool forceEmit = reason != nullptr &&
                           (std::string(reason) == "drop" ||
                            std::string(reason) == "shutdown");
    const bool intervalElapsed =
            queueTelemetryState.lastEmit == std::chrono::steady_clock::time_point{} ||
            (now - queueTelemetryState.lastEmit) >= std::chrono::milliseconds(200);
    if(!forceEmit && !intervalElapsed && queueTelemetryState.eventsSinceEmit < 64){
        return;
    }

    std::ostringstream ss;
    ss << "queue reason=" << (reason == nullptr ? "unknown" : reason)
       << " depth=" << commandQueue.length();
    if(syncLaneId != 0){
        ss << " lane=" << syncLaneId;
    }
    if(syncPacketId != 0){
        ss << " packet=" << syncPacketId;
    }
    ss << " queued{" << queueCountSummary(queueTelemetryState.queuedByType) << "}"
       << " enq{" << queueCountSummary(queueTelemetryState.enqueuedByType) << "}"
       << " deq{" << queueCountSummary(queueTelemetryState.dequeuedByType) << "}"
       << " drop{" << queueCountSummary(queueTelemetryState.droppedByType) << "}";
    emitQueueTrace(ss.str());
    queueTelemetryState.lastEmit = now;
    queueTelemetryState.eventsSinceEmit = 0;
}

void Compositor::dropQueuedStaleForLaneLocked(std::uint64_t syncLaneId,
                                              const SharedHandle<CompositorCommand> & incoming){
    if(syncLaneId == 0 || incoming == nullptr){
        return;
    }
    OmegaCommon::Vector<RenderTargetEpoch> incomingTargets {};
    collectRenderTargetsForCommand(incoming,incomingTargets);
    if(incomingTargets.empty()){
        return;
    }
    commandQueue.filter([&](SharedHandle<CompositorCommand> & queuedCommand){
        if(!isRenderLikeCommand(queuedCommand)){
            return false;
        }
        if(queuedCommand->syncLaneId != syncLaneId){
            return false;
        }
        if(isLaneStartupCriticalPacket(syncLaneId,queuedCommand->syncPacketId) ||
           isLaneStartupCriticalPacket(syncLaneId,incoming->syncPacketId)){
            return false;
        }
        OmegaCommon::Vector<RenderTargetEpoch> pendingTargets {};
        collectRenderTargetsForCommand(queuedCommand,pendingTargets);
        if(pendingTargets.empty() || !targetsOverlap(pendingTargets,incomingTargets)){
            return false;
        }
        // Target overlap — drop the older queued command.
        markPacketDropped(queuedCommand->syncLaneId,
                          queuedCommand->syncPacketId,
                          PacketDropReason::StaleCoalesced);
        queuedCommand->status.set(CommandStatus::Delayed);
        noteQueueDropLocked(queuedCommand);
        return true;
    });
}


// markLaneResizeActivity removed (Phase 3).

std::weak_ptr<Compositor::PacketTelemetryState> Compositor::telemetryState() const {
    return packetTelemetryState;
}

void Compositor::markPacketQueued(std::uint64_t syncLaneId,
                                  std::uint64_t syncPacketId,
                                  const SharedHandle<CompositorCommand> & command){
    if(syncLaneId == 0 || syncPacketId == 0 || packetTelemetryState == nullptr){
        return;
    }
    const bool hasNonNoOpRender = commandHasNonNoOpRender(command);

    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
    auto & entry = packetTelemetryState->lanes[syncLaneId][syncPacketId];
    if(entry.pendingSubmissions == 0){
        entry.phase = PacketLifecyclePhase::Queued;
    }
    if(entry.queuedTimeCpu == std::chrono::steady_clock::time_point{}){
        entry.queuedTimeCpu = std::chrono::steady_clock::now();
    }
    laneState.packetsQueued += 1;
    entry.hasNonNoOpRender = hasNonNoOpRender;
    entry.layerTreeMirrorApplied = true;
}

void Compositor::markPacketSubmitted(std::uint64_t syncLaneId,
                                     std::uint64_t syncPacketId,
                                     std::chrono::steady_clock::time_point submitTimeCpu){
    if(syncLaneId == 0 || syncPacketId == 0 || packetTelemetryState == nullptr){
        return;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
    auto & entry = packetTelemetryState->lanes[syncLaneId][syncPacketId];
    if(entry.queuedTimeCpu == std::chrono::steady_clock::time_point{}){
        entry.queuedTimeCpu = submitTimeCpu;
    }
    entry.phase = PacketLifecyclePhase::Submitted;
    if(entry.submitTimeCpu == std::chrono::steady_clock::time_point{} ||
       submitTimeCpu < entry.submitTimeCpu){
        entry.submitTimeCpu = submitTimeCpu;
    }
    if(entry.pendingSubmissions == 0){
        laneState.inFlight += 1;
        laneState.packetsSubmitted += 1;
    }
    entry.pendingSubmissions += 1;
    emitSyncTrace(
            "submit lane=" + std::to_string(syncLaneId) +
            " packet=" + std::to_string(syncPacketId) +
            " inFlight=" + std::to_string(laneState.inFlight));
}


void Compositor::markPacketDropped(std::uint64_t syncLaneId,
                                   std::uint64_t syncPacketId,
                                   PacketDropReason reason){
    if(syncLaneId == 0 || syncPacketId == 0 || packetTelemetryState == nullptr){
        return;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
    auto & entry = packetTelemetryState->lanes[syncLaneId][syncPacketId];
    const auto now = std::chrono::steady_clock::now();
    if(entry.queuedTimeCpu == std::chrono::steady_clock::time_point{}){
        entry.queuedTimeCpu = now;
    }
    if(entry.pendingSubmissions > 0){
        entry.pendingSubmissions = 0;
        if(laneState.inFlight > 0){
            laneState.inFlight -= 1;
        }
        if(packetTelemetryState->wakeCondition != nullptr){
            packetTelemetryState->wakeCondition->notify_all();
        }
    }
    if(entry.phase != PacketLifecyclePhase::Dropped){
        laneState.packetsDropped += 1;
    }
    entry.phase = PacketLifecyclePhase::Dropped;
    entry.backendStatus = BackendSubmissionStatus::Dropped;
    entry.presentTimeCpu = now;

    if(reason == PacketDropReason::StaleCoalesced){
    }
    else if(reason == PacketDropReason::NoOpTransparent){
    }
    else if(reason == PacketDropReason::EpochSuperseded){
    }
    emitSyncTrace(
            "drop lane=" + std::to_string(syncLaneId) +
            " packet=" + std::to_string(syncPacketId) +
            " reason=" + std::to_string(static_cast<int>(reason)));
}

void Compositor::markPacketFailed(std::uint64_t syncLaneId,std::uint64_t syncPacketId){
    if(syncLaneId == 0 || syncPacketId == 0 || packetTelemetryState == nullptr){
        return;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
    auto & entry = packetTelemetryState->lanes[syncLaneId][syncPacketId];
    const auto now = std::chrono::steady_clock::now();
    if(entry.queuedTimeCpu == std::chrono::steady_clock::time_point{}){
        entry.queuedTimeCpu = now;
    }
    if(entry.pendingSubmissions > 0){
        entry.pendingSubmissions = 0;
        if(laneState.inFlight > 0){
            laneState.inFlight -= 1;
        }
        if(packetTelemetryState->wakeCondition != nullptr){
            packetTelemetryState->wakeCondition->notify_all();
        }
    }
    if(entry.phase != PacketLifecyclePhase::Failed){
        laneState.packetsFailed += 1;
    }
    entry.phase = PacketLifecyclePhase::Failed;
    entry.backendStatus = BackendSubmissionStatus::Error;
    entry.gpuCompleteTimeCpu = now;
    entry.presentTimeCpu = now;
    emitSyncTrace(
            "fail lane=" + std::to_string(syncLaneId) +
            " packet=" + std::to_string(syncPacketId));
}

bool Compositor::shouldDropNoOpTransparentFrame(std::uint64_t syncLaneId,std::uint64_t syncPacketId) const {
    if(syncLaneId == 0 || syncPacketId == 0 || packetTelemetryState == nullptr){
        return false;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto runtimeIt = packetTelemetryState->laneRuntime.find(syncLaneId);
    if(runtimeIt == packetTelemetryState->laneRuntime.end()){
        return false;
    }
    const auto & laneState = runtimeIt->second;
    auto laneIt = packetTelemetryState->lanes.find(syncLaneId);
    if(laneIt == packetTelemetryState->lanes.end()){
        return false;
    }
    auto entryIt = laneIt->second.find(syncPacketId);
    if(entryIt == laneIt->second.end()){
        return false;
    }
    const auto & entry = entryIt->second;
    // Do not skip no-op frames until mirror state has been materialized.
    if(!entry.layerTreeMirrorApplied){
        return false;
    }
    // Keep startup deterministic: wait until at least one non-no-op packet
    // has actually presented for this lane before dropping transparent no-op packets.
    if(!laneState.startupStabilized || !laneState.hasPresentedRenderableContent){
        return false;
    }
    // If packet has a real render, no-op children can be dropped safely.
    // Keep no-op renders only for state/effect-only packets that still need
    // lifecycle completion for synchronization.
    if(entry.hasNonNoOpRender){
        return true;
    }
    return false;
}

void Compositor::completePacketWithoutGpu(std::uint64_t syncLaneId,std::uint64_t syncPacketId){
    if(syncLaneId == 0 || syncPacketId == 0){
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    markPacketSubmitted(syncLaneId,syncPacketId,now);
    BackendSubmissionTelemetry telemetry {};
    telemetry.syncLaneId = syncLaneId;
    telemetry.syncPacketId = syncPacketId;
    telemetry.submitTimeCpu = now;
    telemetry.completeTimeCpu = now;
    telemetry.presentTimeCpu = now;
    telemetry.status = BackendSubmissionStatus::Completed;
    onBackendSubmissionCompleted(telemetryState(),telemetry);
}

void Compositor::onBackendSubmissionCompleted(std::weak_ptr<PacketTelemetryState> weakState,
                                              const BackendSubmissionTelemetry & telemetry){
    auto state = weakState.lock();
    if(state == nullptr || telemetry.syncLaneId == 0 || telemetry.syncPacketId == 0){
        return;
    }
    std::lock_guard<std::mutex> lk(state->mutex);
    auto & laneState = state->laneRuntime[telemetry.syncLaneId];
    auto & entry = state->lanes[telemetry.syncLaneId][telemetry.syncPacketId];
    if(entry.queuedTimeCpu == std::chrono::steady_clock::time_point{}){
        entry.queuedTimeCpu = telemetry.submitTimeCpu;
    }
    if(entry.submitTimeCpu == std::chrono::steady_clock::time_point{} ||
       telemetry.submitTimeCpu < entry.submitTimeCpu){
        entry.submitTimeCpu = telemetry.submitTimeCpu;
    }
    entry.gpuCompleteTimeCpu = telemetry.completeTimeCpu;
    entry.gpuStartTimeSec = telemetry.gpuStartTimeSec;
    entry.gpuEndTimeSec = telemetry.gpuEndTimeSec;
    entry.backendStatus = telemetry.status;
    if(telemetry.status == BackendSubmissionStatus::Completed){
        const bool hadGpuCompleted = entry.phase == PacketLifecyclePhase::GPUCompleted ||
                                     entry.phase == PacketLifecyclePhase::Presented;
        if(entry.pendingSubmissions > 0){
            entry.pendingSubmissions -= 1;
        }
        entry.phase = PacketLifecyclePhase::GPUCompleted;
        if(!hadGpuCompleted){
            laneState.packetsGPUCompleted += 1;
        }
        if(entry.pendingSubmissions == 0){
            if(laneState.inFlight > 0){
                laneState.inFlight -= 1;
            }
            entry.phase = PacketLifecyclePhase::Presented;
            entry.presentTimeCpu = telemetry.presentTimeCpu == std::chrono::steady_clock::time_point{}
                                   ? telemetry.completeTimeCpu
                                   : telemetry.presentTimeCpu;
            laneState.packetsPresented += 1;
            if(!laneState.startupStabilized){
                laneState.startupStabilized = true;
                laneState.firstPresentedPacketId = telemetry.syncPacketId;
            }
            if(entry.hasNonNoOpRender){
                laneState.hasPresentedRenderableContent = true;
            }

            // EWMA/pressure/quality updates removed (Phase 3).

            if(state->wakeCondition != nullptr){
                state->wakeCondition->notify_all();
            }
        }
    }
    else {
        if(entry.pendingSubmissions > 0){
            entry.pendingSubmissions = 0;
            if(laneState.inFlight > 0){
                laneState.inFlight -= 1;
            }
        }
        if(entry.phase != PacketLifecyclePhase::Failed){
            laneState.packetsFailed += 1;
        }
        entry.phase = PacketLifecyclePhase::Failed;
        entry.presentTimeCpu = telemetry.completeTimeCpu;
        emitSyncTrace(
                "backend-fail lane=" + std::to_string(telemetry.syncLaneId) +
                " packet=" + std::to_string(telemetry.syncPacketId));
        if(state->wakeCondition != nullptr){
            state->wakeCondition->notify_all();
        }
    }
}

Compositor::LaneTelemetrySnapshot Compositor::getLaneTelemetrySnapshot(std::uint64_t syncLaneId) const{
    LaneTelemetrySnapshot snapshot {};
    snapshot.syncLaneId = syncLaneId;
    if(syncLaneId == 0 || packetTelemetryState == nullptr){
        return snapshot;
    }

    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    const auto now = std::chrono::steady_clock::now();

    auto runtimeIt = packetTelemetryState->laneRuntime.find(syncLaneId);
    if(runtimeIt != packetTelemetryState->laneRuntime.end()){
        snapshot.inFlight = runtimeIt->second.inFlight;
        snapshot.startupStabilized = runtimeIt->second.startupStabilized;
        snapshot.firstPresentedPacketId = runtimeIt->second.firstPresentedPacketId;
        snapshot.droppedPacketCount = runtimeIt->second.packetsDropped;
        snapshot.failedPacketCount = runtimeIt->second.packetsFailed;
    }

    auto laneIt = packetTelemetryState->lanes.find(syncLaneId);
    if(laneIt == packetTelemetryState->lanes.end()){
        return snapshot;
    }

    for(auto & packetEntry : laneIt->second){
        const auto packetId = packetEntry.first;
        const auto & record = packetEntry.second;

        if(record.submitTimeCpu != std::chrono::steady_clock::time_point{}){
            snapshot.lastSubmittedPacketId = std::max(snapshot.lastSubmittedPacketId,packetId);
        }

        if(record.phase == PacketLifecyclePhase::GPUCompleted ||
           record.phase == PacketLifecyclePhase::Presented){
            snapshot.lastPresentedPacketId = std::max(snapshot.lastPresentedPacketId,packetId);
        }

    }

    return snapshot;
}

// getLayerTreeSyncSnapshot removed (Phase 2).

Compositor::LaneDiagnosticsSnapshot Compositor::getLaneDiagnosticsSnapshot(std::uint64_t syncLaneId) const {
    LaneDiagnosticsSnapshot snapshot {};
    snapshot.syncLaneId = syncLaneId;
    if(syncLaneId == 0 || packetTelemetryState == nullptr){
        return snapshot;
    }

    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    const auto now = std::chrono::steady_clock::now();

    auto runtimeIt = packetTelemetryState->laneRuntime.find(syncLaneId);
    if(runtimeIt != packetTelemetryState->laneRuntime.end()){
        const auto & laneState = runtimeIt->second;
        snapshot.firstPresentedPacketId = laneState.firstPresentedPacketId;
        snapshot.queuedPacketCount = laneState.packetsQueued;
        snapshot.submittedPacketCount = laneState.packetsSubmitted;
        snapshot.presentedPacketCount = laneState.packetsPresented;
        snapshot.droppedPacketCount = laneState.packetsDropped;
        snapshot.failedPacketCount = laneState.packetsFailed;
        snapshot.inFlight = laneState.inFlight;
        snapshot.startupStabilized = laneState.startupStabilized;
    }

    auto laneIt = packetTelemetryState->lanes.find(syncLaneId);
    if(laneIt == packetTelemetryState->lanes.end()){
        return snapshot;
    }
    for(auto & packetEntry : laneIt->second){
        const auto packetId = packetEntry.first;
        const auto & record = packetEntry.second;
        if(record.submitTimeCpu != std::chrono::steady_clock::time_point{}){
            snapshot.lastSubmittedPacketId = std::max(snapshot.lastSubmittedPacketId,packetId);
        }
        if(record.phase == PacketLifecyclePhase::GPUCompleted ||
           record.phase == PacketLifecyclePhase::Presented){
            snapshot.lastPresentedPacketId = std::max(snapshot.lastPresentedPacketId,packetId);
        }
    }
    return snapshot;
}

OmegaCommon::String Compositor::dumpLaneDiagnostics(std::uint64_t syncLaneId) const {
    auto snapshot = getLaneDiagnosticsSnapshot(syncLaneId);
    std::ostringstream ss;
    ss << "Lane " << snapshot.syncLaneId
       << " {startup=" << (snapshot.startupStabilized ? "yes" : "no")
       << ", inFlight=" << snapshot.inFlight
       << ", firstPresented=" << snapshot.firstPresentedPacketId
       << ", lastSubmitted=" << snapshot.lastSubmittedPacketId
       << ", lastPresented=" << snapshot.lastPresentedPacketId
       << ", queued=" << snapshot.queuedPacketCount
       << ", submitted=" << snapshot.submittedPacketCount
       << ", presented=" << snapshot.presentedPacketCount
       << ", dropped=" << snapshot.droppedPacketCount
       << ", failed=" << snapshot.failedPacketCount
       << "}";
    return ss.str();
}


};
