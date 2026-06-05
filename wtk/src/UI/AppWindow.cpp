#include "AppWindowImpl.h"
#include "../Composition/Compositor.h"
#include "../Composition/backend/ResourceFactory.h"
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/App.h"
#include "omegaWTK/Native/NativeWindow.h"
#include "omegaWTK/Native/NativeDialog.h"
#include "WidgetTreeHost.h"

#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/UI/Menu.h"
#include "omegaWTK/UI/View.h"

#include <cmath>
#include <iostream>

namespace OmegaWTK {

#if defined(TARGET_MACOS)
namespace {
static inline bool resizeRectChanged(const Composition::Rect &lhs,const Composition::Rect &rhs){
    constexpr float kEpsilon = 0.25f;
    return std::fabs(lhs.pos.x - rhs.pos.x) > kEpsilon ||
           std::fabs(lhs.pos.y - rhs.pos.y) > kEpsilon ||
           std::fabs(lhs.w - rhs.w) > kEpsilon ||
           std::fabs(lhs.h - rhs.h) > kEpsilon;
}
}
#endif

namespace {

// §2.9 NativeScreen: the no-screen ctor delegates to the manager's
// configured default; AppInst is the canonical owner of the
// AppWindowManager. Every visual test wires through AppInst, so this
// is the live path. The null-AppInst branch is a defensive fallback
// for any future caller that constructs an AppWindow without setting
// up an AppInst — falls through to primaryScreen() so the window
// still lands on a valid display.
Native::NativeScreenDesc resolveDefaultScreen(){
    AppInst *app = AppInst::inst();
    if(app != nullptr && app->windowManager != nullptr){
        return app->windowManager->defaultScreen();
    }
    return Native::primaryScreen();
}

// §2.9: AppWindow ctors interpret `rect.pos` as screen-local DIPs and
// translate to virtual-screen absolute coordinates before reaching
// the backend. Applied uniformly to both ctors so the native factory
// sees one coordinate space regardless of how the AppWindow was
// constructed.
Composition::Rect translateRectToScreen(Composition::Rect rect,
                                        const Native::NativeScreenDesc & screen){
    rect.pos.x += screen.frame.pos.x;
    rect.pos.y += screen.frame.pos.y;
    return rect;
}

}

    AppWindow::AppWindow(Composition::Rect rect,AppWindowDelegate *delegate):
    AppWindow(rect, resolveDefaultScreen(), delegate){}

    AppWindow::AppWindow(Composition::Rect rect,
                         const Native::NativeScreenDesc & screen,
                         AppWindowDelegate *delegate):
    impl_(std::make_unique<Impl>(*this,translateRectToScreen(rect, screen),delegate,&screen)){
        // MessageBoxA(HWND_DESKTOP,"Create Window Layer!","NOTE",MB_OK);
        if(impl_->delegate != nullptr) {
            setReciever(impl_->delegate.get());
            impl_->delegate->window = this;
        }
        // Tier 3 Phase 3.1 / Tier 4 §4.2: stand up the FrameBuilder. (The
        // window Canvas it used to probe was deleted in 4.2 — FrameBuilder
        // packs DrawOps straight into the CompositeFrame via the proxy.)
        // It does not start bracketing paint passes until AppWindow-driven
        // entry points (displayRootWindow, dispatchResize*ToHosts) call
        // into it below.
        impl_->frameBuilder_ = std::make_unique<FrameBuilder>(*this);
        // Tier 4 Phase 4.3 (Block 2): stand up the per-window animation
        // scheduler as a peer of the FrameBuilder. Additive — the legacy
        // ViewAnimator/LayerAnimator path still drives every animation;
        // nothing reads the scheduler's side table until 4.4. FrameBuilder
        // ticks it once per frame (see beginFrame).
        impl_->animationScheduler_ = std::make_unique<AnimationScheduler>(*this);
        // Widget-View-Paint-Lifecycle-Plan Tier D / D7.5 (2026-06-04):
        // seat the user-agent default stylesheet at the bottom of the
        // cascade stack BEFORE the app sees the window. The cascade
        // walks `styleSheets_` front-to-back with later sheets winning
        // specificity ties (see `StyleResolver::apply`'s
        // `cascadeBeats` and the `sheetIndex` tiebreak), so installing
        // the UA sheet at index 0 makes it the lowest-priority sheet —
        // every app-added sheet and every widget's inline `Style`
        // overrides it. Pushed directly into `styleSheets_` rather
        // than via `addStyleSheet` because the latter calls
        // `applyCascadeChange()` which is wasted work during ctor
        // (no widget tree yet) — the first `setRootWidget` /
        // `displayRootWindow` runs the first cascade pass and picks
        // up the UA sheet naturally. Cached in a function-local
        // static so every AppWindow in the process shares the same
        // immutable handle (the sheet is read-only after build()).
        static const SharedHandle<StyleSheets::StyleSheet> kUserAgentSheet =
            StyleSheets::BuildUserAgentStyleSheet();
        if(kUserAgentSheet != nullptr){
            impl_->styleSheets_.push_back(kUserAgentSheet);
        }
    };

FrameBuilder * AppWindow::frameBuilder() const {
    return impl_->frameBuilder_.get();
}

Composition::LayerTree * AppWindow::windowLayerTree() const {
    return impl_->windowLayerTree_.get();
}

void AppWindow::setMenu(SharedHandle<Menu> & menu){
    impl_->menu = menu;
    impl_->nativeWindow->setMenu(impl_->menu->getNativeMenu());
};

void AppWindow::setTitle(OmegaCommon::StrRef title){
    impl_->nativeWindow->setTitle(title);
}

void AppWindow::setEnableWindowHeader(bool enable) {
    impl_->nativeWindow->setEnableWindowHeader(enable);
}

// void AppWindow::setLayerStyle(SharedHandle<Composition::WindowStyle> & style){
//     layer->setWindowStyle(style);
// };

// void AppWindow::setMenuStyle(SharedHandle<Composition::MenuStyle> & style){
//     menuStyle = style;
// };

void AppWindow::onThemeSet(Native::ThemeDesc &desc){
    if(impl_->widgetTreeHost != nullptr){
        impl_->widgetTreeHost->root->onThemeSetRecurse(desc);
    }
}

void AppWindow::requestFrame(){
    // Tier A: coalesced schedule of flushFrame() on the next run-loop
    // turn. The native window dedups bursts into one callback.
    if(impl_->nativeWindow != nullptr){
        impl_->nativeWindow->requestFrameFlush();
    }
}

void AppWindow::refresh(){
    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.4 (2026-06-04):
    // public idle-context entrypoint. Thin wrapper over `requestFrame()`
    // — the named-on-app-surface counterpart to the internal-leaning
    // name `Widget::invalidate` uses. The native-window request itself
    // coalesces, so multiple `refresh()` calls in a turn still collapse
    // to one paint.
    requestFrame();
}

void AppWindow::flushFrame(){
    // Tier A: one frame for all pending invalidations. Open a single
    // FrameBuilder ScopedFrame and repaint every dirty widget into it.
    if(impl_->widgetTreeHost == nullptr){
        return;
    }
    FrameBuilder::ScopedFrame frame(impl_->frameBuilder_.get());
    impl_->widgetTreeHost->paintDirty();
}

// ---------------------------------------------------------------
// Widget-View-Paint-Lifecycle-Plan Tier D / D6.2 (2026-06-03):
// per-window style-sheet stack. The stack is consulted by the D6.3
// `StyleSheets::StyleResolver`; mutating it dirties root Style so
// the next frame re-resolves cells against the new cascade and asks
// the window to flush.
// ---------------------------------------------------------------

namespace {

// Widget-View-Paint-Lifecycle-Plan Tier D / D7.1 (2026-06-04):
// Walk a Widget subtree, marking each widget's view Style|Layout|Paint
// dirty so the next FrameBuilder pass re-resolves every cascade
// against whatever cascade input just changed (sheet stack or
// `ThemeVars`). Pre-D7.1 the sheet-stack mutators only called
// `requestFrame()`, which is enough for the first frame after
// `setRootWidget` (every view starts dirty) but silently misses
// runtime sheet swaps and theme-var changes once views have gone
// clean — the `styleSubtree` walker in `FrameBuilder` gates on each
// view's *own* `dirtyBits() & Style`, so without a per-node dirty bit
// the cascade does not re-evaluate. Mirrors the shape of
// `Widget::onThemeSetRecurse`, minus the native `onThemeSet` hook
// dispatch (which only carries a `ThemeDesc` for OS light/dark
// observers — not relevant to the app-level style cascade).
//
// D7.2 fixup (2026-06-04): the original v1 of this helper called
// `widget->invalidate(ThemeChanged)` per widget — which marks the
// widget's MAIN view dirty but DOES NOT propagate down to sub-views
// created via `makeSubView<UIView>(...)`. `View::markDirty` only
// walks UP (OR-ing into ancestor `descendantDirty`), so sub-UIViews
// stayed clean and the FrameBuilder's `styleSubtree` walker
// skipped them (gated on `(self | desc) & Style`). The visible
// symptom: D7.2 transitions on sub-UIView-hosted elements (the
// canonical pattern: a `BlueRectWidget` with a `blue_rect_view`
// sub-UIView containing a `blue_rect` element) never fired,
// because the sub-UIView's `resolveStyles` never ran and
// `applyTransitions` never saw the cell change. This is D8 (b) —
// the "sub-UIView markDirty propagation sharp edge" — surfacing
// at runtime. Fix: walk the VIEW subtree at each widget (not just
// the widget tree) so every render node, including sub-UIViews,
// gets a fresh self-dirty bit.
void markViewSubtreeDirty(View & view){
    constexpr uint8_t kCascadeBits =
        View::Style | View::Layout | View::Paint;
    view.markDirty(kCascadeBits);
    for(auto * sv : view.subviews()){
        if(sv != nullptr){
            markViewSubtreeDirty(*sv);
        }
    }
}

void invalidateCascadeRecurse(Widget * widget){
    if(widget == nullptr){
        return;
    }
    // Walk the widget's full view subtree (main view + every
    // sub-view created via `makeSubView`). Replaces the pre-fixup
    // `widget->invalidate(ThemeChanged)` call, which only marked
    // the widget's main view.
    markViewSubtreeDirty(widget->viewRef());
    for(const auto & child : widget->childWidgets()){
        if(child != nullptr){
            invalidateCascadeRecurse(child.get());
        }
    }
}

} // namespace

void AppWindow::applyCascadeChange(){
    if(impl_->widgetTreeHost != nullptr && impl_->widgetTreeHost->root != nullptr){
        invalidateCascadeRecurse(impl_->widgetTreeHost->root.get());
    }
    // Defensive request — `Widget::invalidate` already routes through
    // `treeHost->requestFrame()` for every visited node, but a
    // pre-`setRootWidget` mutation (no tree, no view) still needs the
    // native side to schedule the next-frame turn so a subsequent
    // setRootWidget actually paints. `requestFrame` coalesces, so the
    // extra call is free when a tree exists.
    requestFrame();
}

void AppWindow::addStyleSheet(SharedHandle<StyleSheets::StyleSheet> sheet){
    if(sheet == nullptr){
        return;
    }
    impl_->styleSheets_.push_back(std::move(sheet));
    applyCascadeChange();
}

void AppWindow::removeStyleSheet(const SharedHandle<StyleSheets::StyleSheet> & sheet){
    auto & stack = impl_->styleSheets_;
    for(auto it = stack.begin(); it != stack.end(); ++it){
        if(*it == sheet){
            stack.erase(it);
            applyCascadeChange();
            return;
        }
    }
}

const OmegaCommon::Vector<SharedHandle<StyleSheets::StyleSheet>> &
AppWindow::styleSheets() const {
    return impl_->styleSheets_;
}

void AppWindow::setRootWidget(WidgetPtr widget){
    impl_->widgetTreeHost = WidgetTreeHost::Create();
    impl_->widgetTreeHost->setRoot(widget);
    // Tier 3 Phase 3.8: hand the WidgetTreeHost the window frame
    // driver. Pre-Tier-D/D1 `Widget::executePaint` bracketed each
    // paint with a `FrameBuilder::ScopedFrame`; after D1 the
    // ScopedFrame brackets live on `AppWindow::flushFrame` (deferred
    // path) and `WidgetTreeHost::paintDirty` (immediate path), and
    // every invalidate/init repaint routes through one of those two.
    impl_->widgetTreeHost->setFrameBuilder(impl_->frameBuilder_.get());
    // Tier A (Widget-View-Paint-Lifecycle): wire deferred invalidation.
    // The tree host routes requestFrame() back here; the native window
    // invokes flushFrame() once per coalesced run-loop turn.
    impl_->widgetTreeHost->setOwnerWindow(this);
    impl_->nativeWindow->setFrameFlushCallback([this]{ flushFrame(); });
    // Phase 3: propagate the window's single render target to the
    // WidgetTreeHost so it can be distributed to all Views during
    // initWidgetTree(). All Views in this window share this target.
    // Build the window's single backend visual tree and register it so
    // the compositor can resolve the window render target during draining.
    {
        Composition::Rect rootRect = impl_->rect;
        rootRect.pos = {0.f, 0.f};
        // Single source of truth for the window's logical->physical pixel
        // scale: the native window. Seed it onto the render target before
        // the backend visual tree is built so every backend reads it via
        // ViewRenderTarget::getRenderScale() rather than recomputing it.
        impl_->rootViewRenderTarget->setRenderScale(impl_->nativeWindow->scaleFactor());

        // §2.14 Pass 1 — every backend constructs the per-window
        // Native::VisualTree and hands it to the compositor. The
        // compositor lazily binds the root visual's RTC on first
        // render (via the per-backend `tryBindRootVisual`), retrying
        // every frame until the platform surface is realized
        // (relevant only on Linux X11; macOS / Win32 succeed
        // synchronously).
        impl_->visualTree_ = Native::make_native_visual_tree(
            impl_->nativeWindow->getRootView(),
            rootRect,
            impl_->nativeWindow->scaleFactor());
        if(impl_->visualTree_ != nullptr && impl_->widgetTreeHost->compositor != nullptr){
            impl_->widgetTreeHost->compositor->attachVisualTree(
                impl_->visualTree_,
                impl_->rootViewRenderTarget);
        }
    }
    impl_->widgetTreeHost->setWindowRenderTarget(impl_->rootViewRenderTarget);
    // Phase 5: provide the root native item so NativeViewHost can
    // embed real native views as children of the window's root view.
    impl_->widgetTreeHost->setRootNativeItem(impl_->nativeWindow->getRootView());
    impl_->proxy.setFrontendPtr(impl_->widgetTreeHost->compositor);
    // Tier 3 Phase 3.1/3.2: register the window-scoped layer tree
    // with the compositor frontend on the WidgetTreeHost's sync
    // lane. Without this, the slices FrameBuilder deposits into
    // the window CompositeFrame reference a layer tree the
    // compositor doesn't know about and never get rendered.
    impl_->proxy.setSyncLaneId(impl_->widgetTreeHost->laneId());
    if(impl_->windowLayerTree_ != nullptr){
        impl_->widgetTreeHost->compositor->observeLayerTree(
            impl_->windowLayerTree_.get(),
            impl_->widgetTreeHost->laneId());
    }
    // Phase A: create the per-window compositor surface mailbox and
    // wire it to both the WidgetTreeHost (deposit side) and the
    // Compositor (consumption side).
    impl_->windowSurface = SharedHandle<Composition::CompositorSurface>(
        new Composition::CompositorSurface());
    // NativeWindow-Ready-Signal-Plan step 3: set the surface's owner
    // back-pointer BEFORE registerWindowSurface. registerWindowSurface
    // calls notifyFrameDirty internally, and on GTK with an
    // already-realized window the worker can wake and drain before
    // this ctor returns — the gate check in drainWindowSurfaces must
    // find a non-null owner from the very first drain.
    impl_->windowSurface->setOwnerAppWindow(this);
    impl_->widgetTreeHost->setWindowSurface(impl_->windowSurface);
    impl_->widgetTreeHost->compPtr()->registerWindowSurface(
        impl_->rootViewRenderTarget, impl_->windowSurface);
    // NativeWindow-Ready-Signal-Plan step 3: register the
    // realize-wake. Once the native surface realizes (synchronous
    // fire on backends where it is already ready at this point — see
    // GTKAppWindow::onFirstRealize fast path), this re-flips
    // frameDirty_ so the worker drains again and the deferred initial
    // paint dispatches with isNativeReady() now true. notifyFrameDirty
    // is private on Compositor but AppWindow is a friend, and the
    // lambda inherits this member function's access rights.
    if(auto * comp = impl_->widgetTreeHost->compPtr()){
        impl_->nativeWindow->onFirstRealize([comp]{
            comp->notifyFrameDirty();
        });
    }
    impl_->widgetTreeHost->attachedToWindow = true;
};


SharedHandle<Native::NativeFSDialog> AppWindow::openFSDialog(const Native::NativeFSDialog::Descriptor & desc){
    return Native::NativeFSDialog::Create(desc,impl_->nativeWindow);
};

SharedHandle<Native::NativeNoteDialog> AppWindow::openNoteDialog(const Native::NativeNoteDialog::Descriptor & desc){
    return Native::NativeNoteDialog::Create(desc,impl_->nativeWindow);
};

SharedHandle<Native::NativeAlertDialog> AppWindow::openAlertDialog(const Native::NativeAlertDialog::Descriptor & desc){
    return Native::NativeAlertDialog::Create(desc,impl_->nativeWindow);
};

void AppWindow::close(){
    impl_->nativeWindow->close();
};

void AppWindow::minimize(){ impl_->nativeWindow->minimize(); }
void AppWindow::maximize(){ impl_->nativeWindow->maximize(); }
void AppWindow::restore(){ impl_->nativeWindow->restore(); }
void AppWindow::toggleFullscreen(){ impl_->nativeWindow->toggleFullscreen(); }
bool AppWindow::isNativeReady() const {
    // NativeWindow-Ready-Signal-Plan §3.5(A) pass-through. Returns the
    // base interface's default (true) if there is no NativeWindow, so
    // a partially-constructed AppWindow never blocks the gate.
    if(impl_ == nullptr || impl_->nativeWindow == nullptr){
        return true;
    }
    return impl_->nativeWindow->isNativeReady();
}

bool AppWindow::isMinimized() const { return impl_->nativeWindow->isMinimized(); }
bool AppWindow::isMaximized() const { return impl_->nativeWindow->isMaximized(); }
bool AppWindow::isFullscreen() const { return impl_->nativeWindow->isFullscreen(); }
bool AppWindow::isVisible() const { return impl_->nativeWindow->isVisible(); }
Composition::Rect AppWindow::getRect() const { return impl_->nativeWindow->getRect(); }
void AppWindow::setRect(const Composition::Rect & rect){ impl_->nativeWindow->setRect(rect); }
float AppWindow::scaleFactor() const { return impl_->nativeWindow->scaleFactor(); }
void AppWindow::setMinSize(float w, float h){ impl_->nativeWindow->setMinSize(w, h); }
void AppWindow::setMaxSize(float w, float h){ impl_->nativeWindow->setMaxSize(w, h); }
void AppWindow::setResizable(bool resizable){ impl_->nativeWindow->setResizable(resizable); }
void AppWindow::orderFront(){ impl_->nativeWindow->orderFront(); }
void AppWindow::orderBack(){ impl_->nativeWindow->orderBack(); }
void AppWindow::setOpacity(float alpha){ impl_->nativeWindow->setOpacity(alpha); }
float AppWindow::getOpacity() const { return impl_->nativeWindow->getOpacity(); }
bool AppWindow::isKeyWindow() const { return impl_->nativeWindow->isKeyWindow(); }
void AppWindow::becomeKeyWindow(){ impl_->nativeWindow->becomeKeyWindow(); }

Native::NativeScreenDesc AppWindow::currentScreen() const {
    return impl_->nativeWindow->currentScreen();
}

void AppWindow::moveToScreen(const Native::NativeScreenDesc & screen){
    // Preserve the window's logical offset within the source screen
    // on the destination screen. `nativeWindow->getRect()` is in
    // virtual-screen absolute coordinates; subtracting the source
    // screen's origin recovers the source-local position, which is
    // then re-anchored to the destination.
    const Composition::Rect curRect = impl_->nativeWindow->getRect();
    const Native::NativeScreenDesc src = impl_->nativeWindow->currentScreen();
    Composition::Rect dest = curRect;
    dest.pos.x = screen.frame.pos.x + (curRect.pos.x - src.frame.pos.x);
    dest.pos.y = screen.frame.pos.y + (curRect.pos.y - src.frame.pos.y);
    impl_->nativeWindow->setRect(dest);
}

AppWindow::~AppWindow(){
    std::cout << "Closing Window" << std::endl;
    // §2.14 Pass 1 — tell the compositor to drop the per-Visual RTC
    // (releases GTE resources) and clear the Visual's onResize hook
    // BEFORE the tree handle drops. Without this the compositor's
    // `nativeAttachedTrees_` map would keep the tree alive past
    // AppWindow's destruction, deferring GTE release until the
    // compositor's own shutdown.
    if(impl_->widgetTreeHost != nullptr &&
       impl_->widgetTreeHost->compositor != nullptr &&
       impl_->rootViewRenderTarget != nullptr){
        impl_->widgetTreeHost->compositor->detachVisualTree(impl_->rootViewRenderTarget.get());
    }
    impl_->visualTree_.reset();
    close();
};

AppWindowManager::AppWindowManager():rootWindow(nullptr){};

void AppWindowManager::setRootWindow(AppWindowPtr handle){
    rootWindow = handle;
};

void AppWindowManager::onThemeSet(Native::ThemeDesc & desc){
    rootWindow->onThemeSet(desc);
}

AppWindowPtr AppWindowManager::getRootWindow(){
    return rootWindow;
};

Native::NativeScreenDesc AppWindowManager::defaultScreen() const {
    // Lazy first-read: `Native::primaryScreen()` requires the platform
    // display layer to be live, which is not guaranteed at
    // AppWindowManager construction time on every backend (GTK needs
    // `gtk_init` to have run first). Deferring the resolution until
    // first read keeps the manager's ctor cheap and synchronous.
    if(!defaultScreenInitialized_){
        defaultScreen_ = Native::primaryScreen();
        defaultScreenInitialized_ = true;
    }
    return defaultScreen_;
}

void AppWindowManager::setDefaultScreen(const Native::NativeScreenDesc & screen){
    defaultScreen_ = screen;
    defaultScreenInitialized_ = true;
}

void AppWindowManager::displayRootWindow(){
    rootWindow->impl_->nativeWindow->initialDisplay();
    if(rootWindow->impl_->widgetTreeHost != nullptr){
        // Tier 3 Phase 3.1: bracket the initial widget-tree paint pass
        // with the window-level FrameBuilder session. Pre-Tier-D/D1
        // the per-widget `Widget::executePaint` opened nested ScopedFrames
        // inside this one; after D1 the only nested ScopedFrame is the
        // one `WidgetTreeHost::paintDirty` opens (called by `Widget::init`
        // during the initial-tree walk).
        FrameBuilder::ScopedFrame frame(rootWindow->frameBuilder());
        rootWindow->impl_->widgetTreeHost->initWidgetTree();
    }
};

void AppWindowManager::closeAllWindows(){
    if(rootWindow != nullptr){
        rootWindow->close();
        rootWindow.reset();
    }
};

// Phase 3 made View purely virtual — the per-View NativeItem was removed,
// so View::resize cannot reach the platform's presentation layer
// (CAMetalLayer / swap chain) directly. The window's root NativeItem is
// the only surviving handle to that layer, so AppWindowDelegate is
// responsible for keeping it in sync on every resize tick.
//
// The backend visual tree also needs an explicit resize: Layer::resize
// stores the new rect on the virtual layer but does not notify observers,
// and Compositor::layerHasResized is a no-op. So the only path that
// touches BackendRenderTargetContext from a window resize is this one.
// Without it, BackingTextureSet::applyViewportOverride (called per-slice
// during frame rendering) keeps the backing dimensions strictly growing
// and the tessellation context stays sized for the largest historical
// window — which is why content stretches sideways on shrink: the GPU
// still renders at the old larger logical size into the new smaller
// drawable.
void AppWindowDelegate::syncNativePresentLayer(const Composition::Rect & rect){
    if(window == nullptr || window->impl_ == nullptr){
        return;
    }
    auto & rootItem = window->impl_->rootNativeItem;
    if(rootItem != nullptr){
        float scale = 1.f;
        if(window->impl_->rootViewRenderTarget != nullptr){
            scale = window->impl_->rootViewRenderTarget->getRenderScale();
        }
        rootItem->resizeNativeLayer(rect, scale);
    }

    // §2.14 Pass 1 — every backend resizes through the
    // Native::VisualTree path. The tree's resize propagates through
    // `Visual::onResize_` to the RTC's `setRenderTargetSize`
    // (installed by the per-backend binder), which recomputes backing
    // dimensions for the next frame.
    if(window->impl_->visualTree_ != nullptr){
        Composition::Rect mutableRect = rect;
        mutableRect.pos = {0.f, 0.f};
        window->impl_->visualTree_->resize(mutableRect);
    }

    // Tier 3 Phase 3.0: keep the window-scoped LayerTree's root layer
    // sized in lockstep with the native present layer. Mirrors the
    // per-view tree resize path that View::resize runs today; without
    // this, the window Canvas would emit frames against a stale rect
    // once FrameBuilder (Phase 3.1) starts consuming it.
    auto & windowLayerTree = window->impl_->windowLayerTree_;
    if(windowLayerTree != nullptr && windowLayerTree->getRootLayer() != nullptr){
        Composition::Rect layerRect = rect;
        layerRect.pos = {0.f, 0.f};
        windowLayerTree->getRootLayer()->resize(layerRect);
    }
}

void AppWindowDelegate::dispatchResizeToHosts(const Composition::Rect & rect){
    window->impl_->rect = rect;
    syncNativePresentLayer(rect);
    if(window->impl_->widgetTreeHost != nullptr){
        // Tier 3 Phase 3.1: resize-driven repaints are AppWindow-owned
        // paint passes too — bracket them with the window FrameBuilder.
        FrameBuilder::ScopedFrame frame(window->frameBuilder());
        window->impl_->widgetTreeHost->notifyWindowResize(rect);
    }
}

void AppWindowDelegate::dispatchResizeBeginToHosts(const Composition::Rect & rect){
    window->impl_->rect = rect;
    syncNativePresentLayer(rect);
    if(window->impl_->widgetTreeHost != nullptr){
        FrameBuilder::ScopedFrame frame(window->frameBuilder());
        window->impl_->widgetTreeHost->notifyWindowResizeBegin(rect);
    }
}

void AppWindowDelegate::dispatchResizeEndToHosts(const Composition::Rect & rect){
    window->impl_->rect = rect;
    syncNativePresentLayer(rect);
    if(window->impl_->widgetTreeHost != nullptr){
        FrameBuilder::ScopedFrame frame(window->frameBuilder());
        window->impl_->widgetTreeHost->notifyWindowResizeEnd(rect);
    }
}

void AppWindowDelegate::onRecieveEvent(Native::NativeEventPtr event){
    switch (event->type) {
        case Native::NativeEvent::WindowWillClose: {
            windowWillClose(event);
            break;
        }
        case Native::NativeEvent::WindowWillStartResize: {
            auto *params = reinterpret_cast<Native::WindowWillResize *>(event->params);
            if(params != nullptr){
#if defined(TARGET_MACOS)
                if(params->generation != 0 &&
                   params->generation <= lastResizeBeginGeneration){
                    break;
                }
                if(params->generation != 0){
                    lastResizeBeginGeneration = params->generation;
                }
#endif
                windowWillResize(params->rect);
#if defined(TARGET_MACOS)
                pendingLiveResizeRect = params->rect;
                hasPendingLiveResize = true;
#endif
                if(!liveResizeActive){
                    dispatchResizeBeginToHosts(params->rect);
                    liveResizeActive = true;
                }
            }
            break;
        }
        case Native::NativeEvent::WindowWillResize : {
            Native::WindowWillResize *params = (Native::WindowWillResize *)event->params;
            if(params == nullptr){
                break;
            }
#if defined(TARGET_MACOS)
            if(params->generation != 0 &&
               params->generation <= lastDispatchedResizeGeneration){
                break;
            }
#endif
            // MessageBoxA(HWND_DESKTOP,"Window Will Resize","NOTE",MB_OK);
            windowWillResize(params->rect);
            if(!liveResizeActive){
                dispatchResizeBeginToHosts(params->rect);
                liveResizeActive = true;
            }
#if defined(TARGET_MACOS)
            if(params->generation != 0){
                lastDispatchedResizeGeneration = params->generation;
            }
            pendingLiveResizeRect = params->rect;
            hasPendingLiveResize = true;
            if(!hasLastDispatchedLiveResizeRect ||
               resizeRectChanged(pendingLiveResizeRect,lastDispatchedLiveResizeRect)){
                dispatchResizeToHosts(pendingLiveResizeRect);
                lastDispatchedLiveResizeRect = pendingLiveResizeRect;
                hasLastDispatchedLiveResizeRect = true;
                hasPendingLiveResize = false;
            }
#else
            dispatchResizeToHosts(params->rect);
#endif
            break;
        }
        case Native::NativeEvent::WindowScaleFactorChanged: {
            // Native-API-Completion-Proposal §2.2 consumer. The native side
            // (CocoaAppWindow / WinAppWindow / GTKAppWindow) emits this
            // when the window's combined logical→physical scale changes —
            // Win32 WM_DPICHANGED, macOS -windowDidChangeBackingProperties:,
            // GTK notify::scale-factor. The handler runs the three-step
            // coupling specified by UIView-Render-Redesign-Plan Phase F so
            // every DrawOp re-rasterizes at the new pixel density on the
            // next frame: (1) match the OS-suggested rect on Win32, (2)
            // propagate the new scale to the Compositor's
            // ViewRenderTarget, (3) reuse the resize ScopedFrame to force
            // a full-tree repaint.
            auto *params = static_cast<Native::WindowScaleFactorChangedParams *>(event->params);
            if(params == nullptr || window == nullptr || window->impl_ == nullptr){
                break;
            }
            // (1) Win32 WM_DPICHANGED carries a physical-size-preserving
            // rect; macOS / GTK leave this empty. The Win32 wndproc has
            // typically already issued SetWindowPos by the time we see
            // the event, but applying it through the NativeWindow API
            // here keeps the consumer portable and self-contained — any
            // backend that surfaces a suggested rect without applying
            // it gets the same behavior.
            if(params->suggestedRect.has_value() && window->impl_->nativeWindow != nullptr){
                window->impl_->nativeWindow->setRect(*params->suggestedRect);
            }
            // (2) Compositor scale propagation. ViewRenderTarget is the
            // single source of truth for "what scale is this window
            // rendering at"; every backend visual tree
            // (CALayerTree / DCVisualTree / VKVisualTree) reads
            // getRenderScale() at allocation time. Updating it before
            // dispatchResizeToHosts means syncNativePresentLayer's
            // resizeNativeLayer call below carries the new scale into
            // the native present layer in the same pass.
            //
            // Cross-plan: a follow-up ViewRenderTarget::scaleChanged()
            // (owned by DPI-Aware-Text-Plan.md "Per-monitor DPI
            // updates") is what re-runs
            // BackendRenderTargetContext::recomputeBackingDimensions
            // so the backing texture itself rebuilds at the new
            // pixel density. Until it lands the front-side scale is
            // updated correctly here but the backend may still
            // rasterize at the old backing extent — that final
            // coupling is the Phase F follow-up the §2.2 status block
            // calls out, not in this proposal's scope.
            if(window->impl_->rootViewRenderTarget != nullptr){
                window->impl_->rootViewRenderTarget->setRenderScale(params->newScale);
            }
            // (3) Force the full-tree repaint by routing through the
            // shared resize machinery — same dispatchResizeToHosts
            // ScopedFrame the resize handler opens, so the contract
            // ("every element re-rasterizes against fresh
            // renderScale_") is identical for resize and scale-change.
            // The rect is unchanged on a scale-only event (macOS /
            // GTK); on Win32 the suggestedRect application above has
            // already updated window->impl_->rect through the native
            // resize path. Re-read getRect() to pick up whatever the
            // platform settled on.
            const Composition::Rect currentRect =
                window->impl_->nativeWindow != nullptr
                    ? window->impl_->nativeWindow->getRect()
                    : window->impl_->rect;
            dispatchResizeToHosts(currentRect);
            break;
        }
        case Native::NativeEvent::WindowHasFinishedResize: {
#if defined(TARGET_MACOS)
            if(hasPendingLiveResize){
                dispatchResizeToHosts(pendingLiveResizeRect);
                lastDispatchedLiveResizeRect = pendingLiveResizeRect;
                hasLastDispatchedLiveResizeRect = true;
                hasPendingLiveResize = false;
                dispatchResizeEndToHosts(pendingLiveResizeRect);
            }
            else if(window != nullptr &&
                    (!hasLastDispatchedLiveResizeRect ||
                     resizeRectChanged(window->impl_->rect,lastDispatchedLiveResizeRect))){
                dispatchResizeToHosts(window->impl_->rect);
                dispatchResizeEndToHosts(window->impl_->rect);
            } else if(window != nullptr){
                dispatchResizeEndToHosts(window->impl_->rect);
            }
            hasLastDispatchedLiveResizeRect = false;
            lastResizeBeginGeneration = 0;
#else
            if(window != nullptr){
                dispatchResizeEndToHosts(window->impl_->rect);
            }
#endif
            liveResizeActive = false;
            break;
        }
        // Input events: route through WidgetTreeHost hit testing.
        case Native::NativeEvent::LMouseDown:
        case Native::NativeEvent::LMouseUp:
        case Native::NativeEvent::RMouseDown:
        case Native::NativeEvent::RMouseUp:
        case Native::NativeEvent::CursorEnter:
        case Native::NativeEvent::CursorExit:
        case Native::NativeEvent::CursorMove:
        case Native::NativeEvent::KeyDown:
        case Native::NativeEvent::KeyUp:
        case Native::NativeEvent::ScrollWheel: {
            if(window != nullptr && window->impl_->widgetTreeHost != nullptr){
                window->impl_->widgetTreeHost->dispatchInputEvent(event);
            }
            break;
        }
        default:
            break;
    }
};

void AppWindowDelegate::windowWillClose(Native::NativeEventPtr event){
    /// To Be Overrided by its sub-classes!
};

void AppWindowDelegate::windowWillResize(Composition::Rect & nRect){
    /// To Be Overrided by its sub-classes!
};

    


};
