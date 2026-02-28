#include "../Compositor.h"
#include "VisualTree.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
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

    static BackendRenderTargetContext * ensureLayerSurfaceTarget(BackendCompRenderTarget & target,Layer * layer){
        if(layer == nullptr || target.visualTree == nullptr){
            return nullptr;
        }

        auto existing = target.surfaceTargets.find(layer);
        if(existing != target.surfaceTargets.end() && existing->second != nullptr){
            return existing->second;
        }

        if(layer->isChildLayer()){
            if(!target.visualTree->hasRootVisual()){
                auto treeRoot = layer->getParentLimb()->getRootLayer();
                auto rootRect = sanitizeCommandRect(
                        treeRoot->getLayerRect(),
                        Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
                rootRect = normalizeRootVisualRect(rootRect);
                auto rootVisual = target.visualTree->makeVisual(rootRect,rootRect.pos);
                target.visualTree->setRootVisual(rootVisual);
                auto insertedRoot = target.surfaceTargets.insert(std::make_pair(treeRoot.get(),&rootVisual->renderTarget));
                if(!insertedRoot.second){
                    insertedRoot.first->second = &rootVisual->renderTarget;
                }
            }
            auto layerRect = sanitizeCommandRect(
                    layer->getLayerRect(),
                    Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
            auto visual = target.visualTree->makeVisual(layerRect,layerRect.pos);
            target.visualTree->addVisual(visual);
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
            target.visualTree->root->resize(layerRect);
            return inserted.first->second;
        }

        auto layerRect = sanitizeCommandRect(
                layer->getLayerRect(),
                Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
        layerRect = normalizeRootVisualRect(layerRect);
        auto visual = target.visualTree->makeVisual(layerRect,layerRect.pos);
        target.visualTree->setRootVisual(visual);
        auto inserted = target.surfaceTargets.insert(std::make_pair(layer,&visual->renderTarget));
        if(!inserted.second){
            inserted.first->second = &visual->renderTarget;
        }
        return inserted.first->second;
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

    static void resizeVisualForSurface(BackendCompRenderTarget & target,
                                       BackendRenderTargetContext *surface,
                                       const Core::Rect & rect){
        if(surface == nullptr || target.visualTree == nullptr){
            return;
        }
        if(target.visualTree->root != nullptr &&
           surface == &(target.visualTree->root->renderTarget)){
            auto mutableRect = rect;
            mutableRect = normalizeRootVisualRect(mutableRect);
            target.visualTree->root->resize(mutableRect);
            return;
        }
        for(auto & visual : target.visualTree->body){
            if(visual != nullptr && surface == &(visual->renderTarget)){
                auto mutableRect = rect;
                visual->resize(mutableRect);
                break;
            }
        }
    }
}

void Compositor::applyLayerTreePacketDeltasToBackendMirror(std::uint64_t syncLaneId,
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
                assert(delta.epoch >= treeState.lastAppliedEpoch);
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
                        collectLayersForTreeLimb(delta.tree,delta.tree->getTreeRoot(),treeLayers);
                        for(auto *treeLayer : treeLayers){
                            if(treeLayer == nullptr){
                                continue;
                            }
                            auto & layerState = treeState.layers[treeLayer];
                            layerState.rect = sanitizeCommandRect(
                                    treeLayer->getLayerRect(),
                                    Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
                            layerState.enabled = true;
                            assert(delta.epoch >= layerState.lastAppliedEpoch);
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
                        assert(delta.epoch >= layerState.lastAppliedEpoch);
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
            resizeVisualForSurface(*target,surface,rect);
        }
    }
}


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
             auto renderTarget = std::dynamic_pointer_cast<ViewRenderTarget>(comm->renderTarget);
             auto visualTree = BackendVisualTree::Create(renderTarget);
             BackendCompRenderTarget compRenderTarget {visualTree};
             target = &renderTargetStore.store.insert(std::make_pair(comm->renderTarget,compRenderTarget)).first->second;
        } else {
            target = &renderTargetStore.store[comm->renderTarget];
        };

        applyLayerTreePacketDeltasToBackendMirror(currentCommand->syncLaneId,
                                                  currentCommand->syncPacketId,
                                                  target);

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
        auto layerRect = sanitizeCommandRect(
                comm->frame->targetLayer->getLayerRect(),
                Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
        if(target->visualTree != nullptr &&
           target->visualTree->root != nullptr &&
           targetContext == &(target->visualTree->root->renderTarget)){
            layerRect = normalizeRootVisualRect(layerRect);
        }
        targetContext->setRenderTargetSize(layerRect);
        resizeVisualForSurface(*target,targetContext,layerRect);


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

        targetContext->clear(bkgrd.r,bkgrd.g,bkgrd.b,bkgrd.a);

        for(auto & c : commands){
            targetContext->renderToTarget(c.type,(void *)&c.params);
        }

        for(auto & effect : comm->frame->currentEffects){
            auto adaptedEffect = adaptCanvasEffectForLane(currentCommand->syncLaneId,effect);
            targetContext->applyEffectToTarget(adaptedEffect);
        }

        const auto submitTimeCpu = std::chrono::steady_clock::now();
        markPacketSubmitted(currentCommand->syncLaneId,currentCommand->syncPacketId,submitTimeCpu);
        auto weakTelemetryState = telemetryState();
        targetContext->commit(currentCommand->syncLaneId,
                              currentCommand->syncPacketId,
                              submitTimeCpu,
                              [weakTelemetryState](const BackendSubmissionTelemetry & telemetry){
                                  Compositor::onBackendSubmissionCompleted(weakTelemetryState,telemetry);
                              });
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
            BackendCompRenderTarget *viewTarget = nullptr;
            auto viewRenderTarget = renderTargetStore.store.find(params->parentTarget);
            if(viewRenderTarget == renderTargetStore.store.end()){
                auto parentRenderTarget = std::dynamic_pointer_cast<ViewRenderTarget>(params->parentTarget);
                if(parentRenderTarget == nullptr){
                    markPacketFailed(currentCommand->syncLaneId,currentCommand->syncPacketId);
                    currentCommand->status.set(CommandStatus::Failed);
                    return;
                }
                auto visualTree = BackendVisualTree::Create(parentRenderTarget);
                BackendCompRenderTarget compRenderTarget {visualTree};
                viewTarget = &renderTargetStore.store.insert(std::make_pair(params->parentTarget,compRenderTarget)).first->second;
            }
            else {
                viewTarget = &viewRenderTarget->second;
            }
            if(viewTarget == nullptr || viewTarget->visualTree == nullptr){
                markPacketDropped(currentCommand->syncLaneId,currentCommand->syncPacketId);
                currentCommand->status.set(CommandStatus::Delayed);
                return;
            }
            if(params->effect == nullptr){
                markPacketFailed(currentCommand->syncLaneId,currentCommand->syncPacketId);
                currentCommand->status.set(CommandStatus::Failed);
                return;
            }

            auto surfaceIt = viewTarget->surfaceTargets.find(params->layer);
            if(surfaceIt == viewTarget->surfaceTargets.end() || surfaceIt->second == nullptr){
                auto * ensuredSurface = ensureLayerSurfaceTarget(*viewTarget,params->layer);
                if(ensuredSurface == nullptr){
                    markPacketDropped(currentCommand->syncLaneId,currentCommand->syncPacketId);
                    currentCommand->status.set(CommandStatus::Delayed);
                    return;
                }
                surfaceIt = viewTarget->surfaceTargets.find(params->layer);
                if(surfaceIt == viewTarget->surfaceTargets.end() || surfaceIt->second == nullptr){
                    markPacketDropped(currentCommand->syncLaneId,currentCommand->syncPacketId);
                    currentCommand->status.set(CommandStatus::Delayed);
                    return;
                }
            }

            auto *s = surfaceIt->second;
            auto & visualTree = viewTarget->visualTree;
            bool applied = false;
            if(visualTree->root != nullptr &&
               s == &(visualTree->root->renderTarget)){
                if(params->effect->type == LayerEffect::DropShadow){
                    auto adaptedShadow = adaptDropShadowForLane(currentCommand->syncLaneId,
                                                                 params->effect->dropShadow);
                    visualTree->root->updateShadowEffect(adaptedShadow);
                }
                else {
                    visualTree->root->updateTransformEffect(params->effect->transform);
                }
                applied = true;
            }
            else {
                for(auto & v : visualTree->body){
                    if(v != nullptr && s == &(v->renderTarget)){
                        if(params->effect->type == LayerEffect::DropShadow){
                            auto adaptedShadow = adaptDropShadowForLane(currentCommand->syncLaneId,
                                                                         params->effect->dropShadow);
                            v->updateShadowEffect(adaptedShadow);
                        }
                        else {
                            v->updateTransformEffect(params->effect->transform);
                        }
                        applied = true;
                        break;
                    }
                }
            }
            if(!applied){
                markPacketDropped(currentCommand->syncLaneId,currentCommand->syncPacketId);
                currentCommand->status.set(CommandStatus::Delayed);
                return;
            }
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
};


};
