#include "omegaWTK/Native/NativeWindow.h"
#include "omegaWTK/Native/NativeEvent.h"
#include "omegaWTK/Native/NativeItem.h"
#include "NativePrivate/gtk/GTKItem.h"
#include "GTKApp.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace OmegaWTK::Native::GTK {

namespace {

static Composition::Rect sanitizeRect(const Composition::Rect &candidate,const Composition::Rect &fallback){
    Composition::Rect sane = candidate;
    if(!std::isfinite(sane.pos.x)){
        sane.pos.x = fallback.pos.x;
    }
    if(!std::isfinite(sane.pos.y)){
        sane.pos.y = fallback.pos.y;
    }
    if(!std::isfinite(sane.w) || sane.w <= 0.f){
        sane.w = fallback.w;
    }
    if(!std::isfinite(sane.h) || sane.h <= 0.f){
        sane.h = fallback.h;
    }
    sane.w = std::max(1.f,sane.w);
    sane.h = std::max(1.f,sane.h);
    return sane;
}

}

class GTKAppWindow : public NativeWindow {
    Composition::Rect rect;
    NativeEventEmitter *eventEmitter = nullptr;
    SharedHandle<GTKItem> rootView = nullptr;
    GtkWindow *window = nullptr;
    GtkWidget *windowRootBox = nullptr;
    GtkWidget *menuWidget = nullptr;
    guint resizeFinishDebounceSource = 0;

    void emitEvent(NativeEvent::EventType type,NativeEventParams params){
        if(eventEmitter == nullptr){
            if(params != nullptr){
                switch(type){
                    case NativeEvent::WindowWillResize:
                        delete reinterpret_cast<Native::WindowWillResize *>(params);
                        break;
                    default:
                        break;
                }
            }
            return;
        }
        eventEmitter->emit(NativeEventPtr(new NativeEvent(type,params)));
    }

    void queueResizeFinishedEvent(){
        if(resizeFinishDebounceSource != 0){
            g_source_remove(resizeFinishDebounceSource);
            resizeFinishDebounceSource = 0;
        }
        resizeFinishDebounceSource = g_timeout_add(120,[](gpointer data) -> gboolean {
            auto *self = static_cast<GTKAppWindow *>(data);
            self->resizeFinishDebounceSource = 0;
            self->emitEvent(NativeEvent::WindowHasFinishedResize,nullptr);
            return G_SOURCE_REMOVE;
        },this);
    }

    static gboolean onDeleteEvent(GtkWidget *widget,GdkEvent *event,gpointer data){
        (void)widget;
        (void)event;
        auto *self = static_cast<GTKAppWindow *>(data);
        self->emitEvent(NativeEvent::WindowWillClose,nullptr);
        return FALSE;
    }

    static gboolean onConfigureEvent(GtkWidget *widget,GdkEvent *event,gpointer data){
        (void)widget;
        auto *self = static_cast<GTKAppWindow *>(data);
        auto *configure = reinterpret_cast<GdkEventConfigure *>(event);
        if(configure == nullptr){
            return FALSE;
        }
        Composition::Rect resizeRect{
            Composition::Point2D{0.f,0.f},
            static_cast<float>(configure->width),
            static_cast<float>(configure->height)
        };
        self->rect = sanitizeRect(resizeRect,self->rect);
        if(self->rootView != nullptr){
            self->rootView->resize(self->rect);
        }
        self->emitEvent(NativeEvent::WindowWillResize,new Native::WindowWillResize(self->rect));
        self->queueResizeFinishedEvent();
        return FALSE;
    }

public:
    GTKAppWindow(Composition::Rect &rect,NativeEventEmitter *emitter):
    rect(sanitizeRect(rect,Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f})),
    eventEmitter(emitter){
        if(gdk_display_get_default() == nullptr){
            std::cerr << "[OmegaWTK][GTK] No active GDK display. Skipping native window creation." << std::endl;
            return;
        }
        auto *app = get_active_application();
        if(app != nullptr && g_application_get_is_registered(G_APPLICATION(app))){
            window = GTK_WINDOW(gtk_application_window_new(app));
        }
        else {
            window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
            if(app != nullptr){
                gtk_window_set_application(window,app);
            }
        }
        if(window != nullptr){
            g_object_add_weak_pointer(G_OBJECT(window),reinterpret_cast<gpointer *>(&window));
            gtk_window_set_default_size(GTK_WINDOW(window),(gint)this->rect.w,(gint)this->rect.h);
            gtk_window_move(GTK_WINDOW(window),(gint)this->rect.pos.x,(gint)this->rect.pos.y);
            windowRootBox = gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
            gtk_container_add(GTK_CONTAINER(window),windowRootBox);
            g_signal_connect(window,"delete-event",G_CALLBACK(onDeleteEvent),this);
            g_signal_connect(window,"configure-event",G_CALLBACK(onConfigureEvent),this);
        }

        auto nativeRootView = Native::make_native_item(this->rect,Native::Default,nullptr);
        rootView = std::dynamic_pointer_cast<GTKItem>(nativeRootView);
        if(windowRootBox != nullptr && rootView != nullptr && rootView->getWidget() != nullptr){
            gtk_box_pack_end(GTK_BOX(windowRootBox),rootView->getWidget(),TRUE,TRUE,0);
        }
    }

    NativeItemPtr getRootView() override {
        return std::static_pointer_cast<NativeItem>(rootView);
    }

    void enable() override {
        if(window != nullptr){
            gtk_widget_show(GTK_WIDGET(window));
        }
    }

    void disable() override {
        if(window != nullptr){
            gtk_widget_hide(GTK_WIDGET(window));
        }
    }

    void initialDisplay() override {
        if(window != nullptr){
            gtk_widget_show_all(GTK_WIDGET(window));
        }
    }

    void close() override {
        if(window != nullptr){
            gtk_window_close(GTK_WINDOW(window));
        }
    }

    void setMenu(NM menu) override {
        this->menu = menu;
        if(windowRootBox == nullptr){
            return;
        }
        if(menuWidget != nullptr){
            auto *parent = gtk_widget_get_parent(menuWidget);
            if(parent != nullptr && GTK_IS_CONTAINER(parent)){
                gtk_container_remove(GTK_CONTAINER(parent),menuWidget);
            }
            menuWidget = nullptr;
        }
        if(this->menu == nullptr){
            return;
        }
        auto *binding = static_cast<GtkWidget *>(this->menu->getNativeBinding());
        if(binding == nullptr){
            return;
        }
        auto *bindingParent = gtk_widget_get_parent(binding);
        if(bindingParent != nullptr && GTK_IS_CONTAINER(bindingParent)){
            gtk_container_remove(GTK_CONTAINER(bindingParent),binding);
        }
        menuWidget = binding;
        gtk_box_pack_start(GTK_BOX(windowRootBox),menuWidget,FALSE,FALSE,0);
        gtk_box_reorder_child(GTK_BOX(windowRootBox),menuWidget,0);
        gtk_widget_show(menuWidget);
    }

    void setTitle(OmegaCommon::StrRef title) override {
        if(window == nullptr){
            return;
        }
        OmegaCommon::String windowTitle(title);
        gtk_window_set_title(GTK_WINDOW(window),windowTitle.c_str());
    }

    void setEnableWindowHeader(bool &enable) override {
        if(window != nullptr){
            gtk_window_set_decorated(GTK_WINDOW(window),enable ? TRUE : FALSE);
        }
    }

    ~GTKAppWindow() {
        if(resizeFinishDebounceSource != 0){
            g_source_remove(resizeFinishDebounceSource);
            resizeFinishDebounceSource = 0;
        }
        if(window != nullptr){
            g_object_remove_weak_pointer(G_OBJECT(window),reinterpret_cast<gpointer *>(&window));
            gtk_widget_destroy(GTK_WIDGET(window));
            window = nullptr;
        }
        windowRootBox = nullptr;
        menuWidget = nullptr;
        rootView = nullptr;
        eventEmitter = nullptr;
    }
};

}

namespace OmegaWTK::Native {

NWH make_native_window(Composition::Rect &rect,NativeEventEmitter *emitter){
    return (NWH)new GTK::GTKAppWindow(rect,emitter);
}

}
