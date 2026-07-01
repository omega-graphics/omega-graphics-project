#include "Compositor.h"
#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/UI/AppWindow.h"  // AppWindow::isNativeReady() — NativeWindow-Ready-Signal-Plan §3.5(A)
#include "backend/ResourceFactory.h"
#include "backend/VisualBinder.h"
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
    // §2.14 Pass 1 retired the `BackendCompRenderTarget` /
    // `RenderTargetStore` cache that pre-§2.14 needed cleaning here
    // (those were the legacy per-Layer surface targets indexed by
    // CompositionRenderTarget). Native::VisualTree carries its own
    // teardown through `Compositor::detachVisualTree`, which is
    // called from `~AppWindow`. All that remains is observer
    // bookkeeping.
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
    // §2.14 Pass 1 retired RenderTargetStore (legacy per-Layer
    // BackendRenderTargetContext cache). nativeAttachedTrees_ takes
    // its place and is drained below.
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
        // §2.14 Pass 1: drop attached visual trees + their RTCs while
        // GTE is still alive. Same rationale as windowSurfaces_ above.
        nativeAttachedTrees_.clear();
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

void Compositor::attachVisualTree(SharedHandle<Native::VisualTree> tree,
                                  SharedHandle<CompositionRenderTarget> target){
    if(tree == nullptr || target == nullptr){
        return;
    }
    std::lock_guard<std::mutex> lk(mutex);
    auto & slot = nativeAttachedTrees_[target.get()];
    slot.tree = std::move(tree);
    // rootContext stays null until the first renderCompositeFrame
    // succeeds in binding — `tryBindRootVisual` returns nullptr while
    // the platform surface is unrealized (Linux X11 pre-show), and we
    // retry every frame until it lands.
    slot.rootContext.reset();
}

void Compositor::detachVisualTree(CompositionRenderTarget * target){
    if(target == nullptr){
        return;
    }
    // Quiesce the worker for this target before touching its rootContext.
    // drainWindowSurfaces holds frameProcessingMutex_ for the duration of
    // a frame iteration, so acquiring it here waits until any in-flight
    // renderCompositeFrame finishes. Once we hold it, no new frame work
    // can start until detach is done. Order is frameProcessingMutex_
    // BEFORE `mutex` to match drainWindowSurfaces.
    std::lock_guard<std::mutex> processLk(frameProcessingMutex_);
    std::lock_guard<std::mutex> lk(mutex);
    auto it = nativeAttachedTrees_.find(target);
    if(it == nativeAttachedTrees_.end()){
        return;
    }
    // Tear-down order: clear the Visual's onResize hook BEFORE
    // dropping the RTC, since the hook captures the RTC by raw
    // pointer (installed in the per-backend binder). Then drop the
    // RTC (releases GTE resources while GTE is still live), then
    // erase the map entry (which releases the SharedHandle to the
    // tree itself, last).
    if(it->second.tree != nullptr){
        if(auto *rootVisual = it->second.tree->rootVisual()){
            rootVisual->setOnResize(nullptr);
        }
    }
    it->second.rootContext.reset();
    nativeAttachedTrees_.erase(it);
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
    // Hold frameProcessingMutex_ across the entire processing loop so a
    // concurrent detachVisualTree blocks until the worker is done touching
    // the targets it snapshotted above. Must be acquired AFTER `mutex`
    // is released (see frameProcessingMutex_'s comment in Compositor.h).
    std::lock_guard<std::mutex> processLk(frameProcessingMutex_);
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
        // Native-Theme-Application-Plan Tier 2 (2026-07-01): the per-frame
        // clear value is the owning window's resolved surface color (set
        // by the Style phase). A null owner (tests / pre-back-edge) falls
        // back to opaque white — the old code cleared such frames to
        // transparent black, which on an RGBA swapchain reads as pitch
        // black; white is the benign default.
        Composition::Color surfaceColor {1.f, 1.f, 1.f, 1.f};
        if(appWindow != nullptr){
            surfaceColor = appWindow->surfaceColor();
        }
        renderCompositeFrame(entry.first, frame, surfaceColor);
    }
}

void Compositor::renderCompositeFrame(const SharedHandle<CompositionRenderTarget> & target,
                                      const SharedHandle<CompositeFrame> & frame,
                                      const Composition::Color & surfaceColor){
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

    // §2.14 Pass 1 — every backend uses the Native::VisualTree path.
    // The per-Visual RTC is owned by
    // `nativeAttachedTrees_[target].rootContext` and bound lazily by
    // the per-backend `tryBindRootVisual` (defined in
    // backend/<plat>/<Plat>VisualBinder). On Linux this can return
    // nullptr until the X11 Window realizes; we retry every frame
    // until the bind succeeds — the same loop the pre-§2.14
    // `resolveDeferredNativeTarget` ran. macOS / Win32 binders
    // succeed synchronously.
    BackendRenderTargetContext *targetContext = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex);
        auto nativeIt = nativeAttachedTrees_.find(target.get());
        if(nativeIt == nativeAttachedTrees_.end()){
            if(OmegaGTE::isDebugLayerEnabled()){
                std::cerr << "[WTK_RP] renderCompositeFrame: early-return (no Native::VisualTree attached for target)" << std::endl;
            }
            return;
        }
        auto & attached = nativeIt->second;
        if(attached.rootContext == nullptr && attached.tree != nullptr){
            attached.rootContext = tryBindRootVisual(*attached.tree);
        }
        if(attached.rootContext == nullptr){
            if(OmegaGTE::isDebugLayerEnabled()){
                std::cerr << "[WTK_RP] renderCompositeFrame: early-return (tryBindRootVisual returned null — surface not realized yet)" << std::endl;
            }
            return;
        }
        targetContext = attached.rootContext.get();
    }

    if(OmegaGTE::isDebugLayerEnabled()){
        std::cerr << "[WTK_RP] renderCompositeFrame: reached dispatch, slices=" << frame->slices.size() << std::endl;
    }

    // Native-Theme-Application-Plan Tier 2 (2026-07-01): the clear value
    // is the window's resolved surface color, passed in by the caller
    // from AppWindow::surfaceColor(). This replaces the old "first slice
    // with any non-zero background channel wins" heuristic — which was
    // dead (nothing ever populated slice.background, so it always cleared
    // to transparent black → pitch black on RGBA swapchains) and latently
    // fragile (a translucent non-root slice could have hijacked the
    // whole-window clear).
    targetContext->beginFrame(surfaceColor.r,surfaceColor.g,surfaceColor.b,surfaceColor.a);
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
