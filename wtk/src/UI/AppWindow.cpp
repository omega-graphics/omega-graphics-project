#include "AppWindowImpl.h"
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/Native/NativeWindow.h"
#include "omegaWTK/Native/NativeDialog.h"
#include "WidgetTreeHost.h"

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

    AppWindow::AppWindow(Composition::Rect rect,AppWindowDelegate *delegate):
    impl_(std::make_unique<Impl>(*this,rect,delegate)){
        // MessageBoxA(HWND_DESKTOP,"Create Window Layer!","NOTE",MB_OK);
        if(impl_->delegate != nullptr) {
            setReciever(impl_->delegate.get());
            impl_->delegate->window = this;
        }
    };

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

void AppWindow::setRootWidget(WidgetPtr widget){
    impl_->widgetTreeHost = WidgetTreeHost::Create();
    impl_->widgetTreeHost->setRoot(widget);
    // Phase 3: propagate the window's single render target to the
    // WidgetTreeHost so it can be distributed to all Views during
    // initWidgetTree(). All Views in this window share this target.
    impl_->widgetTreeHost->setWindowRenderTarget(impl_->rootViewRenderTarget);
    // Phase 5: provide the root native item so NativeViewHost can
    // embed real native views as children of the window's root view.
    impl_->widgetTreeHost->setRootNativeItem(impl_->nativeWindow->getRootView());
    impl_->proxy.setFrontendPtr(impl_->widgetTreeHost->compositor);
    impl_->widgetTreeHost->attachedToWindow = true;
};


SharedHandle<Native::NativeFSDialog> AppWindow::openFSDialog(const Native::NativeFSDialog::Descriptor & desc){
    return Native::NativeFSDialog::Create(desc,impl_->nativeWindow);
};

SharedHandle<Native::NativeNoteDialog> AppWindow::openNoteDialog(const Native::NativeNoteDialog::Descriptor & desc){
    return Native::NativeNoteDialog::Create(desc,impl_->nativeWindow);
};

void AppWindow::close(){
    impl_->nativeWindow->close();
};

AppWindow::~AppWindow(){
    std::cout << "Closing Window" << std::endl;
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

void AppWindowManager::displayRootWindow(){
    rootWindow->impl_->nativeWindow->initialDisplay();
    if(rootWindow->impl_->widgetTreeHost != nullptr){
        rootWindow->impl_->widgetTreeHost->initWidgetTree();
    }
};

void AppWindowManager::closeAllWindows(){
    if(rootWindow != nullptr){
        rootWindow->close();
        rootWindow.reset();
    }
};

void AppWindowDelegate::dispatchResizeToHosts(const Composition::Rect & rect){
    window->impl_->rect = rect;
    if(window->impl_->widgetTreeHost != nullptr){
        window->impl_->widgetTreeHost->notifyWindowResize(rect);
    }
}

void AppWindowDelegate::dispatchResizeBeginToHosts(const Composition::Rect & rect){
    window->impl_->rect = rect;
    if(window->impl_->widgetTreeHost != nullptr){
        window->impl_->widgetTreeHost->notifyWindowResizeBegin(rect);
    }
}

void AppWindowDelegate::dispatchResizeEndToHosts(const Composition::Rect & rect){
    window->impl_->rect = rect;
    if(window->impl_->widgetTreeHost != nullptr){
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
