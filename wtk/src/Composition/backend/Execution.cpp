#include "../Compositor.h"
#include "VisualTree.h"
#include <utility>

namespace OmegaWTK::Composition {

namespace {
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
                auto rootRect = treeRoot->getLayerRect();
                auto rootVisual = target.visualTree->makeVisual(rootRect,rootRect.pos);
                target.visualTree->setRootVisual(rootVisual);
                auto insertedRoot = target.surfaceTargets.insert(std::make_pair(treeRoot.get(),&rootVisual->renderTarget));
                if(!insertedRoot.second){
                    insertedRoot.first->second = &rootVisual->renderTarget;
                }
            }
            auto layerRect = layer->getLayerRect();
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
            auto layerRect = layer->getLayerRect();
            rootTarget->setRenderTargetSize(layerRect);
            target.visualTree->root->resize(layerRect);
            return inserted.first->second;
        }

        auto layerRect = layer->getLayerRect();
        auto visual = target.visualTree->makeVisual(layerRect,layerRect.pos);
        target.visualTree->setRootVisual(visual);
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
             auto renderTarget = std::dynamic_pointer_cast<ViewRenderTarget>(comm->renderTarget);
             auto visualTree = BackendVisualTree::Create(renderTarget);
             BackendCompRenderTarget compRenderTarget {visualTree};
             target = &renderTargetStore.store.insert(std::make_pair(comm->renderTarget,compRenderTarget)).first->second;
        } else {
            target = &renderTargetStore.store[comm->renderTarget];
        };

        /// 2. Locate / Create Layer Render Target in Visual Tree.
        BackendRenderTargetContext *targetContext;

        auto layer = comm->frame->targetLayer;
        auto layer_found = target->surfaceTargets.find(layer);
        if(layer_found == target->surfaceTargets.end()){
            if(layer->isChildLayer()){
                if(!target->visualTree->hasRootVisual()){
                    auto treeRoot = layer->getParentLimb()->getRootLayer();
                    auto root_v = target->visualTree->makeVisual(treeRoot->getLayerRect(),treeRoot->getLayerRect().pos);
                    target->visualTree->setRootVisual(root_v);
                    auto rootLayer = treeRoot.get();
                    auto root_surface = target->surfaceTargets.find(rootLayer);
                    if(root_surface == target->surfaceTargets.end()){
                        target->surfaceTargets.insert(std::make_pair(rootLayer,&root_v->renderTarget));
                    }
                    else {
                        root_surface->second = &root_v->renderTarget;
                    }
                }
                auto v = target->visualTree->makeVisual(layer->getLayerRect(),layer->getLayerRect().pos);
                target->visualTree->addVisual(v);
                auto inserted = target->surfaceTargets.insert(std::make_pair(layer,&v->renderTarget));
                if(!inserted.second){
                    inserted.first->second = &v->renderTarget;
                }
                targetContext = inserted.first->second;
            }
            else {
                if(target->visualTree->root != nullptr){
                    targetContext = &(target->visualTree->root->renderTarget);
                    auto inserted = target->surfaceTargets.insert(std::make_pair(layer,targetContext));
                    if(!inserted.second){
                        inserted.first->second = targetContext;
                    }
                    auto layerRect = layer->getLayerRect();
                    targetContext->setRenderTargetSize(layerRect);
                    target->visualTree->root->resize(layerRect);
                }
                else {
                    auto v = target->visualTree->makeVisual(layer->getLayerRect(),layer->getLayerRect().pos);
                    target->visualTree->setRootVisual(v);
                    auto inserted = target->surfaceTargets.insert(std::make_pair(layer,&v->renderTarget));
                    if(!inserted.second){
                        inserted.first->second = &v->renderTarget;
                    }
                    targetContext = inserted.first->second;
                }
            }
        }
        else {
            targetContext = layer_found->second;
            auto layerRect = comm->frame->targetLayer->getLayerRect();
            targetContext->setRenderTargetSize(layerRect);

            auto surface = layer_found->second;
            if(target->visualTree->root != nullptr &&
               surface == &(target->visualTree->root->renderTarget)){
                target->visualTree->root->resize(layerRect);
            }
            else {
                for(auto & visual : target->visualTree->body){
                    if(visual != nullptr && surface == &(visual->renderTarget)){
                        visual->resize(layerRect);
                        break;
                    }
                }
            }
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
        // Skipping them prevents wiping the last presented frame.
        if(isNoOpFrame){
            OMEGAWTK_DEBUG("Skipping no-op transparent frame.")
            markPacketDropped(currentCommand->syncLaneId,
                              currentCommand->syncPacketId,
                              PacketDropReason::NoOpTransparent);
            currentCommand->status.set(CommandStatus::Ok);
            return;
        }

        targetContext->clear(bkgrd.r,bkgrd.g,bkgrd.b,bkgrd.a);

        for(auto & c : commands){
            targetContext->renderToTarget(c.type,(void *)&c.params);
        }

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
            auto layerRect = params->layer->getLayerRect();
            layerRect.pos.x += (float)params->delta_x;
            layerRect.pos.y += (float)params->delta_y;
            layerRect.w += (float)params->delta_w;;
            layerRect.h += (float)params->delta_h;
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
                    visualTree->root->updateShadowEffect(params->effect->dropShadow);
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
                            v->updateShadowEffect(params->effect->dropShadow);
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
            auto rect = params->viewPtr->getRect();
            params->viewPtr->resize(Core::Rect {Core::Position {rect.pos.x + params->delta_x,rect.pos.y + params->delta_y},rect.w + params->delta_w,rect.h + params->delta_h});
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
