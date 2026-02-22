#include "Compositor.h"
#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/Canvas.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>

#if defined(TARGET_MACOS)
#include <dispatch/dispatch.h>
#include <pthread.h>
#endif

namespace OmegaWTK::Composition {

namespace {
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
}

//void Compositor::hasDetached(LayerTree *tree){
//    for(auto it = targetLayerTrees.begin();it != targetLayerTrees.end();it++){
//        if(tree == *it){
//            targetLayerTrees.erase(it);
//            renderTargetStore.cleanTreeTargets(tree);
//            tree->removeObserver(this);
//            break;
//        }
//    }
//};

void CompositorScheduler::processCommand(SharedHandle<CompositorCommand> & command,bool laneAdmissionBypassed){
    if(hasSyncPacketMetadata(command) && !laneAdmissionBypassed){
        if(!compositor->waitForLaneAdmission(command->syncLaneId,command->syncPacketId)){
            compositor->markPacketFailed(command->syncLaneId,command->syncPacketId);
            command->status.set(CommandStatus::Failed);
            return;
        }
    }
    if(command != nullptr && command->type == CompositorCommand::Packet){
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
#if defined(TARGET_MACOS)
        if(pthread_main_np() != 0){
            compositor->executeCurrentCommand();
        }
        else {
            dispatch_sync_f(dispatch_get_main_queue(),compositor,[](void *ctx){
                ((Compositor *)ctx)->executeCurrentCommand();
            });
        }
#else
        compositor->executeCurrentCommand();
#endif
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
                        compositor->markPacketFailed(pending->syncLaneId,pending->syncPacketId);
                        pending->status.set(CommandStatus::Failed);
                    }
                    break;
                }
                command = compositor->commandQueue.first();
                compositor->commandQueue.pop();
                compositor->currentCommand = command;
            }
            processCommand(command);
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
    if(t.joinable() && t.get_id() != std::this_thread::get_id()){
        t.join();
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
     scheduler.shutdownAndJoin();
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
        if(command->syncLaneId != 0 && commandContainsResizeActivity(command)){
            markLaneResizeActivity(command->syncLaneId);
        }

        if(hasSyncPacketMetadata(command)){
            markPacketQueued(command->syncLaneId,command->syncPacketId,command);
            if(commandHasNonNoOpRender(command)){
                // Base target-aware coalescing.
                dropQueuedStaleForLaneLocked(command->syncLaneId,command);
                // Saturated lanes keep newest packet for overlapping targets.
                if(isLaneSaturated(command->syncLaneId)){
                    dropQueuedStaleForLaneLocked(command->syncLaneId,command);
                }
            }
        }
        commandQueue.push(std::move(command));
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

unsigned Compositor::laneBudgetForNow(const LaneRuntimeState & laneState,
                                      std::chrono::steady_clock::time_point now) const {
    unsigned budget = now < laneState.resizeModeUntil
                      ? kMaxFramesInFlightResize
                      : kMaxFramesInFlightNormal;
    if(!laneState.startupStabilized){
        budget = std::min(budget,1u);
    }
    return std::max(1u,budget);
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
    return entry.hasStateMutation || entry.hasEffectMutation;
}

bool Compositor::isLaneSaturated(std::uint64_t syncLaneId) const {
    if(syncLaneId == 0 || packetTelemetryState == nullptr){
        return false;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
    const auto now = std::chrono::steady_clock::now();
    const unsigned budget = laneBudgetForNow(laneState,now);
    return laneState.inFlight >= budget;
}

bool Compositor::waitForLaneAdmission(std::uint64_t syncLaneId,std::uint64_t syncPacketId){
    if(syncLaneId == 0 || syncPacketId == 0 || packetTelemetryState == nullptr){
        return true;
    }
    const auto waitStart = std::chrono::steady_clock::now();
    bool waited = false;
    bool saturationRecorded = false;
    bool startupHoldRecorded = false;

    while(true){
        {
            std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
            auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
            auto & entry = packetTelemetryState->lanes[syncLaneId][syncPacketId];
            if(entry.pendingSubmissions > 0){
                if(waited){
                    laneState.admissionWaitCount += 1;
                    laneState.totalAdmissionWait += std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - waitStart);
                    emitSyncTrace(
                            "admit lane=" + std::to_string(syncLaneId) +
                            " packet=" + std::to_string(syncPacketId) +
                            " waitedUs=" + std::to_string(
                                    std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - waitStart).count()));
                }
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            const unsigned budget = laneBudgetForNow(laneState,now);
            if(laneState.inFlight < budget){
                if(waited){
                    laneState.admissionWaitCount += 1;
                    laneState.totalAdmissionWait += std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - waitStart);
                    emitSyncTrace(
                            "admit lane=" + std::to_string(syncLaneId) +
                            " packet=" + std::to_string(syncPacketId) +
                            " waitedUs=" + std::to_string(
                                    std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - waitStart).count()));
                }
                return true;
            }
            waited = true;
            if(!saturationRecorded){
                laneState.saturationCount += 1;
                saturationRecorded = true;
                emitSyncTrace(
                        "wait lane=" + std::to_string(syncLaneId) +
                        " packet=" + std::to_string(syncPacketId) +
                        " inFlight=" + std::to_string(laneState.inFlight) +
                        " budget=" + std::to_string(budget));
            }
            if(!laneState.startupStabilized && !startupHoldRecorded){
                laneState.startupAdmissionHoldCount += 1;
                startupHoldRecorded = true;
            }
        }
        std::unique_lock<std::mutex> lk(mutex);
        queueCondition.wait_for(lk,std::chrono::milliseconds(1),[&]{
            return scheduler.shutdown;
        });
        if(scheduler.shutdown){
            if(waited){
                std::lock_guard<std::mutex> telemetryLock(packetTelemetryState->mutex);
                auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
                laneState.admissionWaitCount += 1;
                laneState.totalAdmissionWait += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - waitStart);
            }
            return false;
        }
    }
}

bool Compositor::commandContainsResizeActivity(const SharedHandle<CompositorCommand> & command) const {
    if(command == nullptr){
        return false;
    }
    if(command->type == CompositorCommand::View){
        auto viewCommand = std::dynamic_pointer_cast<CompositorViewCommand>(command);
        return viewCommand != nullptr &&
               viewCommand->subType == CompositorViewCommand::Resize;
    }
    if(command->type == CompositorCommand::Layer){
        auto layerCommand = std::dynamic_pointer_cast<CompositorLayerCommand>(command);
        return layerCommand != nullptr &&
               layerCommand->subtype == CompositorLayerCommand::Resize;
    }
    if(command->type == CompositorCommand::Packet){
        auto packet = std::dynamic_pointer_cast<CompositorPacketCommand>(command);
        if(packet == nullptr){
            return false;
        }
        for(auto & child : packet->commands){
            if(commandContainsResizeActivity(child)){
                return true;
            }
        }
    }
    return false;
}

bool Compositor::commandContainsStateMutation(const SharedHandle<CompositorCommand> & command) const {
    return commandContainsStateMutationRecursive(command);
}

bool Compositor::commandContainsEffectMutation(const SharedHandle<CompositorCommand> & command) const {
    return commandContainsEffectMutationRecursive(command);
}

bool Compositor::commandContainsRenderCommand(const SharedHandle<CompositorCommand> & command) const {
    return commandContainsRenderCommandRecursive(command);
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
        // Never coalesce away packets that carry state mutations.
        if(commandContainsStateMutation(queuedCommand)){
            return false;
        }
        OmegaCommon::Vector<RenderTargetEpoch> pendingTargets {};
        collectRenderTargetsForCommand(queuedCommand,pendingTargets);
        if(pendingTargets.empty() || !targetsOverlap(pendingTargets,incomingTargets)){
            return false;
        }
        if(queuedCommand->type == CompositorCommand::Packet){
            auto pendingPacket = std::dynamic_pointer_cast<CompositorPacketCommand>(queuedCommand);
            if(pendingPacket != nullptr){
                for(auto & child : pendingPacket->commands){
                    if(child != nullptr){
                        child->status.set(CommandStatus::Delayed);
                    }
                }
            }
            markPacketDropped(queuedCommand->syncLaneId,
                              queuedCommand->syncPacketId,
                              PacketDropReason::StaleCoalesced);
            queuedCommand->status.set(CommandStatus::Delayed);
            return true;
        }
        if(queuedCommand->type == CompositorCommand::Render){
            markPacketDropped(queuedCommand->syncLaneId,
                              queuedCommand->syncPacketId,
                              PacketDropReason::StaleCoalesced);
            queuedCommand->status.set(CommandStatus::Delayed);
            return true;
        }
        return false;
    });
}

void Compositor::markLaneResizeActivity(std::uint64_t syncLaneId){
    if(syncLaneId == 0 || packetTelemetryState == nullptr){
        return;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
    laneState.resizeModeUntil = std::chrono::steady_clock::now() + kResizeModeHoldWindow;
}

std::weak_ptr<Compositor::PacketTelemetryState> Compositor::telemetryState() const {
    return packetTelemetryState;
}

void Compositor::markPacketQueued(std::uint64_t syncLaneId,
                                  std::uint64_t syncPacketId,
                                  const SharedHandle<CompositorCommand> & command){
    if(syncLaneId == 0 || syncPacketId == 0 || packetTelemetryState == nullptr){
        return;
    }
    const bool hasRenderCommand = commandContainsRenderCommand(command);
    const bool hasNonNoOpRender = commandHasNonNoOpRender(command);
    const bool hasStateMutation = commandContainsStateMutation(command);
    const bool hasEffectMutation = commandContainsEffectMutation(command);
    const bool hasResizeMutation = commandContainsResizeActivity(command);

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
    entry.hasRenderCommand = hasRenderCommand;
    entry.hasNonNoOpRender = hasNonNoOpRender;
    entry.hasStateMutation = hasStateMutation;
    entry.hasEffectMutation = hasEffectMutation;
    entry.hasResizeMutation = hasResizeMutation;
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
        laneState.maxInFlightObserved = std::max(laneState.maxInFlightObserved,laneState.inFlight);
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
        laneState.staleCoalescedCount += 1;
    }
    else if(reason == PacketDropReason::NoOpTransparent){
        laneState.noOpTransparentDropCount += 1;
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
        return true;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto laneIt = packetTelemetryState->lanes.find(syncLaneId);
    if(laneIt == packetTelemetryState->lanes.end()){
        return true;
    }
    auto entryIt = laneIt->second.find(syncPacketId);
    if(entryIt == laneIt->second.end()){
        return true;
    }
    const auto & entry = entryIt->second;
    return !(entry.hasStateMutation || entry.hasEffectMutation);
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

bool Compositor::isLaneUnderPressure(std::uint64_t syncLaneId) const {
    if(syncLaneId == 0 || packetTelemetryState == nullptr){
        return false;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto runtimeIt = packetTelemetryState->laneRuntime.find(syncLaneId);
    if(runtimeIt == packetTelemetryState->laneRuntime.end()){
        return false;
    }
    return runtimeIt->second.underPressure;
}

LayerEffect::DropShadowParams Compositor::adaptDropShadowForLane(std::uint64_t syncLaneId,
                                                                  const LayerEffect::DropShadowParams & params) const {
    LayerEffect::DropShadowParams clamped = params;
    if(syncLaneId == 0 || packetTelemetryState == nullptr){
        return clamped;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto runtimeIt = packetTelemetryState->laneRuntime.find(syncLaneId);
    if(runtimeIt == packetTelemetryState->laneRuntime.end()){
        return clamped;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool resizeBudget = now < runtimeIt->second.resizeModeUntil;
    const bool pressure = runtimeIt->second.underPressure;
    if(!resizeBudget && !pressure){
        return clamped;
    }
    clamped.radius = std::max(0.f,std::min(
            clamped.radius,
            resizeBudget ? kAdaptiveShadowRadiusResize : kAdaptiveShadowRadiusPressure));
    clamped.blurAmount = std::max(0.f,std::min(
            clamped.blurAmount,
            resizeBudget ? kAdaptiveShadowBlurResize : kAdaptiveShadowBlurPressure));
    clamped.opacity = std::max(0.f,std::min(clamped.opacity,1.f));
    return clamped;
}

CanvasEffect Compositor::adaptCanvasEffectForLane(std::uint64_t syncLaneId,
                                                  const CanvasEffect & effect) const {
    CanvasEffect clamped = effect;
    if(syncLaneId == 0 || packetTelemetryState == nullptr){
        return clamped;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto runtimeIt = packetTelemetryState->laneRuntime.find(syncLaneId);
    if(runtimeIt == packetTelemetryState->laneRuntime.end()){
        return clamped;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool resizeBudget = now < runtimeIt->second.resizeModeUntil;
    const bool pressure = runtimeIt->second.underPressure;
    if(!resizeBudget && !pressure){
        return clamped;
    }
    const float cap = resizeBudget ? kAdaptiveCanvasBlurResize : kAdaptiveCanvasBlurPressure;
    if(clamped.type == CanvasEffect::DirectionalBlur){
        clamped.directionalBlur.radius = std::max(0.f,std::min(clamped.directionalBlur.radius,cap));
    }
    else if(clamped.type == CanvasEffect::GaussianBlur){
        clamped.gaussianBlur.radius = std::max(0.f,std::min(clamped.gaussianBlur.radius,cap));
    }
    return clamped;
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

            if(entry.submitTimeCpu != std::chrono::steady_clock::time_point{} &&
               entry.presentTimeCpu != std::chrono::steady_clock::time_point{}){
                const double submitToPresentMs = durationToMs(entry.presentTimeCpu - entry.submitTimeCpu);
                if(!laneState.hasSubmitToPresentSample){
                    laneState.submitToPresentEwmaMs = submitToPresentMs;
                    laneState.hasSubmitToPresentSample = true;
                }
                else {
                    laneState.submitToPresentEwmaMs =
                            ((1.0 - kLatencyEwmaAlpha) * laneState.submitToPresentEwmaMs) +
                            (kLatencyEwmaAlpha * submitToPresentMs);
                }
            }

            if(entry.gpuEndTimeSec > entry.gpuStartTimeSec){
                const double gpuDurationMs = (entry.gpuEndTimeSec - entry.gpuStartTimeSec) * 1000.0;
                if(!laneState.hasGpuDurationSample){
                    laneState.gpuDurationEwmaMs = gpuDurationMs;
                    laneState.hasGpuDurationSample = true;
                }
                else {
                    laneState.gpuDurationEwmaMs =
                            ((1.0 - kGpuEwmaAlpha) * laneState.gpuDurationEwmaMs) +
                            (kGpuEwmaAlpha * gpuDurationMs);
                }
            }

            const auto now = std::chrono::steady_clock::now();
            const bool resizeBudget = now < laneState.resizeModeUntil;
            const double enterThreshold = resizeBudget ? kPressureEnterLatencyMsResize : kPressureEnterLatencyMsNormal;
            const double exitThreshold = resizeBudget ? kPressureExitLatencyMsResize : kPressureExitLatencyMsNormal;
            if(laneState.hasSubmitToPresentSample){
                if(!laneState.underPressure && laneState.submitToPresentEwmaMs >= enterThreshold){
                    laneState.underPressure = true;
                }
                else if(laneState.underPressure && laneState.submitToPresentEwmaMs <= exitThreshold){
                    laneState.underPressure = false;
                }
            }

            emitSyncTrace(
                    "present lane=" + std::to_string(telemetry.syncLaneId) +
                    " packet=" + std::to_string(telemetry.syncPacketId) +
                    " inFlight=" + std::to_string(laneState.inFlight) +
                    " ewmaMs=" + std::to_string(laneState.submitToPresentEwmaMs));
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
        laneState.underPressure = true;
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
        snapshot.resizeBudgetActive = now < runtimeIt->second.resizeModeUntil;
        snapshot.startupStabilized = runtimeIt->second.startupStabilized;
        snapshot.firstPresentedPacketId = runtimeIt->second.firstPresentedPacketId;
        snapshot.underPressure = runtimeIt->second.underPressure;
        snapshot.submitToPresentEwmaMs = runtimeIt->second.submitToPresentEwmaMs;
        snapshot.gpuDurationEwmaMs = runtimeIt->second.gpuDurationEwmaMs;
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

        if(record.presentTimeCpu != std::chrono::steady_clock::time_point{} &&
           record.presentTimeCpu > snapshot.lastPresentedTimeCpu){
            snapshot.lastPresentedTimeCpu = record.presentTimeCpu;
        }
    }

    return snapshot;
}

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
        snapshot.gpuCompletedPacketCount = laneState.packetsGPUCompleted;
        snapshot.presentedPacketCount = laneState.packetsPresented;
        snapshot.droppedPacketCount = laneState.packetsDropped;
        snapshot.failedPacketCount = laneState.packetsFailed;
        snapshot.staleCoalescedPacketCount = laneState.staleCoalescedCount;
        snapshot.noOpTransparentDropCount = laneState.noOpTransparentDropCount;
        snapshot.saturationCount = laneState.saturationCount;
        snapshot.startupAdmissionHoldCount = laneState.startupAdmissionHoldCount;
        snapshot.admissionWaitCount = laneState.admissionWaitCount;
        snapshot.admissionWaitTotalMs = std::chrono::duration<double,std::milli>(laneState.totalAdmissionWait).count();
        snapshot.inFlight = laneState.inFlight;
        snapshot.maxInFlightObserved = laneState.maxInFlightObserved;
        snapshot.resizeBudgetActive = now < laneState.resizeModeUntil;
        snapshot.startupStabilized = laneState.startupStabilized;
        snapshot.underPressure = laneState.underPressure;
        snapshot.submitToPresentEwmaMs = laneState.submitToPresentEwmaMs;
        snapshot.gpuDurationEwmaMs = laneState.gpuDurationEwmaMs;
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
        if(record.presentTimeCpu != std::chrono::steady_clock::time_point{} &&
           record.presentTimeCpu > snapshot.lastPresentedTimeCpu){
            snapshot.lastPresentedTimeCpu = record.presentTimeCpu;
        }
    }
    return snapshot;
}

OmegaCommon::String Compositor::dumpLaneDiagnostics(std::uint64_t syncLaneId) const {
    auto snapshot = getLaneDiagnosticsSnapshot(syncLaneId);
    std::ostringstream ss;
    ss << "Lane " << snapshot.syncLaneId
       << " {startup=" << (snapshot.startupStabilized ? "yes" : "no")
       << ", pressure=" << (snapshot.underPressure ? "yes" : "no")
       << ", resizeBudget=" << (snapshot.resizeBudgetActive ? "yes" : "no")
       << ", inFlight=" << snapshot.inFlight
       << ", maxInFlight=" << snapshot.maxInFlightObserved
       << ", firstPresented=" << snapshot.firstPresentedPacketId
       << ", lastSubmitted=" << snapshot.lastSubmittedPacketId
       << ", lastPresented=" << snapshot.lastPresentedPacketId
       << ", queued=" << snapshot.queuedPacketCount
       << ", submitted=" << snapshot.submittedPacketCount
       << ", gpuCompleted=" << snapshot.gpuCompletedPacketCount
       << ", presented=" << snapshot.presentedPacketCount
       << ", dropped=" << snapshot.droppedPacketCount
       << ", failed=" << snapshot.failedPacketCount
       << ", staleCoalesced=" << snapshot.staleCoalescedPacketCount
       << ", noOpDrops=" << snapshot.noOpTransparentDropCount
       << ", saturation=" << snapshot.saturationCount
       << ", startupHolds=" << snapshot.startupAdmissionHoldCount
       << ", waitCount=" << snapshot.admissionWaitCount
       << ", waitMs=" << snapshot.admissionWaitTotalMs
       << ", ewmaSubmitPresentMs=" << snapshot.submitToPresentEwmaMs
       << ", ewmaGpuMs=" << snapshot.gpuDurationEwmaMs
       << "}";
    return ss.str();
}


};
