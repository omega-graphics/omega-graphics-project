#include "Compositor.h"
#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/Canvas.h"
#include <algorithm>
#include <array>
#include <cassert>
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

    static inline const char *resizeGovernorPhaseName(ResizeGovernorPhase phase){
        switch(phase){
            case ResizeGovernorPhase::Idle:
                return "Idle";
            case ResizeGovernorPhase::Active:
                return "Active";
            case ResizeGovernorPhase::Settling:
                return "Settling";
            case ResizeGovernorPhase::Completed:
                return "Completed";
            default:
                return "Unknown";
        }
    }

    static inline const char *laneEffectQualityName(LaneEffectQuality quality){
        switch(quality){
            case LaneEffectQuality::Full:
                return "Full";
            case LaneEffectQuality::Reduced:
                return "Reduced";
            case LaneEffectQuality::Minimal:
                return "Minimal";
            default:
                return "Unknown";
        }
    }

    static inline float finiteAbs(float value){
        if(!std::isfinite(value)){
            return 0.f;
        }
        return std::fabs(value);
    }

    static inline bool resizeGovernorIndicatesActive(const ResizeGovernorMetadata & metadata){
        return metadata.active ||
               metadata.phase == ResizeGovernorPhase::Active ||
               metadata.phase == ResizeGovernorPhase::Settling;
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

    static void collectLayersForTreeLimb(LayerTree *tree,
                                         LayerTree::Limb *limb,
                                         OmegaCommon::Vector<Layer *> &layers){
        if(tree == nullptr || limb == nullptr){
            return;
        }
        auto &rootLayer = limb->getRootLayer();
        if(rootLayer != nullptr){
            layers.push_back(rootLayer.get());
        }
        for(auto it = limb->begin(); it != limb->end(); ++it){
            if(*it != nullptr){
                layers.push_back((*it).get());
            }
        }
        const auto childCount = tree->getParentLimbChildCount(limb);
        for(unsigned idx = 0; idx < childCount; ++idx){
            collectLayersForTreeLimb(tree,tree->getLimbAtIndexFromParent(idx,limb),layers);
        }
    }

}

Compositor::GovernorTuningConfig Compositor::loadGovernorTuningConfig(){
    GovernorTuningConfig cfg {};
    cfg.pressureEnterLatencyMsResize = readEnvDoubleClamp(
            "OMEGAWTK_RESIZE_GOV_PRESSURE_ENTER_MS_RESIZE",
            cfg.pressureEnterLatencyMsResize,
            1.0,
            120.0);
    cfg.pressureExitLatencyMsResize = readEnvDoubleClamp(
            "OMEGAWTK_RESIZE_GOV_PRESSURE_EXIT_MS_RESIZE",
            cfg.pressureExitLatencyMsResize,
            1.0,
            120.0);
    cfg.pressureEnterLatencyMsNormal = readEnvDoubleClamp(
            "OMEGAWTK_RESIZE_GOV_PRESSURE_ENTER_MS_NORMAL",
            cfg.pressureEnterLatencyMsNormal,
            1.0,
            200.0);
    cfg.pressureExitLatencyMsNormal = readEnvDoubleClamp(
            "OMEGAWTK_RESIZE_GOV_PRESSURE_EXIT_MS_NORMAL",
            cfg.pressureExitLatencyMsNormal,
            1.0,
            200.0);
    cfg.latencyEwmaAlpha = readEnvDoubleClamp(
            "OMEGAWTK_RESIZE_GOV_EWMA_ALPHA_LATENCY",
            cfg.latencyEwmaAlpha,
            0.01,
            1.0);
    cfg.gpuEwmaAlpha = readEnvDoubleClamp(
            "OMEGAWTK_RESIZE_GOV_EWMA_ALPHA_GPU",
            cfg.gpuEwmaAlpha,
            0.01,
            1.0);
    cfg.velocityBudgetRelaxPxPerSec = readEnvFloatClamp(
            "OMEGAWTK_RESIZE_GOV_VELOCITY_RELAX",
            cfg.velocityBudgetRelaxPxPerSec,
            1.f,
            10000.f);
    cfg.velocityBudgetTightenPxPerSec = readEnvFloatClamp(
            "OMEGAWTK_RESIZE_GOV_VELOCITY_TIGHTEN",
            cfg.velocityBudgetTightenPxPerSec,
            1.f,
            20000.f);
    cfg.velocityPacingMaxPxPerSec = readEnvFloatClamp(
            "OMEGAWTK_RESIZE_GOV_VELOCITY_PACING_MAX",
            cfg.velocityPacingMaxPxPerSec,
            1.f,
            30000.f);
    cfg.admissionSpacingFromLatencyFactor = readEnvDoubleClamp(
            "OMEGAWTK_RESIZE_GOV_ADMIT_LATENCY_FACTOR",
            cfg.admissionSpacingFromLatencyFactor,
            0.0,
            2.0);
    cfg.admissionSpacingFromGpuFactor = readEnvDoubleClamp(
            "OMEGAWTK_RESIZE_GOV_ADMIT_GPU_FACTOR",
            cfg.admissionSpacingFromGpuFactor,
            0.0,
            2.0);
    cfg.admissionSpacingResizeMinMs = readEnvDoubleClamp(
            "OMEGAWTK_RESIZE_GOV_ADMIT_RESIZE_MIN_MS",
            cfg.admissionSpacingResizeMinMs,
            0.0,
            16.0);
    cfg.admissionSpacingResizeMaxMs = readEnvDoubleClamp(
            "OMEGAWTK_RESIZE_GOV_ADMIT_RESIZE_MAX_MS",
            std::max(cfg.admissionSpacingResizeMinMs,cfg.admissionSpacingResizeMaxMs),
            cfg.admissionSpacingResizeMinMs,
            32.0);
    cfg.admissionSpacingPressureMs = readEnvDoubleClamp(
            "OMEGAWTK_RESIZE_GOV_ADMIT_PRESSURE_MS",
            cfg.admissionSpacingPressureMs,
            0.0,
            32.0);

    emitGovernorTuningTrace(
            "tuning load "
            "enterResizeMs=" + std::to_string(cfg.pressureEnterLatencyMsResize) +
            " exitResizeMs=" + std::to_string(cfg.pressureExitLatencyMsResize) +
            " enterNormalMs=" + std::to_string(cfg.pressureEnterLatencyMsNormal) +
            " exitNormalMs=" + std::to_string(cfg.pressureExitLatencyMsNormal) +
            " latencyAlpha=" + std::to_string(cfg.latencyEwmaAlpha) +
            " gpuAlpha=" + std::to_string(cfg.gpuEwmaAlpha) +
            " velRelax=" + std::to_string(cfg.velocityBudgetRelaxPxPerSec) +
            " velTight=" + std::to_string(cfg.velocityBudgetTightenPxPerSec) +
            " velPaceMax=" + std::to_string(cfg.velocityPacingMaxPxPerSec) +
            " admitLatencyFactor=" + std::to_string(cfg.admissionSpacingFromLatencyFactor) +
            " admitGpuFactor=" + std::to_string(cfg.admissionSpacingFromGpuFactor) +
            " resizeSpacingMinMs=" + std::to_string(cfg.admissionSpacingResizeMinMs) +
            " resizeSpacingMaxMs=" + std::to_string(cfg.admissionSpacingResizeMaxMs) +
            " pressureSpacingMs=" + std::to_string(cfg.admissionSpacingPressureMs));
    return cfg;
}

void Compositor::enqueueLayerTreeDeltaLocked(LayerTree *tree,
                                             LayerTreeDeltaType type,
                                             Layer *layer,
                                             const Core::Rect *rect){
    if(tree == nullptr){
        return;
    }
    auto & state = layerTreeSyncState[tree];
    const auto previousIssuedEpoch = state.lastIssuedEpoch;
    const auto epoch = ++state.lastIssuedEpoch;
    assert(epoch > previousIssuedEpoch);
    if(state.lastObservedEpoch < epoch){
        state.lastObservedEpoch = epoch;
    }
    LayerTreeDelta delta {};
    delta.type = type;
    delta.tree = tree;
    delta.layer = layer;
    delta.epoch = epoch;
    delta.timestamp = std::chrono::steady_clock::now();
    if(rect != nullptr){
        delta.rect = *rect;
    }
    else if(layer != nullptr){
        delta.rect = layer->getLayerRect();
    }
    else {
        delta.rect = Core::Rect {Core::Position {0.f,0.f},0.f,0.f};
    }
    state.pendingDeltas.push_back(delta);

    const char *deltaName = "unknown";
    switch(type){
        case LayerTreeDeltaType::TreeAttached:
            deltaName = "tree-attached";
            break;
        case LayerTreeDeltaType::TreeDetached:
            deltaName = "tree-detached";
            break;
        case LayerTreeDeltaType::LayerResized:
            deltaName = "layer-resized";
            break;
        case LayerTreeDeltaType::LayerEnabled:
            deltaName = "layer-enabled";
            break;
        case LayerTreeDeltaType::LayerDisabled:
            deltaName = "layer-disabled";
            break;
        default:
            break;
    }

    emitSyncTrace(
            std::string("deltaQueued type=")
            + deltaName
            + " epoch=" + std::to_string(epoch)
            + " tree=" + std::to_string(reinterpret_cast<std::uintptr_t>(tree))
            + " layer=" + std::to_string(reinterpret_cast<std::uintptr_t>(layer)));
}

void Compositor::coalesceLayerTreeDeltasLocked(OmegaCommon::Vector<LayerTreeDelta> & deltas) const {
    if(deltas.empty()){
        return;
    }

    OmegaCommon::Vector<LayerTreeDelta> attaches {};
    OmegaCommon::Vector<LayerTreeDelta> mutations {};
    OmegaCommon::Vector<LayerTreeDelta> detaches {};

    auto upsertTreeDelta = [](OmegaCommon::Vector<LayerTreeDelta> & bucket,const LayerTreeDelta & delta){
        for(auto & existing : bucket){
            if(existing.tree == delta.tree){
                existing = delta;
                return;
            }
        }
        bucket.push_back(delta);
    };

    auto upsertLayerMutation = [](OmegaCommon::Vector<LayerTreeDelta> & bucket,const LayerTreeDelta & delta){
        for(auto & existing : bucket){
            if(existing.layer == delta.layer && existing.type == delta.type){
                existing = delta;
                return;
            }
        }
        bucket.push_back(delta);
    };

    for(auto & delta : deltas){
        switch(delta.type){
            case LayerTreeDeltaType::TreeAttached:
                upsertTreeDelta(attaches,delta);
                break;
            case LayerTreeDeltaType::TreeDetached:
                upsertTreeDelta(detaches,delta);
                break;
            case LayerTreeDeltaType::LayerResized:
            case LayerTreeDeltaType::LayerEnabled:
            case LayerTreeDeltaType::LayerDisabled:
                upsertLayerMutation(mutations,delta);
                break;
            default:
                break;
        }
    }

    OmegaCommon::Vector<LayerTreeDelta> coalesced {};
    for(auto & delta : attaches){
        coalesced.push_back(delta);
    }
    for(auto & delta : mutations){
        coalesced.push_back(delta);
    }
    for(auto & delta : detaches){
        coalesced.push_back(delta);
    }
    deltas = std::move(coalesced);
}

std::uint64_t Compositor::maxRequiredTreeEpoch(const LayerTreePacketMetadata & metadata){
    std::uint64_t requiredEpoch = 0;
    for(auto & requiredEntry : metadata.requiredEpochByTree){
        requiredEpoch = std::max(requiredEpoch,requiredEntry.second);
    }
    return requiredEpoch;
}

void Compositor::stampCommandRequiredEpochLocked(SharedHandle<CompositorCommand> & command,
                                                 std::uint64_t requiredEpoch) const {
    if(command == nullptr){
        return;
    }
    command->requiredTreeEpoch = requiredEpoch;
    if(command->type != CompositorCommand::Packet){
        return;
    }
    auto packet = std::dynamic_pointer_cast<CompositorPacketCommand>(command);
    if(packet == nullptr){
        return;
    }
    for(auto & child : packet->commands){
        stampCommandRequiredEpochLocked(child,requiredEpoch);
    }
}

bool Compositor::packetMetadataContainsResizeDeltaLocked(std::uint64_t syncLaneId,
                                                         std::uint64_t syncPacketId) const {
    if(syncLaneId == 0 || syncPacketId == 0){
        return false;
    }
    auto laneIt = layerTreePacketMetadata.find(syncLaneId);
    if(laneIt == layerTreePacketMetadata.end()){
        return false;
    }
    auto packetIt = laneIt->second.find(syncPacketId);
    if(packetIt == laneIt->second.end()){
        return false;
    }
    for(auto & delta : packetIt->second.deltas){
        if(delta.type == LayerTreeDeltaType::LayerResized){
            return true;
        }
    }
    return false;
}

bool Compositor::arePacketEpochRequirementsSatisfiedLocked(std::uint64_t syncLaneId,
                                                           std::uint64_t syncPacketId,
                                                           std::uint64_t * maxObservedRequiredEpoch,
                                                           std::uint64_t * maxMissingEpoch) const {
    if(syncLaneId == 0 || syncPacketId == 0){
        return true;
    }
    auto laneIt = layerTreePacketMetadata.find(syncLaneId);
    if(laneIt == layerTreePacketMetadata.end()){
        return true;
    }
    auto packetIt = laneIt->second.find(syncPacketId);
    if(packetIt == laneIt->second.end()){
        return true;
    }

    auto & metadata = packetIt->second;
    const auto maxRequiredEpoch = maxRequiredTreeEpoch(metadata);
    if(maxObservedRequiredEpoch != nullptr){
        *maxObservedRequiredEpoch = maxRequiredEpoch;
    }
    if(maxMissingEpoch != nullptr){
        *maxMissingEpoch = 0;
    }
    if(metadata.requiredEpochByTree.empty()){
        return true;
    }

    bool ready = true;
    for(auto & requiredEntry : metadata.requiredEpochByTree){
        auto * tree = requiredEntry.first;
        const auto requiredEpoch = requiredEntry.second;
        std::uint64_t appliedEpoch = 0;
        if(tree != nullptr){
            auto mirrorIt = backendLayerMirror.find(tree);
            if(mirrorIt != backendLayerMirror.end()){
                appliedEpoch = mirrorIt->second.lastAppliedEpoch;
            }
        }
        if(appliedEpoch < requiredEpoch){
            ready = false;
            if(maxMissingEpoch != nullptr){
                *maxMissingEpoch = std::max(*maxMissingEpoch,requiredEpoch);
            }
        }
    }
    return ready;
}

bool Compositor::isPacketEpochSupersededLocked(std::uint64_t syncLaneId,
                                               std::uint64_t olderPacketId,
                                               std::uint64_t newerPacketId) const {
    if(syncLaneId == 0 || olderPacketId == 0 || newerPacketId == 0 || olderPacketId == newerPacketId){
        return false;
    }
    auto laneIt = layerTreePacketMetadata.find(syncLaneId);
    if(laneIt == layerTreePacketMetadata.end()){
        return false;
    }
    auto olderIt = laneIt->second.find(olderPacketId);
    auto newerIt = laneIt->second.find(newerPacketId);
    if(olderIt == laneIt->second.end() || newerIt == laneIt->second.end()){
        return false;
    }
    auto & olderRequiredByTree = olderIt->second.requiredEpochByTree;
    auto & newerRequiredByTree = newerIt->second.requiredEpochByTree;
    if(olderRequiredByTree.empty() || newerRequiredByTree.empty()){
        return false;
    }

    bool anyTreeAdvanced = false;
    for(auto & olderRequired : olderRequiredByTree){
        auto newerRequired = newerRequiredByTree.find(olderRequired.first);
        if(newerRequired == newerRequiredByTree.end()){
            return false;
        }
        if(newerRequired->second < olderRequired.second){
            return false;
        }
        if(newerRequired->second > olderRequired.second){
            anyTreeAdvanced = true;
        }
    }
    return anyTreeAdvanced;
}

bool Compositor::waitForRequiredTreeEpoch(std::uint64_t syncLaneId,
                                          std::uint64_t syncPacketId,
                                          std::uint64_t requiredTreeEpoch){
    if(syncLaneId == 0 || syncPacketId == 0 || requiredTreeEpoch == 0){
        return true;
    }
    const auto waitStart = std::chrono::steady_clock::now();
    bool waited = false;
    bool attemptedLocalMirrorApply = false;

    auto flushWaitMetrics = [&](){
        if(!waited || packetTelemetryState == nullptr){
            return;
        }
        std::lock_guard<std::mutex> telemetryLock(packetTelemetryState->mutex);
        auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
        laneState.epochWaitCount += 1;
        laneState.totalEpochWait += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - waitStart);
    };

    while(true){
        std::uint64_t observedRequiredEpoch = requiredTreeEpoch;
        std::uint64_t missingEpoch = 0;
        bool epochReady = false;
        {
            std::lock_guard<std::mutex> lk(mutex);
            epochReady = arePacketEpochRequirementsSatisfiedLocked(syncLaneId,
                                                                   syncPacketId,
                                                                   &observedRequiredEpoch,
                                                                   &missingEpoch);
        }
        if(epochReady){
            flushWaitMetrics();
            return true;
        }

        if(!attemptedLocalMirrorApply){
            attemptedLocalMirrorApply = true;
            applyLayerTreePacketDeltasToBackendMirror(syncLaneId,syncPacketId,nullptr);
            continue;
        }

        waited = true;
        emitSyncTrace(
                "renderWaitEpoch lane=" + std::to_string(syncLaneId) +
                " packet=" + std::to_string(syncPacketId) +
                " requiredEpoch=" + std::to_string(observedRequiredEpoch) +
                " missingEpoch=" + std::to_string(missingEpoch));

        std::unique_lock<std::mutex> lk(mutex);
        queueCondition.wait_for(lk,std::chrono::milliseconds(1),[&]{
            return scheduler.shutdown;
        });
        if(scheduler.shutdown){
            flushWaitMetrics();
            return false;
        }
    }
}

void Compositor::bindPendingLayerTreeDeltasToPacketLocked(std::uint64_t syncLaneId,std::uint64_t syncPacketId){
    if(syncLaneId == 0 || syncPacketId == 0){
        return;
    }
    auto & laneMetadata = layerTreePacketMetadata[syncLaneId];
    auto existing = laneMetadata.find(syncPacketId);
    if(existing != laneMetadata.end()){
        return;
    }

    LayerTreePacketMetadata metadata {};
    metadata.syncLaneId = syncLaneId;
    metadata.syncPacketId = syncPacketId;

    for(auto & treeStateEntry : layerTreeSyncState){
        auto *tree = treeStateEntry.first;
        auto & state = treeStateEntry.second;
        if(tree == nullptr || state.pendingDeltas.empty()){
            continue;
        }
        auto laneBindingIt = layerTreeLaneBinding.find(tree);
        if(laneBindingIt != layerTreeLaneBinding.end() &&
           laneBindingIt->second != 0 &&
           laneBindingIt->second != syncLaneId){
            continue;
        }
        for(auto & delta : state.pendingDeltas){
            metadata.deltas.push_back(delta);
        }
        state.pendingDeltas.clear();
    }

    if(metadata.deltas.empty()){
        return;
    }

    coalesceLayerTreeDeltasLocked(metadata.deltas);
    bool hasResizeDelta = false;
    for(auto & delta : metadata.deltas){
        if(delta.tree == nullptr){
            continue;
        }
        if(delta.type == LayerTreeDeltaType::LayerResized){
            hasResizeDelta = true;
        }
        auto & requiredEpoch = metadata.requiredEpochByTree[delta.tree];
        requiredEpoch = std::max(requiredEpoch,delta.epoch);
    }

    laneMetadata[syncPacketId] = std::move(metadata);
    if(hasResizeDelta){
        markLaneResizeActivity(syncLaneId);
    }
    emitSyncTrace(
            "packetize lane=" + std::to_string(syncLaneId) +
            " packet=" + std::to_string(syncPacketId) +
            " deltas=" + std::to_string(laneMetadata[syncPacketId].deltas.size()) +
            " requiredEpoch=" + std::to_string(maxRequiredTreeEpoch(laneMetadata[syncPacketId])));
}

void Compositor::releaseLayerTreePacketMetadata(std::uint64_t syncLaneId,std::uint64_t syncPacketId){
    if(syncLaneId == 0 || syncPacketId == 0){
        return;
    }
    std::lock_guard<std::mutex> lk(mutex);
    auto laneIt = layerTreePacketMetadata.find(syncLaneId);
    if(laneIt == layerTreePacketMetadata.end()){
        return;
    }
    laneIt->second.erase(syncPacketId);
    if(laneIt->second.empty()){
        layerTreePacketMetadata.erase(syncLaneId);
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
            enqueueLayerTreeDeltaLocked(tree,LayerTreeDeltaType::TreeAttached,nullptr,nullptr);
        }
        auto & laneBinding = layerTreeLaneBinding[tree];
        if(syncLaneId != 0){
            laneBinding = syncLaneId;
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
        if(removed){
            enqueueLayerTreeDeltaLocked(tree,LayerTreeDeltaType::TreeDetached,nullptr,nullptr);
            layerTreeLaneBinding.erase(tree);
            backendLayerMirror.erase(tree);
        }
    }
    if(removed){
        tree->removeObserver(this);
    }
}

void Compositor::hasDetached(LayerTree *tree){
    unobserveLayerTree(tree);
}

void Compositor::layerHasResized(Layer *layer){
    if(layer == nullptr){
        return;
    }
    auto *limb = layer->getParentLimb();
    if(limb == nullptr){
        return;
    }
    auto *tree = limb->getParentTree();
    std::lock_guard<std::mutex> lk(mutex);
    enqueueLayerTreeDeltaLocked(tree,LayerTreeDeltaType::LayerResized,layer,&layer->getLayerRect());
}

void Compositor::layerHasDisabled(Layer *layer){
    if(layer == nullptr){
        return;
    }
    auto *limb = layer->getParentLimb();
    if(limb == nullptr){
        return;
    }
    auto *tree = limb->getParentTree();
    std::lock_guard<std::mutex> lk(mutex);
    enqueueLayerTreeDeltaLocked(tree,LayerTreeDeltaType::LayerDisabled,layer,&layer->getLayerRect());
}

void Compositor::layerHasEnabled(Layer *layer){
    if(layer == nullptr){
        return;
    }
    auto *limb = layer->getParentLimb();
    if(limb == nullptr){
        return;
    }
    auto *tree = limb->getParentTree();
    std::lock_guard<std::mutex> lk(mutex);
    enqueueLayerTreeDeltaLocked(tree,LayerTreeDeltaType::LayerEnabled,layer,&layer->getLayerRect());
}

void CompositorScheduler::processCommand(SharedHandle<CompositorCommand> & command,bool laneAdmissionBypassed){
    if(command == nullptr){
        return;
    }
    if(hasSyncPacketMetadata(command) && !laneAdmissionBypassed){
        if(!compositor->waitForLaneAdmission(command->syncLaneId,command->syncPacketId)){
            compositor->markPacketFailed(command->syncLaneId,command->syncPacketId);
            compositor->releaseLayerTreePacketMetadata(command->syncLaneId,command->syncPacketId);
            command->status.set(CommandStatus::Failed);
            return;
        }
        if(isRenderLikeCommand(command) &&
           !compositor->waitForRequiredTreeEpoch(command->syncLaneId,
                                                 command->syncPacketId,
                                                 command->requiredTreeEpoch)){
            compositor->markPacketFailed(command->syncLaneId,command->syncPacketId);
            compositor->releaseLayerTreePacketMetadata(command->syncLaneId,command->syncPacketId);
            command->status.set(CommandStatus::Failed);
            return;
        }
    }
    if(command->type == CompositorCommand::Packet){
        auto packet = std::dynamic_pointer_cast<CompositorPacketCommand>(command);
        if(packet == nullptr){
            compositor->markPacketFailed(command->syncLaneId,command->syncPacketId);
            compositor->releaseLayerTreePacketMetadata(command->syncLaneId,command->syncPacketId);
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
        compositor->releaseLayerTreePacketMetadata(command->syncLaneId,command->syncPacketId);
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
                if(!laneAdmissionBypassed){
                    compositor->releaseLayerTreePacketMetadata(command->syncLaneId,command->syncPacketId);
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
        compositor->releaseLayerTreePacketMetadata(command->syncLaneId,command->syncPacketId);
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
    governorTuning = loadGovernorTuningConfig();
    packetTelemetryState->tuning = governorTuning;
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
     std::cout << "~Compositor()" << std::endl;
};

void Compositor::scheduleCommand(SharedHandle<CompositorCommand> & command){
    if(command == nullptr){
        return;
    }
    const auto traceLaneId = command->syncLaneId;
    const auto tracePacketId = command->syncPacketId;
    const auto traceCommandId = command->id;
    const auto traceRequiredEpoch = command->requiredTreeEpoch;
    {
        std::lock_guard<std::mutex> lk(mutex);
        if(command->syncLaneId != 0){
            markLaneResizeActivity(command->syncLaneId,&command->resizeGovernor);
        }

        if(hasSyncPacketMetadata(command)){
            bindPendingLayerTreeDeltasToPacketLocked(command->syncLaneId,command->syncPacketId);
            std::uint64_t requiredEpoch = 0;
            auto laneMetadata = layerTreePacketMetadata.find(command->syncLaneId);
            if(laneMetadata != layerTreePacketMetadata.end()){
                auto packetMetadata = laneMetadata->second.find(command->syncPacketId);
                if(packetMetadata != laneMetadata->second.end()){
                    requiredEpoch = maxRequiredTreeEpoch(packetMetadata->second);
                }
            }
            stampCommandRequiredEpochLocked(command,requiredEpoch);
            markPacketQueued(command->syncLaneId,command->syncPacketId,command);
            const bool renderLike = isRenderLikeCommand(command);
            const bool liveResizeEpoch = packetMetadataContainsResizeDeltaLocked(command->syncLaneId,
                                                                                 command->syncPacketId);
            const bool coalesceForEpoch = requiredEpoch != 0;
            if(renderLike &&
               (commandHasNonNoOpRender(command) || coalesceForEpoch || liveResizeEpoch)){
                // Base target-aware coalescing.
                dropQueuedStaleForLaneLocked(command->syncLaneId,command);
                // Saturated lanes keep newest packet for overlapping targets.
                if(isLaneSaturated(command->syncLaneId)){
                    dropQueuedStaleForLaneLocked(command->syncLaneId,command);
                }
                // Pressure lanes can additionally coalesce effect-heavy packets.
                if(isLaneUnderPressure(command->syncLaneId)){
                    dropQueuedStaleForLaneLocked(command->syncLaneId,command);
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
                " cmdId=" + std::to_string(traceCommandId) +
                " requiredEpoch=" + std::to_string(traceRequiredEpoch));
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
    const auto & tuning = governorTuning;
    const bool resizeBudgetActive = now < laneState.resizeModeUntil ||
                                    resizeGovernorIndicatesActive(laneState.latestResizeGovernor);
    unsigned budget = resizeBudgetActive
                      ? kMaxFramesInFlightResize
                      : kMaxFramesInFlightNormal;
    const float velocity = finiteAbs(laneState.latestResizeGovernor.velocityPxPerSec);
    if(resizeBudgetActive && !laneState.underPressure){
        // Relax to the normal budget at low resize velocity so static-heavy
        // trees can keep up without corner-collapse.
        if(velocity <= tuning.velocityBudgetRelaxPxPerSec &&
           (!laneState.hasSubmitToPresentSample ||
            laneState.submitToPresentEwmaMs <= tuning.pressureExitLatencyMsResize)){
            budget = std::max(budget,kMaxFramesInFlightNormal);
        }
        // Tighten under aggressive velocity spikes.
        if(velocity >= tuning.velocityBudgetTightenPxPerSec){
            budget = 1;
        }
    }
    if(laneState.underPressure){
        budget = 1;
    }
    if(laneState.hasSubmitToPresentSample){
        const double pressureGate = resizeBudgetActive
                                    ? tuning.pressureEnterLatencyMsResize
                                    : tuning.pressureEnterLatencyMsNormal;
        if(laneState.submitToPresentEwmaMs >= (pressureGate * 1.25)){
            budget = 1;
        }
    }
    if(!laneState.startupStabilized){
        budget = std::min(budget,1u);
    }
    return std::max(1u,budget);
}

std::chrono::microseconds Compositor::laneMinSubmitSpacingForNow(const LaneRuntimeState & laneState,
                                                                 std::chrono::steady_clock::time_point now) const {
    const auto & tuning = governorTuning;
    double spacingMs = 0.0;
    if(laneState.hasSubmitToPresentSample){
        spacingMs = std::max(spacingMs,laneState.submitToPresentEwmaMs * tuning.admissionSpacingFromLatencyFactor);
    }
    if(laneState.hasGpuDurationSample){
        spacingMs = std::max(spacingMs,laneState.gpuDurationEwmaMs * tuning.admissionSpacingFromGpuFactor);
    }

    const bool resizeBudgetActive = now < laneState.resizeModeUntil ||
                                    resizeGovernorIndicatesActive(laneState.latestResizeGovernor);
    if(resizeBudgetActive){
        const float velocity = finiteAbs(laneState.latestResizeGovernor.velocityPxPerSec);
        const double velocityNorm = std::min(1.0,std::max(0.0,static_cast<double>(velocity) / tuning.velocityPacingMaxPxPerSec));
        const double resizeSpacing = tuning.admissionSpacingResizeMinMs +
                                     ((tuning.admissionSpacingResizeMaxMs - tuning.admissionSpacingResizeMinMs) * velocityNorm);
        spacingMs = std::max(spacingMs,resizeSpacing);
    }
    if(laneState.underPressure){
        spacingMs = std::max(spacingMs,tuning.admissionSpacingPressureMs);
    }
    spacingMs = std::min(16.0,std::max(0.0,spacingMs));
    return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::duration<double,std::milli>(spacingMs));
}

LaneEffectQuality Compositor::desiredLaneQualityForNow(const LaneRuntimeState & laneState,
                                                       std::chrono::steady_clock::time_point now,
                                                       const GovernorTuningConfig & tuning) {
    unsigned stress = 0;
    const bool resizeBudgetActive = now < laneState.resizeModeUntil ||
                                    resizeGovernorIndicatesActive(laneState.latestResizeGovernor);
    if(resizeBudgetActive){
        stress += 1;
    }
    if(laneState.underPressure){
        stress += 1;
    }
    const float velocity = finiteAbs(laneState.latestResizeGovernor.velocityPxPerSec);
    if(velocity >= tuning.velocityBudgetTightenPxPerSec){
        stress += 2;
    }
    else if(velocity >= (tuning.velocityBudgetRelaxPxPerSec * 2.f)){
        stress += 1;
    }
    if(laneState.hasSubmitToPresentSample){
        const double pressureGate = resizeBudgetActive
                                    ? tuning.pressureEnterLatencyMsResize
                                    : tuning.pressureEnterLatencyMsNormal;
        if(laneState.submitToPresentEwmaMs >= (pressureGate * 1.35)){
            stress += 1;
        }
    }
    if(stress >= 3){
        return LaneEffectQuality::Minimal;
    }
    if(stress >= 1){
        return LaneEffectQuality::Reduced;
    }
    return LaneEffectQuality::Full;
}

void Compositor::updateLaneQualityForPresentedPacket(LaneRuntimeState & laneState,
                                                     std::chrono::steady_clock::time_point now,
                                                     const GovernorTuningConfig & tuning){
    const auto desired = desiredLaneQualityForNow(laneState,now,tuning);
    if(static_cast<unsigned>(desired) > static_cast<unsigned>(laneState.qualityLevel)){
        laneState.qualityLevel = desired;
        laneState.qualityRecoveryStreak = 0;
        return;
    }
    if(desired == laneState.qualityLevel){
        if(desired == LaneEffectQuality::Full){
            laneState.qualityRecoveryStreak = 0;
        }
        else {
            laneState.qualityRecoveryStreak = std::min<std::uint32_t>(
                    laneState.qualityRecoveryStreak + 1,
                    kQualityRecoveryPresents);
        }
        return;
    }

    laneState.qualityRecoveryStreak += 1;
    if(laneState.qualityRecoveryStreak < kQualityRecoveryPresents){
        return;
    }
    laneState.qualityRecoveryStreak = 0;
    if(laneState.qualityLevel == LaneEffectQuality::Minimal){
        laneState.qualityLevel = LaneEffectQuality::Reduced;
    }
    else {
        laneState.qualityLevel = LaneEffectQuality::Full;
    }
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
    return entry.hasStateMutation ||
           entry.hasEffectMutation ||
           entry.hasNonNoOpRender;
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
    bool pacingRecorded = false;

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
                const bool startupCritical =
                        !laneState.startupStabilized &&
                        (entry.hasStateMutation || entry.hasEffectMutation || entry.hasNonNoOpRender);
                const auto minSpacing = startupCritical
                                        ? std::chrono::microseconds{0}
                                        : laneMinSubmitSpacingForNow(laneState,now);
                const bool paceReady = minSpacing.count() == 0 ||
                                       laneState.lastAdmissionGrantTime == std::chrono::steady_clock::time_point{} ||
                                       (now - laneState.lastAdmissionGrantTime) >= minSpacing;
                if(!paceReady){
                    waited = true;
                    if(!pacingRecorded){
                        laneState.pacingWaitCount += 1;
                        pacingRecorded = true;
                        emitSyncTrace(
                                "pace-wait lane=" + std::to_string(syncLaneId) +
                                " packet=" + std::to_string(syncPacketId) +
                                " spacingUs=" + std::to_string(minSpacing.count()));
                    }
                }
                else {
                    laneState.lastAdmissionGrantTime = now;
                    if(waited){
                        laneState.admissionWaitCount += 1;
                        const auto waitedFor = std::chrono::steady_clock::now() - waitStart;
                        laneState.totalAdmissionWait += std::chrono::duration_cast<std::chrono::microseconds>(
                                waitedFor);
                        if(pacingRecorded){
                            laneState.totalPacingWait += std::chrono::duration_cast<std::chrono::microseconds>(
                                    waitedFor);
                        }
                        emitSyncTrace(
                                "admit lane=" + std::to_string(syncLaneId) +
                                " packet=" + std::to_string(syncPacketId) +
                                " waitedUs=" + std::to_string(
                                        std::chrono::duration_cast<std::chrono::microseconds>(
                                                waitedFor).count()));
                    }
                    return true;
                }
            }
            else {
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
        }
        std::unique_lock<std::mutex> lk(mutex);
        queueCondition.wait_for(lk,std::chrono::milliseconds(1),[&]{
            return scheduler.shutdown;
        });
        if(scheduler.shutdown){
            if(waited){
                std::lock_guard<std::mutex> telemetryLock(packetTelemetryState->mutex);
                auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
                const auto waitedFor = std::chrono::steady_clock::now() - waitStart;
                laneState.admissionWaitCount += 1;
                laneState.totalAdmissionWait += std::chrono::duration_cast<std::chrono::microseconds>(
                        waitedFor);
                if(pacingRecorded){
                    laneState.totalPacingWait += std::chrono::duration_cast<std::chrono::microseconds>(
                            waitedFor);
                }
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
    const bool incomingHasTargets = !incomingTargets.empty();
    const bool incomingHasResizeMutation = commandContainsResizeActivity(incoming);
    const bool incomingHasEffectMutation = commandContainsEffectMutation(incoming);
    const bool laneUnderPressure = isLaneUnderPressure(syncLaneId);
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
        const bool supersededByEpoch = isPacketEpochSupersededLocked(syncLaneId,
                                                                     queuedCommand->syncPacketId,
                                                                     incoming->syncPacketId);
        // By default, never coalesce away packets that carry state mutations.
        // During live resize, allow coalescing packets that are resize-only
        // (no effect/cancel/non-resize state) so newest geometry wins.
        if(commandContainsStateMutation(queuedCommand)){
            const bool queuedResizeOnly =
                    commandContainsResizeActivity(queuedCommand) &&
                    !commandContainsEffectMutation(queuedCommand);
            const bool queuedEffectOnly =
                    commandContainsEffectMutation(queuedCommand) &&
                    !queuedResizeOnly;
            const bool pressureCoalesceAllowed =
                    laneUnderPressure &&
                    incomingHasEffectMutation &&
                    queuedEffectOnly;
            if(!(incomingHasResizeMutation && queuedResizeOnly) &&
               !pressureCoalesceAllowed &&
               !supersededByEpoch){
                return false;
            }
        }
        if(!supersededByEpoch){
            if(!incomingHasTargets){
                return false;
            }
            OmegaCommon::Vector<RenderTargetEpoch> pendingTargets {};
            collectRenderTargetsForCommand(queuedCommand,pendingTargets);
            if(pendingTargets.empty() || !targetsOverlap(pendingTargets,incomingTargets)){
                return false;
            }
        }
        const auto dropReason = supersededByEpoch
                                ? PacketDropReason::EpochSuperseded
                                : PacketDropReason::StaleCoalesced;
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
                              dropReason);
            if(supersededByEpoch){
                emitSyncTrace(
                        "epochDropped lane=" + std::to_string(queuedCommand->syncLaneId) +
                        " packet=" + std::to_string(queuedCommand->syncPacketId) +
                        " replacedBy=" + std::to_string(incoming->syncPacketId) +
                        " requiredEpoch=" + std::to_string(queuedCommand->requiredTreeEpoch));
            }
            queuedCommand->status.set(CommandStatus::Delayed);
            noteQueueDropLocked(queuedCommand);
            return true;
        }
        if(queuedCommand->type == CompositorCommand::Render){
            markPacketDropped(queuedCommand->syncLaneId,
                              queuedCommand->syncPacketId,
                              dropReason);
            if(supersededByEpoch){
                emitSyncTrace(
                        "epochDropped lane=" + std::to_string(queuedCommand->syncLaneId) +
                        " packet=" + std::to_string(queuedCommand->syncPacketId) +
                        " replacedBy=" + std::to_string(incoming->syncPacketId) +
                        " requiredEpoch=" + std::to_string(queuedCommand->requiredTreeEpoch));
            }
            queuedCommand->status.set(CommandStatus::Delayed);
            noteQueueDropLocked(queuedCommand);
            return true;
        }
        return false;
    });
}

void Compositor::markLaneResizeActivity(std::uint64_t syncLaneId,
                                        const ResizeGovernorMetadata * governorMetadata){
    if(syncLaneId == 0 || packetTelemetryState == nullptr){
        return;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto & laneState = packetTelemetryState->laneRuntime[syncLaneId];
    const auto now = std::chrono::steady_clock::now();
    bool activateResizeBudget = true;
    if(governorMetadata != nullptr){
        laneState.latestResizeGovernor = *governorMetadata;
        activateResizeBudget = resizeGovernorIndicatesActive(*governorMetadata);
    }
    if(activateResizeBudget){
        laneState.resizeModeUntil = now + kResizeModeHoldWindow;
    }
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
    entry.requiredTreeEpoch = command == nullptr ? 0 : command->requiredTreeEpoch;
    entry.layerTreeMirrorApplied = entry.requiredTreeEpoch == 0;
    if(command != nullptr){
        entry.resizeGovernor = command->resizeGovernor;
        entry.resizeCoordinatorGeneration = command->resizeCoordinatorGeneration;
        laneState.latestResizeGovernor = command->resizeGovernor;
        laneState.latestResizeCoordinatorGeneration =
                std::max(laneState.latestResizeCoordinatorGeneration,
                         command->resizeCoordinatorGeneration);
        if(resizeGovernorIndicatesActive(command->resizeGovernor)){
            laneState.resizeModeUntil = std::chrono::steady_clock::now() + kResizeModeHoldWindow;
        }
    }
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

void Compositor::markPacketMirrorApplied(std::uint64_t syncLaneId,std::uint64_t syncPacketId){
    if(syncLaneId == 0 || syncPacketId == 0 || packetTelemetryState == nullptr){
        return;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto & entry = packetTelemetryState->lanes[syncLaneId][syncPacketId];
    entry.layerTreeMirrorApplied = true;
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
    else if(reason == PacketDropReason::EpochSuperseded){
        laneState.epochDropCount += 1;
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
    return !(entry.hasStateMutation || entry.hasEffectMutation || entry.hasResizeMutation);
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
    const auto desiredQuality = desiredLaneQualityForNow(runtimeIt->second,now,governorTuning);
    auto quality = runtimeIt->second.qualityLevel;
    if(static_cast<unsigned>(desiredQuality) > static_cast<unsigned>(quality)){
        quality = desiredQuality;
    }
    if(quality == LaneEffectQuality::Full){
        return clamped;
    }
    const float scale = quality == LaneEffectQuality::Reduced
                        ? kReducedEffectScale
                        : kMinimalEffectScale;
    const float opacityScale = quality == LaneEffectQuality::Reduced
                               ? kReducedShadowOpacityScale
                               : kMinimalShadowOpacityScale;
    clamped.radius = std::max(0.f,clamped.radius * scale);
    clamped.blurAmount = std::max(0.f,clamped.blurAmount * scale);
    clamped.x_offset *= scale;
    clamped.y_offset *= scale;
    clamped.opacity *= opacityScale;
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
    const auto desiredQuality = desiredLaneQualityForNow(runtimeIt->second,now,governorTuning);
    auto quality = runtimeIt->second.qualityLevel;
    if(static_cast<unsigned>(desiredQuality) > static_cast<unsigned>(quality)){
        quality = desiredQuality;
    }
    if(quality == LaneEffectQuality::Full){
        return clamped;
    }
    const float scale = quality == LaneEffectQuality::Reduced
                        ? kReducedEffectScale
                        : kMinimalEffectScale;
    if(clamped.type == CanvasEffect::DirectionalBlur){
        clamped.directionalBlur.radius = std::max(0.f,clamped.directionalBlur.radius * scale);
    }
    else if(clamped.type == CanvasEffect::GaussianBlur){
        clamped.gaussianBlur.radius = std::max(0.f,clamped.gaussianBlur.radius * scale);
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
            if(entry.hasNonNoOpRender){
                laneState.hasPresentedRenderableContent = true;
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
                            ((1.0 - state->tuning.latencyEwmaAlpha) * laneState.submitToPresentEwmaMs) +
                            (state->tuning.latencyEwmaAlpha * submitToPresentMs);
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
                            ((1.0 - state->tuning.gpuEwmaAlpha) * laneState.gpuDurationEwmaMs) +
                            (state->tuning.gpuEwmaAlpha * gpuDurationMs);
                }
            }

            const auto now = std::chrono::steady_clock::now();
            const bool resizeBudget = now < laneState.resizeModeUntil ||
                                      resizeGovernorIndicatesActive(laneState.latestResizeGovernor);
            const double enterThreshold = resizeBudget
                                          ? state->tuning.pressureEnterLatencyMsResize
                                          : state->tuning.pressureEnterLatencyMsNormal;
            const double exitThreshold = resizeBudget
                                         ? state->tuning.pressureExitLatencyMsResize
                                         : state->tuning.pressureExitLatencyMsNormal;
            if(laneState.hasSubmitToPresentSample){
                if(!laneState.underPressure && laneState.submitToPresentEwmaMs >= enterThreshold){
                    laneState.underPressure = true;
                }
                else if(laneState.underPressure && laneState.submitToPresentEwmaMs <= exitThreshold){
                    laneState.underPressure = false;
                }
            }
            Compositor::updateLaneQualityForPresentedPacket(laneState,now,state->tuning);

            emitSyncTrace(
                    "present lane=" + std::to_string(telemetry.syncLaneId) +
                    " packet=" + std::to_string(telemetry.syncPacketId) +
                    " inFlight=" + std::to_string(laneState.inFlight) +
                    " ewmaMs=" + std::to_string(laneState.submitToPresentEwmaMs) +
                    " quality=" + laneEffectQualityName(laneState.qualityLevel));
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
        laneState.qualityLevel = LaneEffectQuality::Minimal;
        laneState.qualityRecoveryStreak = 0;
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
        snapshot.resizeBudgetActive =
                now < runtimeIt->second.resizeModeUntil ||
                resizeGovernorIndicatesActive(runtimeIt->second.latestResizeGovernor);
        snapshot.startupStabilized = runtimeIt->second.startupStabilized;
        snapshot.firstPresentedPacketId = runtimeIt->second.firstPresentedPacketId;
        snapshot.underPressure = runtimeIt->second.underPressure;
        snapshot.submitToPresentEwmaMs = runtimeIt->second.submitToPresentEwmaMs;
        snapshot.gpuDurationEwmaMs = runtimeIt->second.gpuDurationEwmaMs;
        snapshot.droppedPacketCount = runtimeIt->second.packetsDropped;
        snapshot.failedPacketCount = runtimeIt->second.packetsFailed;
        snapshot.qualityLevel = runtimeIt->second.qualityLevel;
        snapshot.latestResizeGovernor = runtimeIt->second.latestResizeGovernor;
        snapshot.latestResizeCoordinatorGeneration = runtimeIt->second.latestResizeCoordinatorGeneration;
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

Compositor::LayerTreeSyncSnapshot Compositor::getLayerTreeSyncSnapshot(LayerTree *tree) {
    LayerTreeSyncSnapshot snapshot {};
    if(tree == nullptr){
        return snapshot;
    }
    std::lock_guard<std::mutex> lk(mutex);
    for(auto *targetTree : targetLayerTrees){
        if(targetTree == tree){
            snapshot.observed = true;
            break;
        }
    }
    auto it = layerTreeSyncState.find(tree);
    if(it == layerTreeSyncState.end()){
        return snapshot;
    }
    snapshot.lastIssuedEpoch = it->second.lastIssuedEpoch;
    snapshot.lastObservedEpoch = it->second.lastObservedEpoch;
    snapshot.pendingDeltaCount = it->second.pendingDeltas.size();
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
        snapshot.pacingWaitCount = laneState.pacingWaitCount;
        snapshot.pacingWaitTotalMs = std::chrono::duration<double,std::milli>(laneState.totalPacingWait).count();
        snapshot.epochWaitCount = laneState.epochWaitCount;
        snapshot.epochWaitTotalMs = std::chrono::duration<double,std::milli>(laneState.totalEpochWait).count();
        snapshot.epochDropCount = laneState.epochDropCount;
        snapshot.inFlight = laneState.inFlight;
        snapshot.maxInFlightObserved = laneState.maxInFlightObserved;
        snapshot.resizeBudgetActive =
                now < laneState.resizeModeUntil ||
                resizeGovernorIndicatesActive(laneState.latestResizeGovernor);
        snapshot.startupStabilized = laneState.startupStabilized;
        snapshot.underPressure = laneState.underPressure;
        snapshot.submitToPresentEwmaMs = laneState.submitToPresentEwmaMs;
        snapshot.gpuDurationEwmaMs = laneState.gpuDurationEwmaMs;
        snapshot.qualityLevel = laneState.qualityLevel;
        snapshot.latestResizeGovernor = laneState.latestResizeGovernor;
        snapshot.latestResizeCoordinatorGeneration = laneState.latestResizeCoordinatorGeneration;
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
        if(snapshot.latestResizeCoordinatorGeneration != 0 &&
           record.resizeCoordinatorGeneration != 0 &&
           record.resizeCoordinatorGeneration < snapshot.latestResizeCoordinatorGeneration &&
           (record.phase == PacketLifecyclePhase::Queued ||
            record.phase == PacketLifecyclePhase::Submitted ||
            record.phase == PacketLifecyclePhase::GPUCompleted)){
            snapshot.staleCoordinatorGenerationPacketCount += 1;
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
       << ", paceWaitCount=" << snapshot.pacingWaitCount
       << ", paceWaitMs=" << snapshot.pacingWaitTotalMs
       << ", epochWaitCount=" << snapshot.epochWaitCount
       << ", epochWaitMs=" << snapshot.epochWaitTotalMs
       << ", epochDrops=" << snapshot.epochDropCount
       << ", staleCoordGenPackets=" << snapshot.staleCoordinatorGenerationPacketCount
       << ", quality=" << laneEffectQualityName(snapshot.qualityLevel)
       << ", resizeSessionId=" << snapshot.latestResizeGovernor.sessionId
       << ", resizePhase=" << resizeGovernorPhaseName(snapshot.latestResizeGovernor.phase)
       << ", resizeActive=" << (snapshot.latestResizeGovernor.active ? "yes" : "no")
       << ", animatedTree=" << (snapshot.latestResizeGovernor.animatedTree ? "yes" : "no")
       << ", resizeVelocity=" << snapshot.latestResizeGovernor.velocityPxPerSec
       << ", resizeAcceleration=" << snapshot.latestResizeGovernor.accelerationPxPerSec2
       << ", coordGen=" << snapshot.latestResizeCoordinatorGeneration
       << ", ewmaSubmitPresentMs=" << snapshot.submitToPresentEwmaMs
       << ", ewmaGpuMs=" << snapshot.gpuDurationEwmaMs
       << "}";
    return ss.str();
}


};
