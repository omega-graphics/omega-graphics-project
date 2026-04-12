#include "../Compositor.h"
#include "VisualTree.h"
#include "ResourceFactory.h"
#include <algorithm>
#include <cassert>
#include <chrono>
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

    static inline Composition::Rect sanitizeCommandRect(const Composition::Rect & candidate,const Composition::Rect & fallback){
        Composition::Rect saneFallback = fallback;
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

        Composition::Rect sane = candidate;
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

    static inline Composition::Rect normalizeRootVisualRect(const Composition::Rect & rect){
        Composition::Rect normalized = rect;
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
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "[WTK Diag] ensureLayerSurface: reusing existing for layer=" << layer
                      << " isChild=" << layer->isChildLayer()
                      << " hasRoot=" << target.visualTree->hasRootVisual() << std::endl;
#endif
            return existing->second;
        }
#ifdef OMEGAWTK_TRACE_RENDER
        std::cout << "[WTK Diag] ensureLayerSurface: CREATING for layer=" << layer
                  << " isChild=" << layer->isChildLayer()
                  << " hasRoot=" << target.visualTree->hasRootVisual() << std::endl;
#endif

        if(layer->isChildLayer()){
            if(!target.visualTree->hasRootVisual()){
                auto treeRoot = layer->getParentTree()->getRootLayer();
                auto rootRect = sanitizeCommandRect(
                        treeRoot->getLayerRect(),
                        Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});
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
                    Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});
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
                    Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});
            layerRect = normalizeRootVisualRect(layerRect);
            rootTarget->setRenderTargetSize(layerRect);
            return inserted.first->second;
        }

        auto layerRect = sanitizeCommandRect(
                layer->getLayerRect(),
                Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});
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
                 if(preCreated->presentTarget.nativeTarget == nullptr){
                     preCreated->bundle.visualTree->resolveDeferredNativeTarget(preCreated->presentTarget);
                 }
                 BackendCompRenderTarget compRenderTarget {preCreated->bundle.visualTree, {}, preCreated->presentTarget};
                 target = &renderTargetStore.store.insert(std::make_pair(comm->renderTarget,compRenderTarget)).first->second;
             } else {
#ifdef OMEGAWTK_TRACE_RENDER
                 std::cout << "[WTK Diag] executeCurrentCommand(Render): no pre-created visual tree for target "
                           << comm->renderTarget.get() << " — dropping frame" << std::endl;
#endif
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
                Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});
        const bool isRootVisualContext = (target->visualTree != nullptr &&
                                          target->visualTree->root != nullptr &&
                                          targetContext == &(target->visualTree->root->renderTarget));
        if(isRootVisualContext){
            // Phase 3: all Views render into the root visual's shared
            // surface.  Use viewport override to position each View's
            // content within the window surface.  The backing surface
            // grows automatically if the window resizes.
            auto & offset = comm->frame->windowOffset;
            targetContext->setViewportOverride(offset.x,offset.y,layerRect.w,layerRect.h);
        } else {
            // Own surface (child layer).
            targetContext->clearViewportOverride();
            targetContext->setRenderTargetSize(layerRect);
        }

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

        targetContext->beginFrame(bkgrd.r, bkgrd.g, bkgrd.b, bkgrd.a);

        for(auto & c : commands){
            targetContext->renderToTarget(c.type,(void *)&c.params);
        }

        targetContext->endFrame();

        for(auto & effect : comm->frame->currentEffects){
            targetContext->applyEffectToTarget(effect);
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
#ifdef _WIN32
#ifdef OMEGAWTK_TRACE_RENDER
        std::cout << "[WTK Diag] Execution: waitForGPU..." << std::endl;
#endif
        targetContext->waitForGPU();
#ifdef OMEGAWTK_TRACE_RENDER
        std::cout << "[WTK Diag] Execution: waitForGPU done" << std::endl;
#endif
#endif
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
                    Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});
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
                    Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});
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
    // Phase A-1: presentation now happens directly in commit(), so
    // presentAllPending() is no longer needed.

    // Phase A surface consumption bridge: when the command queue is
    // drained, check registered window surfaces for pending composite
    // frames and render them.  Phase B replaces the scheduler loop to
    // consume surfaces directly on vsync.
    for(auto & [target, surface] : windowSurfaces_){
        if(surface != nullptr && surface->hasPendingUpdate()){
            auto frame = surface->consume();
            if(frame != nullptr){
                renderCompositeFrame(target, std::move(frame));
            }
        }
    }
}


};
