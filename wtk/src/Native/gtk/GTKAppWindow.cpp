#include "NativePrivate/gtk/GTKItem.h"
#include "GTKApp.h"

// X11's <X.h> (pulled in transitively via GTK) defines the macro
// `CursorShape` which collides with OmegaWTK::Native::CursorShape.
// Undef it before including OmegaWTK headers.
#ifdef CursorShape
#undef CursorShape
#endif

#include "omegaWTK/Native/NativeWindow.h"
#include "omegaWTK/Native/NativeEvent.h"
#include "omegaWTK/Native/NativeItem.h"

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

static const char *cursorNameForShape(CursorShape shape){
    switch(shape){
        case CursorShape::Arrow:           return "default";
        case CursorShape::IBeam:           return "text";
        case CursorShape::Crosshair:       return "crosshair";
        case CursorShape::PointingHand:    return "pointer";
        case CursorShape::ResizeLeftRight: return "ew-resize";
        case CursorShape::ResizeUpDown:    return "ns-resize";
        case CursorShape::ResizeAll:       return "move";
        case CursorShape::NotAllowed:      return "not-allowed";
        case CursorShape::Wait:            return "wait";
        case CursorShape::Custom:          return "default";
    }
    return "default";
}

}

class GTKAppWindow : public NativeWindow {
    SharedHandle<GTKItem> rootView = nullptr;
    GtkWindow *window = nullptr;
    GtkWidget *windowRootBox = nullptr;
    GtkWidget *menuWidget = nullptr;
    guint resizeFinishDebounceSource = 0;
    gulong xftDpiHandler_ = 0;
    bool isFullscreen_ = false;
    bool resizable_ = true;
    float minW_ = 0.f, minH_ = 0.f;
    float maxW_ = 0.f, maxH_ = 0.f;
    // dpiScale_ captures the resolution component (Xft.dpi / GDK_DPI_SCALE /
    // GNOME text-scaling-factor) — the part GTK does NOT auto-apply to window
    // geometry. integerScale_ is the GDK device scale that GTK *does* apply
    // internally to surface allocation. The public scaleFactor() returns the
    // combined product so callers see the same logical→physical ratio that
    // Win32 / macOS report. Geometry conversions at the GTK boundary use
    // dpiScale_ alone to avoid double-applying integerScale_.
    float dpiScale_ = 1.f;
    int integerScale_ = 1;
    float currentScale_ = 1.f;
    CursorShape currentCursorShape_ = CursorShape::Arrow;

    struct PrimaryMonitorPlacement {
        int x = 0;
        int y = 0;
        int integerScale = 1;
        bool valid = false;
    };

    static PrimaryMonitorPlacement queryPrimaryMonitor(){
        PrimaryMonitorPlacement out;
        GdkDisplay *display = gdk_display_get_default();
        if(display == nullptr){
            return out;
        }
        GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
        if(monitor == nullptr){
            monitor = gdk_display_get_monitor(display, 0);
        }
        if(monitor == nullptr){
            return out;
        }
        GdkRectangle geom{};
        gdk_monitor_get_geometry(monitor, &geom);
        out.x = geom.x;
        out.y = geom.y;
        gint s = gdk_monitor_get_scale_factor(monitor);
        out.integerScale = (s < 1) ? 1 : (int)s;
        out.valid = true;
        return out;
    }

    static float computeDpiScale(){
        GdkDisplay *display = gdk_display_get_default();
        if(display == nullptr){
            return 1.f;
        }
        GdkScreen *screen = gdk_display_get_default_screen(display);
        if(screen == nullptr){
            return 1.f;
        }
        gdouble dpi = gdk_screen_get_resolution(screen);
        if(dpi <= 0.0 || !std::isfinite(dpi)){
            return 1.f;
        }
        float scale = (float)(dpi / 96.0);
        if(scale < 0.5f){
            scale = 0.5f;
        }
        return scale;
    }

    int computeIntegerScale() const {
        if(window == nullptr){
            return 1;
        }
        gint s = gtk_widget_get_scale_factor(GTK_WIDGET(window));
        return s < 1 ? 1 : (int)s;
    }

    float toGtkLogical(float dip) const {
        return dip * dpiScale_;
    }

    float fromGtkLogical(float logical) const {
        if(dpiScale_ <= 0.f){
            return logical;
        }
        return logical / dpiScale_;
    }

    void recomputeScale(){
        float oldCombined = currentScale_;
        dpiScale_ = computeDpiScale();
        integerScale_ = computeIntegerScale();
        float newCombined = dpiScale_ * (float)integerScale_;
        if(newCombined == oldCombined){
            return;
        }
        currentScale_ = newCombined;
        auto *params = new Native::WindowScaleFactorChangedParams{oldCombined, newCombined, {}};
        emitEvent(NativeEvent::WindowScaleFactorChanged, params);
    }

    void emitEvent(NativeEvent::EventType type,NativeEventParams params){
        if(!hasEventEmitter()){
            if(params != nullptr){
                switch(type){
                    case NativeEvent::WindowWillResize:
                        delete reinterpret_cast<Native::WindowWillResize *>(params);
                        break;
                    case NativeEvent::WindowScaleFactorChanged:
                        delete reinterpret_cast<Native::WindowScaleFactorChangedParams *>(params);
                        break;
                    default:
                        break;
                }
            }
            return;
        }
        eventEmitter()->emit(NativeEventPtr(new NativeEvent(type,params)));
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
        // configure->width/height are in GTK logical pixels (post integer
        // scale). Convert to DIPs so the Composition layer sees the same
        // contract as Win32 / macOS.
        Composition::Rect resizeRect{
            Composition::Point2D{0.f,0.f},
            self->fromGtkLogical(static_cast<float>(configure->width)),
            self->fromGtkLogical(static_cast<float>(configure->height))
        };
        self->rect = sanitizeRect(resizeRect,self->rect);
        if(self->rootView != nullptr){
            self->rootView->resize(self->rect);
        }
        self->emitEvent(NativeEvent::WindowWillResize,new Native::WindowWillResize(self->rect));
        self->queueResizeFinishedEvent();
        return FALSE;
    }

    static void onXftDpiChanged(GObject *gobject,GParamSpec *pspec,gpointer data){
        (void)gobject;
        (void)pspec;
        auto *self = static_cast<GTKAppWindow *>(data);
        self->recomputeScale();
    }

    static gboolean onWindowState(GtkWidget *widget,GdkEvent *event,gpointer data){
        (void)widget;
        auto *self = static_cast<GTKAppWindow *>(data);
        auto *state = reinterpret_cast<GdkEventWindowState *>(event);
        if(state == nullptr){
            return FALSE;
        }
        self->isFullscreen_ = (state->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
        return FALSE;
    }

    static void onScaleFactorChanged(GObject *gobject,GParamSpec *pspec,gpointer data){
        (void)gobject;
        (void)pspec;
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self->window == nullptr){
            return;
        }
        self->recomputeScale();
    }

public:
    GTKAppWindow(Composition::Rect &rectArg,NativeEventEmitter *emitter):
    NativeWindow(sanitizeRect(rectArg,Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f}), emitter){
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
            // Anchor initial placement to the primary monitor so a default
            // rect at {0,0} doesn't land on whichever monitor the WM picks
            // (often the secondary, which on a mixed-DPI setup carries the
            // wrong scale at construction time). Pull the integer scale from
            // that same monitor — gtk_widget_get_scale_factor on an
            // unrealized window returns 1, and we'd otherwise cache a stale
            // value until the first notify::scale-factor fires.
            PrimaryMonitorPlacement primary = queryPrimaryMonitor();
            dpiScale_ = computeDpiScale();
            integerScale_ = primary.valid ? primary.integerScale : computeIntegerScale();
            currentScale_ = dpiScale_ * (float)integerScale_;
            gtk_window_set_default_size(GTK_WINDOW(window),
                (gint)toGtkLogical(this->rect.w),
                (gint)toGtkLogical(this->rect.h));
            gtk_window_move(GTK_WINDOW(window),
                primary.x + (gint)toGtkLogical(this->rect.pos.x),
                primary.y + (gint)toGtkLogical(this->rect.pos.y));
            windowRootBox = gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
            gtk_container_add(GTK_CONTAINER(window),windowRootBox);
            g_signal_connect(window,"delete-event",G_CALLBACK(onDeleteEvent),this);
            g_signal_connect(window,"configure-event",G_CALLBACK(onConfigureEvent),this);
            g_signal_connect(window,"window-state-event",G_CALLBACK(onWindowState),this);
            g_signal_connect(window,"notify::scale-factor",G_CALLBACK(onScaleFactorChanged),this);
            // Catch desktop-DPI changes (Xft.dpi, GNOME text-scaling-factor)
            // at runtime. GtkSettings holds gtk-xft-dpi globally; gdk_screen
            // resolution is derived from it.
            GtkSettings *settings = gtk_settings_get_default();
            if(settings != nullptr){
                xftDpiHandler_ = g_signal_connect(settings,
                    "notify::gtk-xft-dpi",
                    G_CALLBACK(onXftDpiChanged),
                    this);
            }
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

    void minimize() override {
        if(window != nullptr){
            gtk_window_iconify(GTK_WINDOW(window));
        }
    }
    void maximize() override {
        if(window != nullptr){
            gtk_window_maximize(GTK_WINDOW(window));
        }
    }
    void restore() override {
        if(window == nullptr){
            return;
        }
        gtk_window_deiconify(GTK_WINDOW(window));
        gtk_window_unmaximize(GTK_WINDOW(window));
    }
    void toggleFullscreen() override {
        if(window == nullptr){
            return;
        }
        if(isFullscreen_){
            gtk_window_unfullscreen(GTK_WINDOW(window));
        } else {
            gtk_window_fullscreen(GTK_WINDOW(window));
        }
    }
    bool isMinimized() const override {
        if(window == nullptr){
            return false;
        }
        auto *gdk = gtk_widget_get_window(GTK_WIDGET(window));
        if(gdk == nullptr){
            return false;
        }
        return (gdk_window_get_state(gdk) & GDK_WINDOW_STATE_ICONIFIED) != 0;
    }
    bool isMaximized() const override {
        if(window == nullptr){
            return false;
        }
        return gtk_window_is_maximized(GTK_WINDOW(window)) == TRUE;
    }
    bool isFullscreen() const override {
        return isFullscreen_;
    }
    bool isVisible() const override {
        if(window == nullptr){
            return false;
        }
        return gtk_widget_get_visible(GTK_WIDGET(window)) == TRUE;
    }
    Composition::Rect getRect() const override {
        if(window == nullptr){
            return rect;
        }
        gint x = 0, y = 0, w = 0, h = 0;
        gtk_window_get_position(GTK_WINDOW(window), &x, &y);
        gtk_window_get_size(GTK_WINDOW(window), &w, &h);
        // gtk_window_get_size returns GTK logical pixels; convert to DIPs.
        return Composition::Rect{
            Composition::Point2D{fromGtkLogical((float)x),fromGtkLogical((float)y)},
            fromGtkLogical((float)w),
            fromGtkLogical((float)h)
        };
    }
    void setRect(const Composition::Rect & r) override {
        rect = r;
        if(window == nullptr){
            return;
        }
        gtk_window_move(GTK_WINDOW(window),
            (gint)toGtkLogical(r.pos.x),
            (gint)toGtkLogical(r.pos.y));
        gtk_window_resize(GTK_WINDOW(window),
            std::max(1,(gint)toGtkLogical(r.w)),
            std::max(1,(gint)toGtkLogical(r.h)));
    }
    float scaleFactor() const override {
        // Combined scale: dpiScale (Xft.dpi / 96, the part GTK does not
        // auto-apply) × integerScale (gtk_widget_get_scale_factor, the part
        // GTK does auto-apply to surface allocation). Mirrors the
        // logical→physical ratio that Win32 GetDpiForWindow/96 and macOS
        // backingScaleFactor return on their respective platforms.
        return currentScale_;
    }
    void setMinSize(float w, float h) override {
        minW_ = w; minH_ = h;
        if(window == nullptr){
            return;
        }
        GdkGeometry g {};
        g.min_width = (gint)toGtkLogical(w);
        g.min_height = (gint)toGtkLogical(h);
        if(maxW_ > 0.f && maxH_ > 0.f){
            g.max_width = (gint)toGtkLogical(maxW_);
            g.max_height = (gint)toGtkLogical(maxH_);
            gtk_window_set_geometry_hints(GTK_WINDOW(window), nullptr, &g,
                (GdkWindowHints)(GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE));
        } else {
            gtk_window_set_geometry_hints(GTK_WINDOW(window), nullptr, &g, GDK_HINT_MIN_SIZE);
        }
    }
    void setMaxSize(float w, float h) override {
        maxW_ = w; maxH_ = h;
        if(window == nullptr){
            return;
        }
        GdkGeometry g {};
        g.max_width = (gint)toGtkLogical(w);
        g.max_height = (gint)toGtkLogical(h);
        if(minW_ > 0.f && minH_ > 0.f){
            g.min_width = (gint)toGtkLogical(minW_);
            g.min_height = (gint)toGtkLogical(minH_);
            gtk_window_set_geometry_hints(GTK_WINDOW(window), nullptr, &g,
                (GdkWindowHints)(GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE));
        } else {
            gtk_window_set_geometry_hints(GTK_WINDOW(window), nullptr, &g, GDK_HINT_MAX_SIZE);
        }
    }
    void setResizable(bool resizable) override {
        resizable_ = resizable;
        if(window != nullptr){
            gtk_window_set_resizable(GTK_WINDOW(window), resizable ? TRUE : FALSE);
        }
    }
    void orderFront() override {
        if(window != nullptr){
            gtk_window_present(GTK_WINDOW(window));
        }
    }
    void orderBack() override {
        if(window == nullptr){
            return;
        }
        auto *gdk = gtk_widget_get_window(GTK_WIDGET(window));
        if(gdk != nullptr){
            gdk_window_lower(gdk);
        }
    }
    void setOpacity(float alpha) override {
        if(window != nullptr){
            gtk_widget_set_opacity(GTK_WIDGET(window), (gdouble)alpha);
        }
    }
    float getOpacity() const override {
        if(window == nullptr){
            return 1.f;
        }
        return (float)gtk_widget_get_opacity(GTK_WIDGET(window));
    }
    void setCursorShape(CursorShape shape) override {
        currentCursorShape_ = shape;
        if(window == nullptr){
            return;
        }
        auto *gdk = gtk_widget_get_window(GTK_WIDGET(window));
        if(gdk == nullptr){
            return;
        }
        GdkDisplay *display = gdk_display_get_default();
        GdkCursor *cursor = gdk_cursor_new_from_name(display, cursorNameForShape(shape));
        gdk_window_set_cursor(gdk, cursor);
        if(cursor != nullptr){
            g_object_unref(cursor);
        }
    }
    bool isKeyWindow() const override {
        if(window == nullptr){
            return false;
        }
        return gtk_window_is_active(GTK_WINDOW(window)) == TRUE;
    }
    void becomeKeyWindow() override {
        if(window != nullptr){
            gtk_window_present(GTK_WINDOW(window));
        }
    }

    ~GTKAppWindow() {
        if(resizeFinishDebounceSource != 0){
            g_source_remove(resizeFinishDebounceSource);
            resizeFinishDebounceSource = 0;
        }
        if(xftDpiHandler_ != 0){
            GtkSettings *settings = gtk_settings_get_default();
            if(settings != nullptr){
                g_signal_handler_disconnect(settings, xftDpiHandler_);
            }
            xftDpiHandler_ = 0;
        }
        if(window != nullptr){
            g_object_remove_weak_pointer(G_OBJECT(window),reinterpret_cast<gpointer *>(&window));
            gtk_widget_destroy(GTK_WIDGET(window));
            window = nullptr;
        }
        windowRootBox = nullptr;
        menuWidget = nullptr;
        rootView = nullptr;
    }
};

}

namespace OmegaWTK::Native {

NWH make_native_window(Composition::Rect &rect,NativeEventEmitter *emitter){
    return (NWH)new GTK::GTKAppWindow(rect,emitter);
}

}
