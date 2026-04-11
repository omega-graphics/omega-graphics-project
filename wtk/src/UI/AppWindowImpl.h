#ifndef OMEGAWTK_UI_APPWINDOWIMPL_H
#define OMEGAWTK_UI_APPWINDOWIMPL_H

#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/Native/NativeWindow.h"
#include "omegaWTK/Composition/CompositorClient.h"

namespace OmegaWTK {

struct AppWindow::Impl {
    Native::NWH nativeWindow;
    SharedHandle<Composition::ViewRenderTarget> rootViewRenderTarget;
    Composition::CompositorClientProxy proxy;
    SharedHandle<AppWindowDelegate> delegate;
    SharedHandle<WidgetTreeHost> widgetTreeHost;
    Composition::Rect rect;
    SharedHandle<Menu> menu;

    Impl(AppWindow & owner,Composition::Rect rectValue,AppWindowDelegate * delegateValue):
        nativeWindow(Native::make_native_window(rectValue,&owner)),
        rootViewRenderTarget(new Composition::ViewRenderTarget(nativeWindow->getRootView())),
        proxy(rootViewRenderTarget),
        delegate(delegateValue),
        rect(rectValue){
        // Wire the root native view's event emitter to the AppWindow so
        // that input events (mouse, keyboard) from the single root native
        // view are routed through AppWindowDelegate → WidgetTreeHost hit
        // testing (Phase 2, Native View Architecture Plan).
        nativeWindow->getRootView()->event_emitter = &owner;
    }
};

}

#endif
