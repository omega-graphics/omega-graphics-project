#include "../Compositor.h"
#include "VisualTree.h"
#include "ResourceFactory.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

namespace OmegaWTK::Composition {

namespace {
    constexpr float kMaxTextureDimension = 16384.f;
#if defined(TARGET_MACOS)
    constexpr float kLogicalScaleFloor = 2.f;
#else
    constexpr float kLogicalScaleFloor = 1.f;
#endif
    constexpr float kMaxLogicalLayerDimension = kMaxTextureDimension / kLogicalScaleFloor;

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

    static inline Core::Rect sanitizeCommandRect(const Core::Rect & candidate,const Core::Rect & fallback){
        Core::Rect saneFallback = fallback;
        if(!std::isfinite(saneFallback.pos.x)){
            saneFallback.pos.x = 0.f;
        }
        if(!std::isfinite(saneFallback.pos.y)){
            saneFallback.pos.y = 0.f;
        }
        if(!std::isfinite(saneFallback.w) || saneFallback.w <= 0.f){
            saneFallback.w = 1.f;
        }
        if(!std::isfinite(saneFallback.h) || saneFallback.h <= 0.f){
            saneFallback.h = 1.f;
        }
        saneFallback.w = std::clamp(saneFallback.w,1.f,kMaxLogicalLayerDimension);
        saneFallback.h = std::clamp(saneFallback.h,1.f,kMaxLogicalLayerDimension);

        Core::Rect sane = candidate;
        if(!std::isfinite(sane.pos.x)){
            sane.pos.x = saneFallback.pos.x;
        }
        if(!std::isfinite(sane.pos.y)){
            sane.pos.y = saneFallback.pos.y;
        }
        if(!std::isfinite(sane.w) || sane.w <= 0.f){
            sane.w = saneFallback.w;
        }
        if(!std::isfinite(sane.h) || sane.h <= 0.f){
            sane.h = saneFallback.h;
        }
        sane.w = std::clamp(sane.w,1.f,kMaxLogicalLayerDimension);
        sane.h = std::clamp(sane.h,1.f,kMaxLogicalLayerDimension);
        return sane;
    }

    static inline Core::Rect normalizeRootVisualRect(const Core::Rect & rect){
        Core::Rect normalized = rect;
        normalized.pos.x = 0.f;
        normalized.pos.y = 0.f;
        return normalized;
    }

    static BackendRenderTargetContext *s_lastClearedContext = nullptr;
    static void *s_lastClearedRenderTarget = nullptr;
    static void resetLastClearedForNextBatch(){
        s_lastClearedContext = nullptr;
        s_lastClearedRenderTarget = nullptr;
    }

    static BackendRenderTargetContext * ensureLayerSurfaceTarget(BackendCompRenderTarget & target,Layer * layer){
        if(layer == nullptr || target.visualTree == nullptr){
            return nullptr;
        }

        auto existing = target.surfaceTargets.find(layer);
        if(existing != target.surfaceTargets.end() && existing->second != nullptr){
            std::cout << "[WTK Diag] ensureLayerSurface: reusing existing for layer=" << layer
                      << " isChild=" << layer->isChildLayer()
                      << " hasRoot=" << target.visualTree->hasRootVisual() << std::endl;
            return existing->second;
        }
        std::cout << "[WTK Diag] ensureLayerSurface: CREATING for layer=" << layer
                  << " isChild=" << layer->isChildLayer()
                  << " hasRoot=" << target.visualTree->hasRootVisual() << std::endl;

        if(layer->isChildLayer()){
            if(!target.visualTree->hasRootVisual()){
                auto treeRoot = layer->getParentTree()->getRootLayer();
                auto rootRect = sanitizeCommandRect(
                        treeRoot->getLayerRect(),
                        Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
                rootRect = normalizeRootVisualRect(rootRect);
                BackendResourceFactory factory;
                auto rootVisual = factory.createRootVisual(*target.visualTree, rootRect, target.viewPresentTarget);
                auto insertedRoot = target.surfaceTargets.insert(std::make_pair(treeRoot.get(),&rootVisual->renderTarget));
                if(!insertedRoot.second){
                    insertedRoot.first->second = &rootVisual->renderTarget;
                }
            }
            auto layerRect = sanitizeCommandRect(
                    layer->getLayerRect(),
                    Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
            BackendResourceFactory factory;
            auto visual = factory.createChildVisual(*target.visualTree, layerRect);
            auto inserted = target.surfaceTargets.insert(std::make_pair(layer,&visual->renderTarget));
            if(!inserted.second){
                inserted.first->second = &visual->renderTarget;
            }
            return inserted.first->second;
        }

        if(target.visualTree->root != nullptr){
            auto *rootTarget = &(target.visualTree->root->renderTarget);
            auto inserted = target.surfaceTargets.insert(std::make_pair(layer,rootTarget));
            if(!inserted.second){
                inserted.first->second = rootTarget;
            }
            auto layerRect = sanitizeCommandRect(
                    layer->getLayerRect(),
                    Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
            layerRect = normalizeRootVisualRect(layerRect);
            rootTarget->setRenderTargetSize(layerRect);
            return inserted.first->second;
        }

        auto layerRect = sanitizeCommandRect(
                layer->getLayerRect(),
                Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
        layerRect = normalizeRootVisualRect(layerRect);
        BackendResourceFactory factory;
        auto visual = factory.createRootVisual(*target.visualTree, layerRect, target.viewPresentTarget);
        auto inserted = target.surfaceTargets.insert(std::make_pair(layer,&visual->renderTarget));
        if(!inserted.second){
            inserted.first->second = &visual->renderTarget;
        }
        return inserted.first->second;
    }

}

// applyLayerTreePacketDeltasToBackendMirror removed (Phase 2).
#if 0
void Compositor::applyLayerTreePacketDeltasToBackendMirror_REMOVED(std::uint64_t syncLaneId,
                                                            std::uint64_t syncPacketId,
                                                            BackendCompRenderTarget *target){
    if(syncLaneId == 0 || syncPacketId == 0){
        return;
    }

    LayerTreePacketMetadata metadata {};
    bool mirrorAppliedThisPacket = false;
    {
        std::lock_guard<std::mutex> lk(mutex);
        auto laneIt = layerTreePacketMetadata.find(syncLaneId);
        if(laneIt == layerTreePacketMetadata.end()){
            return;
        }
        auto packetIt = laneIt->second.find(syncPacketId);
        if(packetIt == laneIt->second.end()){
            return;
        }
        auto & packetMetadata = packetIt->second;
        if(!packetMetadata.mirrorApplied){
            for(auto & delta : packetMetadata.deltas){
                if(delta.tree == nullptr){
                    continue;
                }
                const char * deltaName = "unknown";
                switch(delta.type){
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
                auto & treeState = backendLayerMirror[delta.tree];
                if(delta.epoch < treeState.lastAppliedEpoch){
                    emitSyncTrace(
                            "deltaApplied nonMonotonicTreeEpoch lane=" + std::to_string(syncLaneId) +
                            " packet=" + std::to_string(syncPacketId) +
                            " tree=" + std::to_string(reinterpret_cast<std::uintptr_t>(delta.tree)) +
                            " expectedAtLeast=" + std::to_string(treeState.lastAppliedEpoch) +
                            " got=" + std::to_string(delta.epoch));
                    continue;
                }
                treeState.lastAppliedEpoch = std::max(treeState.lastAppliedEpoch,delta.epoch);
                switch(delta.type){
                    case LayerTreeDeltaType::TreeAttached: {
                        treeState.attached = true;
                        OmegaCommon::Vector<Layer *> treeLayers {};
                        collectLayersForTree(delta.tree,treeLayers);
                        for(auto *treeLayer : treeLayers){
                            if(treeLayer == nullptr){
                                continue;
                            }
                            auto & layerState = treeState.layers[treeLayer];
                            layerState.rect = sanitizeCommandRect(
                                    treeLayer->getLayerRect(),
                                    Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
                            layerState.enabled = true;
                            layerState.lastAppliedEpoch = std::max(layerState.lastAppliedEpoch,delta.epoch);
                        }
                        break;
                    }
                    case LayerTreeDeltaType::TreeDetached:
                        treeState.attached = false;
                        treeState.layers.clear();
                        break;
                    case LayerTreeDeltaType::LayerResized:
                    case LayerTreeDeltaType::LayerEnabled:
                    case LayerTreeDeltaType::LayerDisabled: {
                        if(delta.layer == nullptr){
                            break;
                        }
                        treeState.attached = true;
                        auto & layerState = treeState.layers[delta.layer];
                        layerState.rect = sanitizeCommandRect(
                                delta.rect,
                                sanitizeCommandRect(
                                        delta.layer->getLayerRect(),
                                        Core::Rect{Core::Position{0.f,0.f},1.f,1.f}));
                        if(delta.type == LayerTreeDeltaType::LayerEnabled){
                            layerState.enabled = true;
                        }
                        else if(delta.type == LayerTreeDeltaType::LayerDisabled){
                            layerState.enabled = false;
                        }
                        layerState.lastAppliedEpoch = std::max(layerState.lastAppliedEpoch,delta.epoch);
                        break;
                    }
                    default:
                        break;
                }
                emitSyncTrace(
                        std::string("deltaApplied type=") + deltaName +
                        " lane=" + std::to_string(syncLaneId) +
                        " packet=" + std::to_string(syncPacketId) +
                        " epoch=" + std::to_string(delta.epoch) +
                        " tree=" + std::to_string(reinterpret_cast<std::uintptr_t>(delta.tree)) +
                        " layer=" + std::to_string(reinterpret_cast<std::uintptr_t>(delta.layer)));
            }
            packetMetadata.mirrorApplied = true;
            mirrorAppliedThisPacket = true;
        }
        metadata = packetMetadata;
    }
    if(mirrorAppliedThisPacket){
        markPacketMirrorApplied(syncLaneId,syncPacketId);
        queueCondition.notify_all();
    }

    if(target == nullptr || target->visualTree == nullptr){
        return;
    }

    std::lock_guard<std::mutex> lk(mutex);
    for(auto & requiredEpoch : metadata.requiredEpochByTree){
        auto *tree = requiredEpoch.first;
        if(tree == nullptr){
            continue;
        }
        auto mirrorIt = backendLayerMirror.find(tree);
        if(mirrorIt == backendLayerMirror.end()){
            continue;
        }
        auto & treeState = mirrorIt->second;
        if(!treeState.attached){
            continue;
        }
        for(auto & layerStateEntry : treeState.layers){
            auto *layer = layerStateEntry.first;
            auto & layerState = layerStateEntry.second;
            if(layer == nullptr || !layerState.enabled){
                continue;
            }
            auto *surface = ensureLayerSurfaceTarget(*target,layer);
            if(surface == nullptr){
                continue;
            }
            auto rect = sanitizeCommandRect(
                    layerState.rect,
                    sanitizeCommandRect(layer->getLayerRect(),
                                        Core::Rect{Core::Position{0.f,0.f},1.f,1.f}));
            surface->setRenderTargetSize(rect);
        }
    }
}
#endif


void Compositor::executeCurrentCommand(){

    if(currentCommand->type == CompositorCommand::Render) {

        OMEGAWTK_DEBUG("Rendering Frame!!")
        
        auto comm = (CompositionRenderCommand *)currentCommand.get();


        BackendCompRenderTarget *target = nullptr;
        bool _buildVisualTree = false;

        /// 1. Locate / Create View Render Target for Layer Render

        auto found = renderTargetStore.store.find(comm->renderTarget);
        if (found == renderTargetStore.store.end()) {
             _buildVisualTree = true;
             auto *preCreated = PreCreatedResourceRegistry::lookup(comm->renderTarget.get());
             if(preCreated != nullptr && preCreated->bundle.visualTree != nullptr){
                 BackendCompRenderTarget compRenderTarget {preCreated->bundle.visualTree, {}, preCreated->presentTarget};
                 target = &renderTargetStore.store.insert(std::make_pair(comm->renderTarget,compRenderTarget)).first->second;
             } else {
                 std::cout << "[WTK Diag] executeCurrentCommand(Render): no pre-created visual tree for target "
                           << comm->renderTarget.get() << " — dropping frame" << std::endl;
                 markPacketDropped(currentCommand->syncLaneId,currentCommand->syncPacketId);
                 currentCommand->status.set(CommandStatus::Delayed);
                 return;
             }
        } else {
            target = &renderTargetStore.store[comm->renderTarget];
        };

        // Layer tree mirror application removed (Phase 2).
        // Native layer geometry is managed on the main thread.

        /// 2. Locate / Create Layer Render Target in Visual Tree.
        BackendRenderTargetContext *targetContext = nullptr;

        auto layer = comm->frame->targetLayer;
        auto layer_found = target->surfaceTargets.find(layer);
        if(layer_found == target->surfaceTargets.end() || layer_found->second == nullptr){
            auto * ensuredSurface = ensureLayerSurfaceTarget(*target,layer);
            if(ensuredSurface == nullptr){
                markPacketDropped(currentCommand->syncLaneId,currentCommand->syncPacketId);
                currentCommand->status.set(CommandStatus::Delayed);
                return;
            }
            layer_found = target->surfaceTargets.find(layer);
        }
        if(layer_found == target->surfaceTargets.end() || layer_found->second == nullptr){
            markPacketDropped(currentCommand->syncLaneId,currentCommand->syncPacketId);
            currentCommand->status.set(CommandStatus::Delayed);
            return;
        }

        targetContext = layer_found->second;
        // Use the frame's snapshot rect — not the live getLayerRect() —
        // so the render target and viewport match the draw commands that
        // were recorded at paint time.  The live layer rect may have
        // advanced due to subsequent resize events on the main thread.
        auto layerRect = sanitizeCommandRect(
                comm->frame->rect,
                Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
        if(target->visualTree != nullptr &&
           target->visualTree->root != nullptr &&
           targetContext == &(target->visualTree->root->renderTarget)){
            layerRect = normalizeRootVisualRect(layerRect);
        }
        targetContext->setRenderTargetSize(layerRect);

        OmegaCommon::ArrayRef<VisualCommand> commands{comm->frame->currentVisuals};

        /// TODO: Process render commands!

        auto & bkgrd = comm->frame->background;

        const bool isNoOpFrame =
                commands.empty() &&
                comm->frame->currentEffects.empty() &&
                bkgrd.r == 0.f &&
                bkgrd.g == 0.f &&
                bkgrd.b == 0.f &&
                bkgrd.a == 0.f;

        // Some clients can enqueue empty transparent frames during startup/layout.
        // Drop only when telemetry says there is no state/effect-only packet to preserve.
        if(isNoOpFrame){
            if(shouldDropNoOpTransparentFrame(currentCommand->syncLaneId,
                                              currentCommand->syncPacketId)){
                OMEGAWTK_DEBUG("Skipping no-op transparent frame.")
                markPacketDropped(currentCommand->syncLaneId,
                                  currentCommand->syncPacketId,
                                  PacketDropReason::NoOpTransparent);
            }
            else {
                OMEGAWTK_DEBUG("Completing no-op transparent frame for state/effect sync.")
                completePacketWithoutGpu(currentCommand->syncLaneId,
                                         currentCommand->syncPacketId);
            }
            currentCommand->status.set(CommandStatus::Ok);
            return;
        }

        {
            const bool sameTargetAsLast = (targetContext == s_lastClearedContext && comm->renderTarget.get() == s_lastClearedRenderTarget);
            if (!sameTargetAsLast) {
                targetContext->clear(bkgrd.r,bkgrd.g,bkgrd.b,bkgrd.a);
                s_lastClearedContext = targetContext;
                s_lastClearedRenderTarget = comm->renderTarget.get();
            }
        }

        for(auto & c : commands){
            targetContext->renderToTarget(c.type,(void *)&c.params);
        }

        for(auto & effect : comm->frame->currentEffects){
            targetContext->applyEffectToTarget(effect);
        }

        const auto submitTimeCpu = std::chrono::steady_clock::now();
        markPacketSubmitted(currentCommand->syncLaneId,currentCommand->syncPacketId,submitTimeCpu);
        auto weakTelemetryState = telemetryState();
        // #region agent log
        {
            std::ofstream f("../../../debug-85f774.log", std::ios::app);
            if (f) {
                auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"85f774\",\"location\":\"Execution.cpp:before_commit\",\"message\":\"before commit\",\"data\":{},\"timestamp\":" << ts << ",\"hypothesisId\":\"D\"}\n";
            }
        }
        // #endregion
        targetContext->commit(currentCommand->syncLaneId,
                              currentCommand->syncPacketId,
                              submitTimeCpu,
                              [weakTelemetryState](const BackendSubmissionTelemetry & telemetry){
                                  Compositor::onBackendSubmissionCompleted(weakTelemetryState,telemetry);
                              });
#ifdef _WIN32
        std::cout << "[WTK Diag] Execution: waitForGPU..." << std::endl;
        targetContext->waitForGPU();
        std::cout << "[WTK Diag] Execution: waitForGPU done" << std::endl;
#endif
        target->needsPresent = true;
        OMEGAWTK_DEBUG("Committed Data!")
    }
    else if(currentCommand->type == CompositorCommand::Layer){
        auto params = (CompositorLayerCommand *)currentCommand.get();
        if(params->layer == nullptr){
            markPacketFailed(currentCommand->syncLaneId,currentCommand->syncPacketId);
            currentCommand->status.set(CommandStatus::Failed);
            return;
        }
        /// Resize Command
        if(params->subtype == CompositorLayerCommand::Resize){
            auto priorRect = sanitizeCommandRect(
                    params->layer->getLayerRect(),
                    Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
            auto layerRect = priorRect;
            layerRect.pos.x += (float)params->delta_x;
            layerRect.pos.y += (float)params->delta_y;
            layerRect.w += (float)params->delta_w;;
            layerRect.h += (float)params->delta_h;
            layerRect = sanitizeCommandRect(layerRect,priorRect);
            params->layer->resize(layerRect);
        }
        else {
            // Layer effect commands (shadow, transform) are no longer applied via
            // native layer properties. Effects are now draw-time Canvas operations
            // (Phase 2 of the Unified Rendering Architecture Plan).
            // Effect commands are accepted but ignored until the draw-time path is implemented.
        }
    }
    else if(currentCommand->type == CompositorCommand::View){
        auto params = (CompositorViewCommand *)currentCommand.get();
        if(params->subType == CompositorViewCommand::Resize){
            auto currentRect = sanitizeCommandRect(
                    params->viewPtr->getRect(),
                    Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
            auto nextRect = currentRect;
            nextRect.pos.x += static_cast<float>(params->delta_x);
            nextRect.pos.y += static_cast<float>(params->delta_y);
            nextRect.w += static_cast<float>(params->delta_w);
            nextRect.h += static_cast<float>(params->delta_h);
            nextRect = sanitizeCommandRect(nextRect,currentRect);
            params->viewPtr->resize(nextRect);
        }
    }
    else if(currentCommand->type == CompositorCommand::Cancel){
        auto params = (CompositorCancelCommand *)currentCommand.get();
        /// Filter commands that have the id in order to cancel execution of them.
        std::lock_guard<std::mutex> lk(mutex);
        commandQueue.filter([&](SharedHandle<CompositorCommand> & command){
            return command->id >= params->startID && command->id <= params->endID;
        });
    }

    currentCommand->status.set(CommandStatus::Ok);
}

void Compositor::onQueueDrained(){
    renderTargetStore.presentAllPending();
    resetLastClearedForNextBatch();
}


};
