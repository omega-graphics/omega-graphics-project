#ifndef OMEGAWTK_UI_APPWINDOWIMPL_H
#define OMEGAWTK_UI_APPWINDOWIMPL_H

#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/Native/NativeWindow.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/CompositorSurface.h"
#include "omegaWTK/Composition/Layer.h"
#include "../Composition/backend/ResourceFactory.h"
#include "FrameBuilder.h"

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

    // Tier 3 Phase 3.0: window-scoped composition target. Owned by the
    // window for its full lifetime; resized in lockstep with
    // syncNativePresentLayer. Dormant until FrameBuilder (Phase 3.1)
    // routes per-view DrawOps through `windowCanvas_`.
    SharedHandle<Composition::LayerTree> windowLayerTree_;
    SharedHandle<Composition::Canvas>    windowCanvas_;

    // Tier 3 Phase 3.1: window-level frame driver. Constructed after
    // windowCanvas_ in AppWindow's ctor body so beginFrame/endFrame
    // can probe the canvas state. Owned for the AppWindow's lifetime.
    std::unique_ptr<FrameBuilder> frameBuilder_;

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
