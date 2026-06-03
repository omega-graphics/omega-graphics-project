#ifndef OMEGAWTK_UI_APPWINDOWIMPL_H
#define OMEGAWTK_UI_APPWINDOWIMPL_H

#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/StyleSheet.h"
#include "omegaWTK/Native/NativeWindow.h"
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
    Composition::PreCreatedVisualTreeData windowVisualTreeData;

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

    Impl(AppWindow & owner,Composition::Rect rectValue,AppWindowDelegate * delegateValue):
        nativeWindow(Native::make_native_window(rectValue,&owner)),
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
    }
};

}

#endif
