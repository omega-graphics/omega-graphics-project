#ifndef OMEGAWTK_UI_APPWINDOWIMPL_H
#define OMEGAWTK_UI_APPWINDOWIMPL_H

#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/Native/NativeWindow.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/CompositorSurface.h"
#include "../Composition/backend/ResourceFactory.h"

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

    Impl(AppWindow & owner,Composition::Rect rectValue,AppWindowDelegate * delegateValue):
        nativeWindow(Native::make_native_window(rectValue,&owner)),
        rootNativeItem(nativeWindow->getRootView()),
        rootViewRenderTarget(new Composition::ViewRenderTarget(rootNativeItem)),
        proxy(rootViewRenderTarget),
        delegate(delegateValue),
        rect(rectValue){
        // Wire the root native view's event emitter to the AppWindow so
        // that input events (mouse, keyboard) from the single root native
        // view are routed through AppWindowDelegate → WidgetTreeHost hit
        // testing (Phase 2, Native View Architecture Plan).
        rootNativeItem->event_emitter = &owner;
    }
};

}

#endif
