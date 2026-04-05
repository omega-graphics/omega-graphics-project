#ifndef OMEGAWTK_UI_APPWINDOWIMPL_H
#define OMEGAWTK_UI_APPWINDOWIMPL_H

#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/Native/NativeWindow.h"

namespace OmegaWTK {

struct AppWindow::Impl {
    UniqueHandle<Composition::WindowLayer> layer;
    SharedHandle<Composition::ViewRenderTarget> rootViewRenderTarget;
    Composition::CompositorClientProxy proxy;
    SharedHandle<AppWindowDelegate> delegate;
    SharedHandle<WidgetTreeHost> widgetTreeHost;
    Core::Rect rect;
    SharedHandle<Menu> menu;

    Impl(AppWindow & owner,Core::Rect rectValue,AppWindowDelegate * delegateValue):
        layer(std::make_unique<Composition::WindowLayer>(
                rectValue,
                Native::make_native_window(rectValue,&owner))),
        rootViewRenderTarget(new Composition::ViewRenderTarget(layer->native_window_ptr->getRootView())),
        proxy(rootViewRenderTarget),
        delegate(delegateValue),
        rect(rectValue){
    }
};

}

#endif
