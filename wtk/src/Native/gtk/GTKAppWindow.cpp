#include "NativePrivate/gtk/GTKItem.h"
#include "NativePrivate/gtk/X11SurfaceHost.h"
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
#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

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

static ModifierFlags modifierFlagsFromState(guint state){
    ModifierFlags flags;
    flags.shift = (state & GDK_SHIFT_MASK) != 0;
    flags.control = (state & GDK_CONTROL_MASK) != 0;
    flags.alt = (state & GDK_MOD1_MASK) != 0;
#ifdef GDK_META_MASK
    flags.meta = (state & GDK_META_MASK) != 0;
#endif
#ifdef GDK_SUPER_MASK
    flags.meta = flags.meta || ((state & GDK_SUPER_MASK) != 0);
#endif
    flags.capsLock = (state & GDK_LOCK_MASK) != 0;
    return flags;
}

// GDK keyval → OmegaWTK KeyCode. Covers the common subset that the
// macOS Cocoa backend translates (letters, digits, function keys,
// arrows, modifiers). Anything else returns KeyCode::Unknown — the
// `key` field on KeyDownParams still carries the raw Unicode point
// for consumers that need it.
static KeyCode keyCodeFromGdk(guint keyval){
    if(keyval >= GDK_KEY_a && keyval <= GDK_KEY_z){
        return static_cast<KeyCode>(
            static_cast<int>(KeyCode::A) + static_cast<int>(keyval - GDK_KEY_a));
    }
    if(keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z){
        return static_cast<KeyCode>(
            static_cast<int>(KeyCode::A) + static_cast<int>(keyval - GDK_KEY_A));
    }
    if(keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9){
        return static_cast<KeyCode>(
            static_cast<int>(KeyCode::Num0) + static_cast<int>(keyval - GDK_KEY_0));
    }
    if(keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12){
        return static_cast<KeyCode>(
            static_cast<int>(KeyCode::F1) + static_cast<int>(keyval - GDK_KEY_F1));
    }
    if(keyval >= GDK_KEY_F13 && keyval <= GDK_KEY_F15){
        return static_cast<KeyCode>(
            static_cast<int>(KeyCode::F13) + static_cast<int>(keyval - GDK_KEY_F13));
    }
    switch(keyval){
        case GDK_KEY_Escape:      return KeyCode::Escape;
        case GDK_KEY_Tab:
        case GDK_KEY_ISO_Left_Tab: return KeyCode::Tab;
        case GDK_KEY_Caps_Lock:   return KeyCode::CapsLock;
        case GDK_KEY_space:       return KeyCode::Space;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:    return KeyCode::Enter;
        case GDK_KEY_BackSpace:   return KeyCode::Backspace;
        case GDK_KEY_Delete:
        case GDK_KEY_KP_Delete:   return KeyCode::Delete;
        case GDK_KEY_Shift_L:     return KeyCode::LeftShift;
        case GDK_KEY_Shift_R:     return KeyCode::RightShift;
        case GDK_KEY_Control_L:   return KeyCode::LeftControl;
        case GDK_KEY_Control_R:   return KeyCode::RightControl;
        case GDK_KEY_Alt_L:       return KeyCode::LeftAlt;
        case GDK_KEY_Alt_R:       return KeyCode::RightAlt;
        case GDK_KEY_Super_L:
        case GDK_KEY_Meta_L:      return KeyCode::LeftMeta;
        case GDK_KEY_Super_R:
        case GDK_KEY_Meta_R:      return KeyCode::RightMeta;
        case GDK_KEY_Up:          return KeyCode::ArrowUp;
        case GDK_KEY_Down:        return KeyCode::ArrowDown;
        case GDK_KEY_Left:        return KeyCode::ArrowLeft;
        case GDK_KEY_Right:       return KeyCode::ArrowRight;
        case GDK_KEY_Home:        return KeyCode::Home;
        case GDK_KEY_End:         return KeyCode::End;
        case GDK_KEY_Page_Up:     return KeyCode::PageUp;
        case GDK_KEY_Page_Down:   return KeyCode::PageDown;
        default:                  return KeyCode::Unknown;
    }
}

}

class GTKAppWindow : public NativeWindow {
    SharedHandle<GTKItem> rootView = nullptr;
    GtkWindow *window = nullptr;
    GtkWidget *windowRootBox = nullptr;
    GtkWidget *menuWidget = nullptr;
    gulong menuSizeAllocateHandler_ = 0;
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
    // Top inset (in DIPs) consumed by the menu bar. Exposed via
    // gtk_menu_bar_inset_from_native so the hover dispatcher can
    // subtract it before walking the virtual view tree.
    float menuBarInset_ = 0.f;

    // §2.13 root-surface ownership: WTK owns the toplevel X11 Window
    // directly. The host is constructed unconditionally on X11 builds
    // (display in hand from the GdkDisplay) and notified of the
    // toplevel XID once the GtkWindow realizes. On Wayland builds the
    // host is null — the X11-only commitment in §2.13 keeps the
    // Wayland code path on the legacy plumbing.
    std::unique_ptr<X11SurfaceHost> surfaceHost_;

    // NativeWindow-Ready-Signal-Plan step 2: GTK realize-gate state.
    // nativeReady_ is the atomic boolean isNativeReady() returns; read on
    // the CompositorFrameWorker thread, written on the GTK UI thread.
    // The two subscriber vectors back onFirstRealize / onRealize and are
    // guarded by realizeCallbacksMutex_. firstRealizeFired_ tracks the
    // singleton transition that distinguishes "initial realize" from
    // "subsequent re-realize" — see the plan §3.1 / §3.4.
    std::atomic<bool> nativeReady_ {false};
    mutable std::mutex realizeCallbacksMutex_;
    std::vector<std::function<void()>> firstRealizeSubscribers_;
    std::vector<std::function<void()>> realizeSubscribers_;
    bool firstRealizeFired_ = false;

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
        // Push the new bounds into the root GTKItem so the compositor
        // and the per-window WidgetTreeHost see the resize via the
        // existing handleAllocation path. The root GTKItem borrows the
        // toplevel widget — its own widget allocation is unconditionally
        // identical to the toplevel's, so a synthetic GtkAllocation
        // from the configure event is the source of truth.
        if(self->rootView != nullptr){
            GtkAllocation alloc{};
            alloc.x = 0;
            alloc.y = 0;
            alloc.width = configure->width;
            alloc.height = configure->height;
            self->rootView->handleAllocation(alloc);
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

    // GTK "realize" signal handler — invoked when the underlying
    // GdkWindow / X11 Window becomes available. Drives the
    // NativeWindow-Ready-Signal-Plan gate AND hands the toplevel XID
    // to the X11SurfaceHost so any work queued via runOnRealize
    // (the Vulkan native-target deferred resolve in §2.14 is a typical
    // consumer) fires now.
    static void onRealizeSignal(GtkWidget *widget,gpointer data){
        (void)widget;
        auto *self = static_cast<GTKAppWindow *>(data);
        self->handleRealize();
    }

    void handleRealize(){
        // First: feed the toplevel XID to the X11SurfaceHost. Must
        // happen BEFORE we drain the realize subscribers, because some
        // of those subscribers (notably the Vulkan present-target
        // resolve) call back through the X11SurfaceHost.
#if WTK_NATIVE_X11
        if(surfaceHost_ != nullptr && window != nullptr){
            GdkWindow *gdk = gtk_widget_get_window(GTK_WIDGET(window));
            if(gdk != nullptr && GDK_IS_X11_WINDOW(gdk)){
                gdk_window_ensure_native(gdk);
                ::Window xid = gdk_x11_window_get_xid(gdk);
                if(xid != 0){
                    surfaceHost_->onToplevelRealized(xid);
                }
            }
        }
#endif

        std::vector<std::function<void()>> firstCallbacks;
        std::vector<std::function<void()>> reCallbacks;
        {
            std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
            nativeReady_.store(true, std::memory_order_release);
            if(!firstRealizeFired_){
                firstRealizeFired_ = true;
                // Drain + free storage: the firstRealize subscribers fire
                // exactly once per NativeWindow lifetime, never replayed
                // on a re-realize cycle.
                firstCallbacks.swap(firstRealizeSubscribers_);
            }
            else {
                // Sticky semantics: copy the subscriber list so it
                // remains intact for subsequent re-realize fires.
                reCallbacks = realizeSubscribers_;
            }
        }
        for(auto & cb : firstCallbacks){
            if(cb) cb();
        }
        for(auto & cb : reCallbacks){
            if(cb) cb();
        }
    }

    // -- Input event handlers (§2.13: installed directly on the GtkWindow,
    //    not on a child drawing area). All emit through the AppWindow's
    //    own eventEmitter so the input stream lands in the same place
    //    Win32 / macOS deliver to. --

    static gboolean onButtonPressEvent(GtkWidget *,GdkEventButton *event,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || event == nullptr){
            return FALSE;
        }
        NativeEvent::EventType type = NativeEvent::Unknown;
        if(event->button == 1u){
            type = NativeEvent::LMouseDown;
        }
        else if(event->button == 3u){
            type = NativeEvent::RMouseDown;
        }
        else {
            return FALSE;
        }
        Composition::Point2D clientPos {
            static_cast<float>(event->x),
            static_cast<float>(event->y)
        };
        Composition::Point2D screenPos {
            static_cast<float>(event->x_root),
            static_cast<float>(event->y_root)
        };
        NativeEventParams params = nullptr;
        if(type == NativeEvent::LMouseDown){
            auto *mp = new LMouseDownParams();
            mp->position = clientPos;
            mp->screenPosition = screenPos;
            mp->modifiers = modifierFlagsFromState(event->state);
            mp->clickCount = 1u;
            params = mp;
        }
        else {
            auto *mp = new RMouseDownParams();
            mp->position = clientPos;
            mp->screenPosition = screenPos;
            mp->modifiers = modifierFlagsFromState(event->state);
            mp->clickCount = 1u;
            params = mp;
        }
        self->emitEvent(type,params);
        return FALSE;
    }

    static gboolean onButtonReleaseEvent(GtkWidget *,GdkEventButton *event,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || event == nullptr){
            return FALSE;
        }
        NativeEvent::EventType type = NativeEvent::Unknown;
        if(event->button == 1u){
            type = NativeEvent::LMouseUp;
        }
        else if(event->button == 3u){
            type = NativeEvent::RMouseUp;
        }
        else {
            return FALSE;
        }
        Composition::Point2D clientPos {
            static_cast<float>(event->x),
            static_cast<float>(event->y)
        };
        Composition::Point2D screenPos {
            static_cast<float>(event->x_root),
            static_cast<float>(event->y_root)
        };
        NativeEventParams params = nullptr;
        if(type == NativeEvent::LMouseUp){
            auto *mp = new LMouseUpParams();
            mp->position = clientPos;
            mp->screenPosition = screenPos;
            mp->modifiers = modifierFlagsFromState(event->state);
            mp->clickCount = 1u;
            params = mp;
        }
        else {
            auto *mp = new RMouseUpParams();
            mp->position = clientPos;
            mp->screenPosition = screenPos;
            mp->modifiers = modifierFlagsFromState(event->state);
            mp->clickCount = 1u;
            params = mp;
        }
        self->emitEvent(type,params);
        return FALSE;
    }

    static gboolean onMotionNotifyEvent(GtkWidget *,GdkEventMotion *event,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || event == nullptr){
            return FALSE;
        }
        auto *params = new CursorMoveParams();
        params->position = Composition::Point2D {
            static_cast<float>(event->x),
            static_cast<float>(event->y)
        };
        params->screenPosition = Composition::Point2D {
            static_cast<float>(event->x_root),
            static_cast<float>(event->y_root)
        };
        params->modifiers = modifierFlagsFromState(event->state);
        self->emitEvent(NativeEvent::CursorMove,params);
        return FALSE;
    }

    static gboolean onEnterNotifyEvent(GtkWidget *,GdkEventCrossing *event,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || event == nullptr){
            return FALSE;
        }
        auto *params = new CursorEnterParams();
        params->position = Composition::Point2D {
            static_cast<float>(event->x),
            static_cast<float>(event->y)
        };
        self->emitEvent(NativeEvent::CursorEnter,params);
        return FALSE;
    }

    static gboolean onLeaveNotifyEvent(GtkWidget *,GdkEventCrossing *event,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || event == nullptr){
            return FALSE;
        }
        auto *params = new CursorExitParams();
        params->position = Composition::Point2D {
            static_cast<float>(event->x),
            static_cast<float>(event->y)
        };
        self->emitEvent(NativeEvent::CursorExit,params);
        return FALSE;
    }

    static gboolean onScrollEvent(GtkWidget *,GdkEventScroll *event,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || event == nullptr){
            return FALSE;
        }
        Composition::Point2D scrollPos {
            static_cast<float>(event->x),
            static_cast<float>(event->y)
        };
        constexpr double epsilon = 0.0001;
        if(event->direction == GDK_SCROLL_SMOOTH){
            double deltaX = 0.0;
            double deltaY = 0.0;
            if(gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent *>(event),&deltaX,&deltaY)){
                if(std::fabs(deltaX) > epsilon || std::fabs(deltaY) > epsilon){
                    self->emitEvent(NativeEvent::ScrollWheel,
                        new ScrollParams {static_cast<float>(deltaX),
                                          static_cast<float>(deltaY),
                                          scrollPos});
                }
                if(std::fabs(deltaX) > epsilon){
                    auto type = deltaX > 0.0 ? NativeEvent::ScrollRight : NativeEvent::ScrollLeft;
                    self->emitEvent(type,new ScrollParams {
                        static_cast<float>(deltaX), 0.f, scrollPos});
                }
                if(std::fabs(deltaY) > epsilon){
                    auto type = deltaY > 0.0 ? NativeEvent::ScrollDown : NativeEvent::ScrollUp;
                    self->emitEvent(type,new ScrollParams {
                        0.f, static_cast<float>(deltaY), scrollPos});
                }
            }
            return FALSE;
        }
        float dirDx = 0.f, dirDy = 0.f;
        switch(event->direction){
            case GDK_SCROLL_UP:
                dirDy = -1.f;
                self->emitEvent(NativeEvent::ScrollUp,
                    new ScrollParams {0.f,-1.f,scrollPos});
                break;
            case GDK_SCROLL_DOWN:
                dirDy = 1.f;
                self->emitEvent(NativeEvent::ScrollDown,
                    new ScrollParams {0.f,1.f,scrollPos});
                break;
            case GDK_SCROLL_LEFT:
                dirDx = -1.f;
                self->emitEvent(NativeEvent::ScrollLeft,
                    new ScrollParams {-1.f,0.f,scrollPos});
                break;
            case GDK_SCROLL_RIGHT:
                dirDx = 1.f;
                self->emitEvent(NativeEvent::ScrollRight,
                    new ScrollParams {1.f,0.f,scrollPos});
                break;
            default:
                break;
        }
        if(std::fabs(dirDx) > epsilon || std::fabs(dirDy) > epsilon){
            self->emitEvent(NativeEvent::ScrollWheel,
                new ScrollParams {dirDx * 10.f, dirDy * 10.f, scrollPos});
        }
        return FALSE;
    }

    static gboolean onKeyPressEvent(GtkWidget *,GdkEventKey *event,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || event == nullptr){
            return FALSE;
        }
        auto *params = new KeyDownParams();
        params->code = keyCodeFromGdk(event->keyval);
        params->key = static_cast<OmegaCommon::Unicode32Char>(
            gdk_keyval_to_unicode(event->keyval));
        params->modifiers = modifierFlagsFromState(event->state);
        // GdkEventKey carries no explicit isRepeat in GTK3 (auto-repeat is
        // delivered as a stream of press events); leave isRepeat at its
        // default false and let consumers track repeats themselves.
        self->emitEvent(NativeEvent::KeyDown,params);
        return FALSE;
    }

    static gboolean onKeyReleaseEvent(GtkWidget *,GdkEventKey *event,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || event == nullptr){
            return FALSE;
        }
        auto *params = new KeyUpParams();
        params->code = keyCodeFromGdk(event->keyval);
        params->key = static_cast<OmegaCommon::Unicode32Char>(
            gdk_keyval_to_unicode(event->keyval));
        params->modifiers = modifierFlagsFromState(event->state);
        self->emitEvent(NativeEvent::KeyUp,params);
        return FALSE;
    }

    // Menu bar `size-allocate` → cache the inset so the hover dispatcher
    // can subtract it from incoming event Ys. The menu lives inside
    // windowRootBox; its allocated height is the inset in GTK logical
    // pixels — divide by dpiScale_ to get DIPs (matches the unit our
    // event coordinates are reported in).
    static void onMenuSizeAllocate(GtkWidget *,GtkAllocation *alloc,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || alloc == nullptr){
            return;
        }
        self->menuBarInset_ = self->fromGtkLogical(static_cast<float>(alloc->height));
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

#if WTK_NATIVE_X11
            // §2.13 X11-only: assert the display is X11 and instantiate
            // X11SurfaceHost ahead of realize so deferred child-surface
            // requests have somewhere to land.
            GdkDisplay *gdkDisplay = gtk_widget_get_display(GTK_WIDGET(window));
            if(gdkDisplay != nullptr && GDK_IS_X11_DISPLAY(gdkDisplay)){
                ::Display *xdpy = gdk_x11_display_get_xdisplay(gdkDisplay);
                if(xdpy != nullptr){
                    surfaceHost_ = std::make_unique<X11SurfaceHost>(xdpy);
                }
            }
            else if(gdkDisplay != nullptr){
                std::cerr << "[OmegaWTK][GTK] §2.13 requires GDK_BACKEND=x11; "
                             "non-X11 display detected — X11SurfaceHost disabled."
                          << std::endl;
            }
#endif

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

            // §2.13: turn off GTK painting on the toplevel so the
            // engine can render directly into its X11 Window without
            // the theme background flashing first.
            gtk_widget_set_app_paintable(GTK_WIDGET(window),TRUE);
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            gtk_widget_set_double_buffered(GTK_WIDGET(window),FALSE);
G_GNUC_END_IGNORE_DEPRECATIONS

            // §2.13: install the event mask on the toplevel BEFORE
            // show*, so GDK forwards button/motion/key/scroll/enter/
            // leave/configure events to our handlers.
            gtk_widget_add_events(GTK_WIDGET(window),
                GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                GDK_POINTER_MOTION_MASK |
                GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
                GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
                GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK |
                GDK_STRUCTURE_MASK);

            // The window-root box hosts the menu bar (when one is set)
            // as the only child. It does not contain the rendering
            // surface — that is the GtkWindow's own GdkWindow / X11
            // Window. We keep the box unconditionally so setMenu /
            // setMenu(null) don't need to reparent the toplevel's
            // single child mid-lifetime.
            windowRootBox = gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
            gtk_container_add(GTK_CONTAINER(window),windowRootBox);

            g_signal_connect(window,"delete-event",G_CALLBACK(onDeleteEvent),this);
            g_signal_connect(window,"configure-event",G_CALLBACK(onConfigureEvent),this);
            g_signal_connect(window,"window-state-event",G_CALLBACK(onWindowState),this);
            g_signal_connect(window,"notify::scale-factor",G_CALLBACK(onScaleFactorChanged),this);

            // §2.13: input event handlers go on the GtkWindow itself —
            // the drawing-area child is gone. Hit-testing is virtual
            // (View::containsPoint via the WidgetTreeHost dispatcher),
            // so the same handlers that used to run on the drawing area
            // run on the toplevel here with no functional change.
            g_signal_connect(window,"button-press-event",G_CALLBACK(onButtonPressEvent),this);
            g_signal_connect(window,"button-release-event",G_CALLBACK(onButtonReleaseEvent),this);
            g_signal_connect(window,"motion-notify-event",G_CALLBACK(onMotionNotifyEvent),this);
            g_signal_connect(window,"enter-notify-event",G_CALLBACK(onEnterNotifyEvent),this);
            g_signal_connect(window,"leave-notify-event",G_CALLBACK(onLeaveNotifyEvent),this);
            g_signal_connect(window,"scroll-event",G_CALLBACK(onScrollEvent),this);
            g_signal_connect(window,"key-press-event",G_CALLBACK(onKeyPressEvent),this);
            g_signal_connect(window,"key-release-event",G_CALLBACK(onKeyReleaseEvent),this);

            // NativeWindow-Ready-Signal-Plan §3.2 GTK trigger: the
            // "realize" signal fires when the underlying X11 Window
            // becomes available — exactly when the engine may begin
            // rendering. Connect BEFORE any gtk_widget_show*() so the
            // realize cascade during initial display lands on this
            // handler.
            g_signal_connect(window,"realize",G_CALLBACK(onRealizeSignal),this);
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

        // §2.13: the root NativeItem is a thin handle on the toplevel
        // GtkWindow — no drawing area, no extra widget. The
        // toplevel-binding ctor stores the widget by reference; the
        // GTKItem dtor does NOT destroy it (GTKAppWindow owns
        // lifetime).
        if(window != nullptr){
            rootView = std::make_shared<GTKItem>(this->rect, GTK_WIDGET(window));
        }
    }

    NativeItemPtr getRootView() override {
        return std::static_pointer_cast<NativeItem>(rootView);
    }

    /// Exposes the underlying GtkWindow so sibling backends (dialogs) can
    /// parent transient windows to it. getRootView() is private to AppWindow.
    GtkWindow *getGTKWindow() {
        return window;
    }

    /// §2.13 accessor: WTK's per-window X11 surface manager. Null on
    /// Wayland or before construction has populated it.
    X11SurfaceHost *getSurfaceHost() {
        return surfaceHost_.get();
    }

    /// §2.13 accessor: latest menu-bar height in DIPs (0 if no menu).
    float getMenuBarInset() const {
        return menuBarInset_;
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

    // ---- NativeWindow-Ready-Signal-Plan overrides (step 2) ----
    //
    // The compositor's drainWindowSurfaces() will gate consume() on
    // isNativeReady() once step 3 lands; until then these are exercised
    // only by the AppWindow ctor's onFirstRealize registration.

    bool isNativeReady() const override {
        return nativeReady_.load(std::memory_order_acquire);
    }

    void onFirstRealize(std::function<void()> cb) override {
        if(!cb){
            return;
        }
        {
            std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
            if(!firstRealizeFired_){
                // Pre-realize: queue. The realize handler will drain
                // this list when the GTK realize signal fires.
                firstRealizeSubscribers_.push_back(std::move(cb));
                return;
            }
        }
        // Post-realize fast path: fire synchronously on the calling
        // thread. cb was not moved on this path because the if-branch
        // returned early.
        cb();
    }

    void onRealize(std::function<void()> cb) override {
        if(!cb){
            return;
        }
        // Sticky semantics: no synchronous-replay path. The callback
        // fires only on future re-realize transitions (whose triggers
        // — DPI / display reconfigure / forced surface recreate — are
        // not yet wired on this backend; they will route through
        // handleRealize() when they are).
        std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
        realizeSubscribers_.push_back(std::move(cb));
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
        // Tear down the previously installed menu, if any: detach our
        // size-allocate listener so we stop tracking its inset, then
        // remove from the container.
        if(menuWidget != nullptr){
            if(menuSizeAllocateHandler_ != 0){
                g_signal_handler_disconnect(menuWidget,menuSizeAllocateHandler_);
                menuSizeAllocateHandler_ = 0;
            }
            auto *parent = gtk_widget_get_parent(menuWidget);
            if(parent != nullptr && GTK_IS_CONTAINER(parent)){
                gtk_container_remove(GTK_CONTAINER(parent),menuWidget);
            }
            menuWidget = nullptr;
            menuBarInset_ = 0.f;
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
        // Track the menu's allocated height so getMenuBarInset() stays
        // current as the user resizes / themes change height.
        menuSizeAllocateHandler_ = g_signal_connect(menuWidget,
            "size-allocate", G_CALLBACK(onMenuSizeAllocate), this);
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
        if(menuWidget != nullptr && menuSizeAllocateHandler_ != 0){
            g_signal_handler_disconnect(menuWidget,menuSizeAllocateHandler_);
            menuSizeAllocateHandler_ = 0;
        }
        // §2.13: tear down the rootView (which holds a weak ref to the
        // toplevel widget) BEFORE destroying the GtkWindow. The
        // X11SurfaceHost is destroyed AFTER the GtkWindow so any owned
        // child X11 Windows are released against a still-alive display.
        rootView = nullptr;
        if(window != nullptr){
            g_object_remove_weak_pointer(G_OBJECT(window),reinterpret_cast<gpointer *>(&window));
            gtk_widget_destroy(GTK_WIDGET(window));
            window = nullptr;
        }
        windowRootBox = nullptr;
        menuWidget = nullptr;
        surfaceHost_.reset();
    }
};

GtkWindow *gtk_window_from_native(const NWH & window){
    auto appWindow = std::dynamic_pointer_cast<GTKAppWindow>(window);
    return appWindow ? appWindow->getGTKWindow() : nullptr;
}

X11SurfaceHost *gtk_x11_surface_host_from_native(const NWH & window){
    auto appWindow = std::dynamic_pointer_cast<GTKAppWindow>(window);
    return appWindow ? appWindow->getSurfaceHost() : nullptr;
}

float gtk_menu_bar_inset_from_native(const NWH & window){
    auto appWindow = std::dynamic_pointer_cast<GTKAppWindow>(window);
    return appWindow ? appWindow->getMenuBarInset() : 0.f;
}

}

namespace OmegaWTK::Native {

NWH make_native_window(Composition::Rect &rect,NativeEventEmitter *emitter){
    return (NWH)new GTK::GTKAppWindow(rect,emitter);
}

}
