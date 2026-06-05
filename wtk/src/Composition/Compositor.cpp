#include "Compositor.h"
#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/UI/AppWindow.h"  // AppWindow::isNativeReady() — NativeWindow-Ready-Signal-Plan §3.5(A)
#include "backend/ResourceFactory.h"
#include "backend/VisualTree.h"
#include "omegaGTE/GE.h"  // OmegaGTE::isDebugLayerEnabled() — gates [WTK_RP] traces
#include <cmath>
#include <iostream>
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
            if(OmegaGTE::isDebugLayerEnabled()){
                std::cerr << "[WTK_RP] CompositorFrameWorker: woken by frameDirty_, draining" << std::endl;
            }
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
    // Idempotent path — if AppInst::doShutdown already called
    // shutdown(), this collapses to clearing already-empty state.
    shutdown();
}

void Compositor::shutdown(){
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
    // windowSurfaces_ owns SharedHandle<CompositionRenderTarget> /
    // SharedHandle<CompositorSurface> entries that transitively
    // pin BackendRenderTargetContext-held vertex / param buffers
    // (D3D12MA-backed). The destructor used to leave this map
    // populated, relying on the surrounding C++ runtime to
    // destruct the std::map at atexit; with the singleton path
    // that runs after the D3D12MA allocator is gone. Explicitly
    // clear here so the SharedHandles drop while the allocator is
    // still alive.
    {
        std::lock_guard<std::mutex> lk(mutex);
        windowSurfaces_.clear();
    }
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
    if(OmegaGTE::isDebugLayerEnabled()){
        std::cerr << "[WTK_RP] drainWindowSurfaces: snapshotSize=" << snapshot.size() << std::endl;
    }
    for(auto & entry : snapshot){
        auto & surface = entry.second;
        const bool surfaceNull = (surface == nullptr);
        const bool pending = (!surfaceNull && surface->hasPendingUpdate());
        if(OmegaGTE::isDebugLayerEnabled()){
            std::cerr << "[WTK_RP] drainWindowSurfaces: surface={null=" << (surfaceNull ? 1 : 0)
                      << " hasPending=" << (pending ? 1 : 0) << "}" << std::endl;
        }
        if(surfaceNull || !pending){
            continue;
        }
        // NativeWindow-Ready-Signal-Plan step 3: gate consume() on
        // native-surface realization. If the AppWindow's native
        // surface is not yet realized, leave the frame in the
        // surface's pending slot (do NOT call consume — it would
        // advance consumedGeneration_ and lose the frame). The
        // AppWindow's onFirstRealize registration re-flips
        // frameDirty_ when realize fires, so the worker drains again
        // and this time the gate passes. A null ownerAppWindow
        // (e.g., surfaces registered before AppWindow back-edge
        // wiring lands or in tests) falls through to ready=true so
        // existing behavior is preserved.
        auto * appWindow = surface->ownerAppWindow();
        const bool nativeReady = (appWindow == nullptr) || appWindow->isNativeReady();
        if(OmegaGTE::isDebugLayerEnabled()){
            std::cerr << "[WTK_RP] drainWindowSurfaces: nativeReady=" << (nativeReady ? 1 : 0)
                      << " hasOwner=" << (appWindow != nullptr ? 1 : 0) << std::endl;
        }
        if(!nativeReady){
            continue;
        }
        auto frame = surface->consume();
        if(frame == nullptr){
            if(OmegaGTE::isDebugLayerEnabled()){
                std::cerr << "[WTK_RP] drainWindowSurfaces: consume() returned null, skipping" << std::endl;
            }
            continue;
        }
        renderCompositeFrame(entry.first, frame);
    }
}

void Compositor::renderCompositeFrame(const SharedHandle<CompositionRenderTarget> & target,
                                      const SharedHandle<CompositeFrame> & frame){
    if(OmegaGTE::isDebugLayerEnabled()){
        std::cerr << "[WTK_RP] renderCompositeFrame: target=" << (target == nullptr ? "null" : "ok")
                  << " frame=" << (frame == nullptr ? "null" : "ok")
                  << " slices=" << (frame == nullptr ? std::size_t{0} : frame->slices.size())
                  << std::endl;
    }
    if(target == nullptr || frame == nullptr || frame->slices.empty()){
        if(OmegaGTE::isDebugLayerEnabled()){
            std::cerr << "[WTK_RP] renderCompositeFrame: early-return (null target/frame or empty slices)" << std::endl;
        }
        return;
    }

    BackendCompRenderTarget *backendTarget = nullptr;
    auto found = renderTargetStore.store.find(target);
    if(found == renderTargetStore.store.end()){
        auto *preCreated = PreCreatedResourceRegistry::lookup(target.get());
        if(preCreated == nullptr || preCreated->bundle.visualTree == nullptr){
            if(OmegaGTE::isDebugLayerEnabled()){
                std::cerr << "[WTK_RP] renderCompositeFrame: early-return (preCreated=" << (preCreated ? "ok" : "null")
                          << " visualTree=" << (preCreated && preCreated->bundle.visualTree ? "ok" : "null") << ")" << std::endl;
            }
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
        if(OmegaGTE::isDebugLayerEnabled()){
            std::cerr << "[WTK_RP] renderCompositeFrame: early-return (backendTarget="
                      << (backendTarget ? "ok" : "null") << " visualTree="
                      << (backendTarget && backendTarget->visualTree ? "ok" : "null") << ")" << std::endl;
        }
        return;
    }

    // Root-visual creation is now eager (see NativeWindow-Ready-Signal-Plan
    // step 4): Metal / D3D12 build it synchronously inside
    // BackendResourceFactory::createVisualTreeForView, and Vulkan builds it
    // inside its visualTree's resolveDeferredNativeTarget the first time the
    // native surface is resolvable — which is guaranteed by the
    // isNativeReady() gate at drainWindowSurfaces. The old anchor-from-slice
    // fallback that lived here was a chicken-and-egg workaround for slice
    // routes that don't set slice.targetLayer (SVG/per-view canvas paths) —
    // delete-on-sight now that the root-visual contract is contract-strong.
    if(!backendTarget->visualTree->hasRootVisual()){
        if(OmegaGTE::isDebugLayerEnabled()){
            std::cerr << "[WTK_RP] renderCompositeFrame: early-return (hasRootVisual=false post-resolveDeferredNativeTarget — backend's eager creation path failed)" << std::endl;
        }
        return;
    }

    if(backendTarget->visualTree->root == nullptr){
        if(OmegaGTE::isDebugLayerEnabled()){
            std::cerr << "[WTK_RP] renderCompositeFrame: early-return (visualTree->root is null)" << std::endl;
        }
        return;
    }

    if(OmegaGTE::isDebugLayerEnabled()){
        std::cerr << "[WTK_RP] renderCompositeFrame: reached dispatch, slices=" << frame->slices.size() << std::endl;
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
        // Tier 4 (absolute-coords decision 2026-05-29): DrawOp geometry now
        // arrives in absolute window space (paint baked each view's
        // windowOffset into its coordinates), so there is no per-slice
        // viewport translation. The single window-wide viewport set at
        // beginFrame() (inactive override ⇒ full backing at origin) is
        // correct for every slice. The old per-slice setViewportOverride
        // never re-applied to the command buffer anyway (only begin() /
        // fence-restart call setViewports), which is why per-slice offsets
        // were silently dropped and all views rendered stacked at the
        // origin. slice.windowOffset is retained on the slice but only the
        // (dormant) blur path still consults it.
        //
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
            // Tier 4 §4.1: dispatch the slice's DrawOp DisplayList via the
            // Phase 4.0 renderToTarget(DrawOp::Type) switch (slice.commands
            // is no longer populated by the window paint path).
            for(auto & op : slice.ops.ops()){
                targetContext->renderToTarget(op.type,(void *)&op.params);
            }
        }
    }

    targetContext->endFrame();
    targetContext->clearViewportOverride();

    targetContext->commit();
}

}
