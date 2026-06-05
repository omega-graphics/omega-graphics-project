#ifndef OMEGAWTK_UI_APPWINDOWIMPL_H
#define OMEGAWTK_UI_APPWINDOWIMPL_H

#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/StyleSheet.h"
#include "omegaWTK/Native/NativeWindow.h"
#include "omegaWTK/Native/NativeVisualTree.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/CompositorSurface.h"
#include "omegaWTK/Composition/Layer.h"
#include "../Composition/backend/ResourceFactory.h"
#include "FrameBuilder.h"
#include "AnimationScheduler.h"

#include <memory>

namespace OmegaWTK {

struct AppWindow::Impl {
    Native::NWH nativeWindow;
    Native::NativeItemPtr rootNativeItem;
    SharedHandle<Composition::ViewRenderTarget> rootViewRenderTarget;
    Composition::CompositorClientProxy proxy;
    SharedHandle<AppWindowDelegate> delegate;
    SharedHandle<WidgetTreeHost> widgetTreeHost;
    Composition::Rect rect;
    SharedHandle<Menu> menu;
    SharedHandle<Composition::CompositorSurface> windowSurface;

    /// §2.14 Pass 1: per-window `Native::VisualTree`. Built in
    /// `setRootWidget` via `Native::make_native_visual_tree` and
    /// handed to the compositor via `Compositor::attachVisualTree`.
    /// `~AppWindow` detaches and drops it so the per-Visual
    /// `BackendRenderTargetContext` releases GTE resources while GTE
    /// is still live. Replaces the pre-§2.14
    /// `PreCreatedResourceRegistry` + `windowVisualTreeData` bundle.
    Native::NativeVisualTreePtr visualTree_;

    // Tier 3 Phase 3.0 / Tier 4 §4.2: window-scoped present-layer host.
    // Owned by the window for its full lifetime; resized in lockstep with
    // syncNativePresentLayer. (The companion `windowCanvas_` Canvas was
    // deleted in 4.2 — FrameBuilder packs DrawOps straight into the
    // CompositeFrame via the proxy; the window layer tree stays the
    // present-layer host until 4.8.)
    SharedHandle<Composition::LayerTree> windowLayerTree_;

    // Tier 3 Phase 3.1: window-level frame driver. Owned for the
    // AppWindow's lifetime.
    std::unique_ptr<FrameBuilder> frameBuilder_;

    // Tier 4 Phase 4.3 (Block 2): per-window animation runtime, a peer of
    // the FrameBuilder. Ticked once per frame from FrameBuilder::beginFrame.
    // Additive — the legacy ViewAnimator/LayerAnimator path still drives
    // all animation until 4.4.
    std::unique_ptr<AnimationScheduler> animationScheduler_;

    // Widget-View-Paint-Lifecycle-Plan Tier D / D6.2 (2026-06-03):
    // per-window style-sheet stack. The cascade in D6.3's
    // `StyleSheets::StyleResolver::resolve(node)` walks this top-to-
    // bottom; rules at the back of the vector beat earlier rules in
    // a specificity tie. Sheets are sharable across windows via
    // `SharedHandle`; the stack itself is per-window.
    OmegaCommon::Vector<SharedHandle<StyleSheets::StyleSheet>> styleSheets_;

    /// `screen` is the chosen target screen (§2.9). nullptr falls
    /// through to the backend's "primary monitor" default — the
    /// pre-§2.9 behavior — which is what the legacy `(rect, delegate)`
    /// ctor uses if `AppInst::inst()` isn't around to resolve the
    /// manager's default screen. The AppWindow ctor pre-translates
    /// `rectValue.pos` to virtual-screen absolute coordinates before
    /// reaching this constructor, so the rect handed to the native
    /// factory is uniform across backends.
    Impl(AppWindow & owner,
         Composition::Rect rectValue,
         AppWindowDelegate * delegateValue,
         const Native::NativeScreenDesc * screen):
        nativeWindow(Native::make_native_window(rectValue,&owner,screen)),
        rootNativeItem(nativeWindow->getRootView()),
        rootViewRenderTarget(new Composition::ViewRenderTarget(rootNativeItem)),
        proxy(rootViewRenderTarget),
        delegate(delegateValue),
        rect(rectValue),
        // Local-origin rect: per-view trees also place their root layer at
        // (0,0); the window's window-offset stays the responsibility of the
        // compositor's native present layer.
        windowLayerTree_(std::make_shared<Composition::LayerTree>(
            Composition::Rect{Composition::Point2D{0.f, 0.f},
                              rectValue.w, rectValue.h})){
        // Wire the root native view's event emitter to the AppWindow so
        // that input events (mouse, keyboard) from the single root native
        // view are routed through AppWindowDelegate → WidgetTreeHost hit
        // testing (Phase 2, Native View Architecture Plan).
        rootNativeItem->event_emitter = &owner;
        // §2.9: seed the logical→physical scale at construction so the
        // first frame renders at the right density. Pre-§2.9 this was
        // first set in `setRootWidget` from `nativeWindow->scaleFactor()`
        // — correct value but one frame late on mixed-DPI multi-monitor
        // setups. The setRootWidget seed is now a defensive re-set; with
        // the screen carried in here we know the scale before the visual
        // tree is built.
        if(screen != nullptr && rootViewRenderTarget != nullptr){
            rootViewRenderTarget->setRenderScale(screen->scaleFactor);
        }
    }
};

}

#endif
