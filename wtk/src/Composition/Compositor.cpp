#include "Compositor.h"
#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "backend/ResourceFactory.h"
#include "backend/VisualTree.h"
#include <cmath>
#include <mutex>
#include <utility>

namespace OmegaWTK::Composition {

void Compositor::observeLayerTree(LayerTree *tree, std::uint64_t /*syncLaneId*/){
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

Compositor::LaneTelemetrySnapshot Compositor::getLaneTelemetrySnapshot(std::uint64_t syncLaneId) const {
    LaneTelemetrySnapshot snapshot {};
    snapshot.syncLaneId = syncLaneId;
    return snapshot;
}

CompositorFrameWorker::CompositorFrameWorker(Compositor * compositor):
compositor(compositor),
shutdown(false),
t([this](Compositor *compositor){
    while(true){
        bool frameWake = false;
        {
            std::unique_lock<std::mutex> lk(compositor->mutex);
            compositor->queueCondition.wait(lk,[&]{
                return shutdown || compositor->frameDirty_.load(std::memory_order_acquire);
            });
            if(shutdown){
                break;
            }
            if(compositor->frameDirty_.exchange(false, std::memory_order_acq_rel)){
                frameWake = true;
            }
        }
        if(frameWake){
            compositor->drainWindowSurfaces();
        }
    }
}, compositor){
}

void CompositorFrameWorker::shutdownAndJoin(){
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

CompositorFrameWorker::~CompositorFrameWorker(){
    shutdownAndJoin();
}

Compositor::Compositor():
frameWorker(this){
}

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
    frameWorker.shutdownAndJoin();
    if(gte.graphicsEngine != nullptr){
        gte.graphicsEngine->waitForGPUIdle();
    }
    renderTargetStore.store.clear();
}

void Compositor::registerWindowSurface(SharedHandle<CompositionRenderTarget> target,
                                       SharedHandle<CompositorSurface> surface){
    if(target == nullptr || surface == nullptr){
        return;
    }
    surface->setOnDeposit([this]{ notifyFrameDirty(); });
    {
        std::lock_guard<std::mutex> lk(mutex);
        windowSurfaces_[std::move(target)] = std::move(surface);
    }
    notifyFrameDirty();
}

void Compositor::notifyFrameDirty(){
    frameDirty_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lk(mutex);
    queueCondition.notify_all();
}

void Compositor::drainWindowSurfaces(){
    OmegaCommon::Vector<std::pair<SharedHandle<CompositionRenderTarget>, SharedHandle<CompositorSurface>>> snapshot;
    {
        std::lock_guard<std::mutex> lk(mutex);
        snapshot.reserve(windowSurfaces_.size());
        for(auto & entry : windowSurfaces_){
            snapshot.emplace_back(entry.first, entry.second);
        }
    }
    for(auto & entry : snapshot){
        auto & surface = entry.second;
        if(surface == nullptr || !surface->hasPendingUpdate()){
            continue;
        }
        auto frame = surface->consume();
        if(frame == nullptr){
            continue;
        }
        renderCompositeFrame(entry.first, frame);
    }
}

void Compositor::renderCompositeFrame(const SharedHandle<CompositionRenderTarget> & target,
                                      const SharedHandle<CompositeFrame> & frame){
    if(target == nullptr || frame == nullptr || frame->slices.empty()){
        return;
    }

    BackendCompRenderTarget *backendTarget = nullptr;
    auto found = renderTargetStore.store.find(target);
    if(found == renderTargetStore.store.end()){
        auto *preCreated = PreCreatedResourceRegistry::lookup(target.get());
        if(preCreated == nullptr || preCreated->bundle.visualTree == nullptr){
            return;
        }
        if(preCreated->presentTarget.nativeTarget == nullptr){
            preCreated->bundle.visualTree->resolveDeferredNativeTarget(preCreated->presentTarget);
        }
        BackendCompRenderTarget compRenderTarget {
            preCreated->bundle.visualTree,
            {},
            preCreated->presentTarget};
        backendTarget = &renderTargetStore.store.insert(
                std::make_pair(target,compRenderTarget)).first->second;
    } else {
        backendTarget = &found->second;
    }

    if(backendTarget == nullptr || backendTarget->visualTree == nullptr){
        return;
    }

    if(!backendTarget->visualTree->hasRootVisual()){
        Layer *anchorLayer = nullptr;
        for(auto & slice : frame->slices){
            if(slice.targetLayer != nullptr){
                anchorLayer = slice.targetLayer;
                break;
            }
        }
        if(anchorLayer == nullptr){
            return;
        }
        auto *tree = anchorLayer->getParentTree();
        if(tree == nullptr){
            return;
        }
        auto rootLayer = tree->getRootLayer();
        Composition::Rect rootRect{Composition::Point2D{0.f,0.f},1.f,1.f};
        if(rootLayer != nullptr){
            auto candidate = rootLayer->getLayerRect();
            if(std::isfinite(candidate.w) && candidate.w > 0.f &&
               std::isfinite(candidate.h) && candidate.h > 0.f){
                rootRect.w = candidate.w;
                rootRect.h = candidate.h;
            }
        }
        BackendResourceFactory factory;
        (void)factory.createRootVisual(*backendTarget->visualTree,
                                       rootRect,
                                       backendTarget->viewPresentTarget);
    }

    if(backendTarget->visualTree->root == nullptr){
        return;
    }

    BackendRenderTargetContext *targetContext = backendTarget->visualTree->root->renderTarget.get();

    float clearR = 0.f;
    float clearG = 0.f;
    float clearB = 0.f;
    float clearA = 0.f;
    for(auto & slice : frame->slices){
        if(slice.background.a > 0.f ||
           slice.background.r > 0.f ||
           slice.background.g > 0.f ||
           slice.background.b > 0.f){
            clearR = slice.background.r;
            clearG = slice.background.g;
            clearB = slice.background.b;
            clearA = slice.background.a;
            break;
        }
    }

    targetContext->beginFrame(clearR,clearG,clearB,clearA);
    targetContext->resetElementState();

    for(auto & slice : frame->slices){
        const float w = (std::isfinite(slice.bounds.w) && slice.bounds.w > 0.f) ? slice.bounds.w : 1.f;
        const float h = (std::isfinite(slice.bounds.h) && slice.bounds.h > 0.f) ? slice.bounds.h : 1.f;
        targetContext->setViewportOverride(slice.windowOffset.x,
                                           slice.windowOffset.y,
                                           w,
                                           h);
        // Phase 3: reset per-element transform/opacity at every slice
        // boundary. Without this, a `SetTransform` / `SetOpacity` left
        // dangling at the end of one slice's command stream silently bled
        // into the first draws of the next slice.
        targetContext->resetElementState();
        if(slice.targetLayer != nullptr && slice.targetLayer->hasBlur()){
            // Phase 2: per-layer blur. Route the slice through a scratch
            // surface and composite the blurred result onto the swap chain.
            targetContext->renderBlurredSlice(slice);
        }
        else {
            for(auto & cmd : slice.commands){
                targetContext->renderToTarget(cmd.type,(void *)&cmd.params);
            }
        }
    }

    targetContext->endFrame();
    targetContext->clearViewportOverride();

    targetContext->commit();
}

}
