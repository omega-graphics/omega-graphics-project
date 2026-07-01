// §2.15 GTK4 Migration — Option B′ window backend.
//
// GTKAppWindow falls through to a *bare* toplevel GdkSurface
// (gdk_surface_new_toplevel): no GtkWindow, no GtkWidget, hence no GskRenderer
// bound to the surface, so WTK's own Vulkan swap chain owns it outright (a
// GtkWindow's GSK renderer otherwise steals the surface — vkCreateSwapchainKHR
// fails with VK_ERROR_SURFACE_LOST_KHR; see the probe finding in
// wtk/.plans/Native-API-Completion-Proposal.md §2.15). GTK/GDK is used only for
// windowing lifecycle (present/close/state) and input (the GdkSurface "event"
// signal). The root NativeItem (GTKItem) wraps this surface; child NativeItems
// are subsurfaces owned by the VisualTree via the §2.13a SurfaceHost registry.
//
// G4-7: GTK 3 retired — this is the sole Linux window backend.

#include "NativePrivate/gtk/GTKItem.h"
#include "NativePrivate/gtk/SurfaceHost.h"
#include "NativePrivate/gtk/X11SurfaceHost.h"
#include "NativePrivate/gtk/WaylandSurfaceHost.h"
#include "NativePrivate/gtk/GTKWindowing.h"
#include "GTKApp.h"

// X11's <X.h> (pulled in transitively) defines a `CursorShape` macro that
// collides with OmegaWTK::Native::CursorShape. Undef before our headers.
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
#include <vector>

namespace OmegaWTK::Native::GTK {

namespace {

static Composition::Rect sanitizeRect(const Composition::Rect &candidate,const Composition::Rect &fallback){
    Composition::Rect sane = candidate;
    if(!std::isfinite(sane.pos.x)) sane.pos.x = fallback.pos.x;
    if(!std::isfinite(sane.pos.y)) sane.pos.y = fallback.pos.y;
    if(!std::isfinite(sane.w) || sane.w <= 0.f) sane.w = fallback.w;
    if(!std::isfinite(sane.h) || sane.h <= 0.f) sane.h = fallback.h;
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

// Thickness (logical px) of the edge/corner band that starts a user resize on
// the decorationless bare surface.
static const double kResizeBorder = 8.0;

static bool edgeForPoint(double x,double y,int w,int h,GdkSurfaceEdge *outEdge){
    const bool left   = x <= kResizeBorder;
    const bool right  = x >= w - kResizeBorder;
    const bool top    = y <= kResizeBorder;
    const bool bottom = y >= h - kResizeBorder;
    if(top && left)          *outEdge = GDK_SURFACE_EDGE_NORTH_WEST;
    else if(top && right)    *outEdge = GDK_SURFACE_EDGE_NORTH_EAST;
    else if(bottom && left)  *outEdge = GDK_SURFACE_EDGE_SOUTH_WEST;
    else if(bottom && right) *outEdge = GDK_SURFACE_EDGE_SOUTH_EAST;
    else if(left)   *outEdge = GDK_SURFACE_EDGE_WEST;
    else if(right)  *outEdge = GDK_SURFACE_EDGE_EAST;
    else if(top)    *outEdge = GDK_SURFACE_EDGE_NORTH;
    else if(bottom) *outEdge = GDK_SURFACE_EDGE_SOUTH;
    else return false;
    return true;
}

static const char *cursorNameForEdge(GdkSurfaceEdge edge){
    switch(edge){
        case GDK_SURFACE_EDGE_NORTH:
        case GDK_SURFACE_EDGE_SOUTH:      return "ns-resize";
        case GDK_SURFACE_EDGE_WEST:
        case GDK_SURFACE_EDGE_EAST:       return "ew-resize";
        case GDK_SURFACE_EDGE_NORTH_WEST:
        case GDK_SURFACE_EDGE_SOUTH_EAST: return "nwse-resize";
        case GDK_SURFACE_EDGE_NORTH_EAST:
        case GDK_SURFACE_EDGE_SOUTH_WEST: return "nesw-resize";
        default:                          return "default";
    }
}

// ---- G4-4 input translation helpers ----
//
// GdkModifierType → OmegaWTK ModifierFlags. GTK 4 reads the mask off any event
// with gdk_event_get_modifier_state; GDK_MOD1_MASK was renamed GDK_ALT_MASK in
// GTK 4 (the GTK-3 backend in GTKAppWindow.cpp still uses the old name).
static ModifierFlags modifierFlagsFromState(GdkModifierType state){
    ModifierFlags flags;
    flags.shift    = (state & GDK_SHIFT_MASK)   != 0;
    flags.control  = (state & GDK_CONTROL_MASK) != 0;
    flags.alt      = (state & GDK_ALT_MASK)     != 0;
    flags.meta     = ((state & GDK_META_MASK)   != 0) || ((state & GDK_SUPER_MASK) != 0);
    flags.capsLock = (state & GDK_LOCK_MASK)    != 0;
    return flags;
}

// GDK keyval → OmegaWTK KeyCode. Covers the common subset (letters, digits,
// function keys, navigation, modifiers); anything else returns KeyCode::Unknown
// while the raw Unicode point still rides on KeyDownParams::key. The GDK_KEY_*
// symbols are identical across GTK 3/4, so this mirrors the GTK-3 backend.
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
        case GDK_KEY_Escape:       return KeyCode::Escape;
        case GDK_KEY_Tab:
        case GDK_KEY_ISO_Left_Tab: return KeyCode::Tab;
        case GDK_KEY_Caps_Lock:    return KeyCode::CapsLock;
        case GDK_KEY_space:        return KeyCode::Space;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:     return KeyCode::Enter;
        case GDK_KEY_BackSpace:    return KeyCode::Backspace;
        case GDK_KEY_Delete:
        case GDK_KEY_KP_Delete:    return KeyCode::Delete;
        case GDK_KEY_Shift_L:      return KeyCode::LeftShift;
        case GDK_KEY_Shift_R:      return KeyCode::RightShift;
        case GDK_KEY_Control_L:    return KeyCode::LeftControl;
        case GDK_KEY_Control_R:    return KeyCode::RightControl;
        case GDK_KEY_Alt_L:        return KeyCode::LeftAlt;
        case GDK_KEY_Alt_R:        return KeyCode::RightAlt;
        case GDK_KEY_Super_L:
        case GDK_KEY_Meta_L:       return KeyCode::LeftMeta;
        case GDK_KEY_Super_R:
        case GDK_KEY_Meta_R:       return KeyCode::RightMeta;
        case GDK_KEY_Up:           return KeyCode::ArrowUp;
        case GDK_KEY_Down:         return KeyCode::ArrowDown;
        case GDK_KEY_Left:         return KeyCode::ArrowLeft;
        case GDK_KEY_Right:        return KeyCode::ArrowRight;
        case GDK_KEY_Home:         return KeyCode::Home;
        case GDK_KEY_End:          return KeyCode::End;
        case GDK_KEY_Page_Up:      return KeyCode::PageUp;
        case GDK_KEY_Page_Down:    return KeyCode::PageDown;
        default:                   return KeyCode::Unknown;
    }
}

}

class GTKAppWindow : public NativeWindow {
    SharedHandle<GTKItem> rootView = nullptr;
    GdkSurface *surface = nullptr;
    GdkToplevelLayout *toplevelLayout = nullptr;

    // Latest negotiated logical size (from the "layout" signal) and the size we
    // ask the compositor for in "compute-size". rect (base member) mirrors the
    // logical extent; pixel sizing multiplies by the integer surface scale.
    int logicalW_ = 1, logicalH_ = 1;
    int requestedW_ = 1, requestedH_ = 1;
    int minW_ = 0, minH_ = 0;
    // Integer buffer scale we render + commit at. On a fractional display GDK /
    // the compositor own the fractional part: we commit an integer-scaled buffer
    // (via wl_surface_set_buffer_scale) and the compositor downscales it to the
    // fractional output scale itself. A bare GdkSurface gets no wp_viewport from
    // GDK, so rendering at true fractional pixels (dropping buffer_scale) sizes
    // the window wrong — verified: the window came up oversized. We therefore
    // track and render at the integer scale, and only react when *it* changes.
    int integerScale_ = 1;
    float opacity_ = 1.f;
    bool resizable_ = true;
    CursorShape currentCursorShape_ = CursorShape::Arrow;

    // Last known pointer position (surface-local), tracked from motion / button
    // / crossing events. GTK 4 scroll (axis) events carry no position —
    // gdk_event_get_position returns FALSE and leaves NaN — so handleScroll
    // falls back to this for hit-testing the wheel target.
    double lastPointerX_ = 0.0, lastPointerY_ = 0.0;

    // §2.13a root-surface ownership: chosen from the runtime-detected backend.
    std::unique_ptr<SurfaceHost> surfaceHost_;

    // NativeWindow-Ready-Signal-Plan realize gate. nativeReady_ flips true on
    // the first "layout" (surface mapped + protocol handle valid).
    std::atomic<bool> nativeReady_ {false};
    mutable std::mutex realizeCallbacksMutex_;
    std::vector<std::function<void()>> firstRealizeSubscribers_;
    std::vector<std::function<void()>> realizeSubscribers_;
    bool firstRealizeFired_ = false;

    // requestFrameFlush coalescing (mirror the GTK-3 backend: defer to a
    // g_idle source so a re-entrant requestFrame during flush cannot tower the
    // stack).
    std::atomic<bool> frameFlushQueued_ {false};
    guint frameFlushIdleSource_ = 0;

    // Debounce source for the WindowHasFinishedResize event. A compositor
    // resize fires a stream of "layout" signals; the finish event lands once
    // the stream goes quiet (mirrors the GTK-3 backend's resize-finish
    // debounce).
    guint resizeFinishDebounceSource_ = 0;

    GdkToplevel *toplevel() const {
        return surface != nullptr ? GDK_TOPLEVEL(surface) : nullptr;
    }

    void presentToplevel(){
        if(surface != nullptr && toplevelLayout != nullptr){
            gdk_toplevel_present(GDK_TOPLEVEL(surface),toplevelLayout);
        }
    }

    // HiDPI buffer sizing. The swap chain is sized in *physical* pixels
    // (lround(logical × integerScale_), via VKVisualBinder), so the wl_surface
    // must advertise the matching integer buffer scale — otherwise the
    // compositor treats the physical-sized buffer as logical and the window
    // comes up `scale`× too large. GDK sets this for surfaces *it* paints, but
    // never for a bare GdkSurface we render into ourselves, so we set it. On a
    // fractional display the compositor downscales this integer-scaled buffer to
    // the fractional output scale itself — GDK does not hand a bare surface a
    // wp_viewport, so committing true fractional pixels without a buffer scale
    // sizes the window wrong. Pending state applies on the next WSI commit
    // (vkQueuePresentKHR); no explicit commit needed here.
    void applyWaylandBufferScale(){
#if WTK_NATIVE_WAYLAND
        if(surface != nullptr && GDK_IS_WAYLAND_SURFACE(surface)){
            wl_surface *wls = gdk_wayland_surface_get_wl_surface(GDK_WAYLAND_SURFACE(surface));
            if(wls != nullptr){
                wl_surface_set_buffer_scale(wls, integerScale_ > 0 ? integerScale_ : 1);
            }
        }
#endif
    }

    // §2.2 DPI scale-change emit. GdkSurface's "scale-factor" property changes
    // when the integer buffer scale changes — the window is dragged to a
    // different-integer-DPI monitor, or the display scale crosses an integer
    // boundary. That is the only change that affects what we render (the
    // fractional part within one integer step is the compositor's job — see
    // applyWaylandBufferScale), so we gate on the integer value and re-apply the
    // buffer scale + emit WindowScaleFactorChanged, driving the AppWindow §2.2
    // consumer to re-rasterize the tree at the new density.
    static void onNotifyScaleFactor(GObject *,GParamSpec *,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || self->surface == nullptr) return;
        int oldScale = self->integerScale_;
        self->integerScale_ = std::max(1,gdk_surface_get_scale_factor(self->surface));
        if(self->integerScale_ == oldScale) return;
        self->applyWaylandBufferScale();
        auto *params = new WindowScaleFactorChangedParams();
        params->oldScale = (float)oldScale;
        params->newScale = (float)self->integerScale_;
        // Linux leaves suggestedRect empty (the window does not resize on a
        // backing-scale change — the compositor keeps the logical size).
        self->emitEvent(NativeEvent::WindowScaleFactorChanged,params);
    }

    void feedSurfaceHost(){
        if(surfaceHost_ == nullptr || surface == nullptr){
            return;
        }
        switch(surfaceHost_->backend()){
#if WTK_NATIVE_X11
        case WindowingBackend::X11:
            if(GDK_IS_X11_SURFACE(surface)){
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                ::Window xid = gdk_x11_surface_get_xid(surface);
G_GNUC_END_IGNORE_DEPRECATIONS
                if(xid != 0){
                    static_cast<X11SurfaceHost *>(surfaceHost_.get())->onToplevelRealized(xid);
                }
            }
            break;
#endif
#if WTK_NATIVE_WAYLAND
        case WindowingBackend::Wayland:
            if(GDK_IS_WAYLAND_SURFACE(surface)){
                wl_surface *surf = gdk_wayland_surface_get_wl_surface(GDK_WAYLAND_SURFACE(surface));
                if(surf != nullptr){
                    static_cast<WaylandSurfaceHost *>(surfaceHost_.get())->onToplevelRealized(surf);
                }
            }
            break;
#endif
        default:
            break;
        }
    }

    // Fires once the surface is mapped + configured: feed the SurfaceHost and
    // drain the realize-gate subscribers. Idempotent — guarded by
    // firstRealizeFired_.
    void handleFirstRealize(){
        // Feed the host BEFORE draining subscribers: some (the §2.14 Vulkan
        // present-target resolve) call back through the host.
        feedSurfaceHost();

        std::vector<std::function<void()>> firstCallbacks;
        {
            std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
            if(firstRealizeFired_){
                return;
            }
            firstRealizeFired_ = true;
            nativeReady_.store(true, std::memory_order_release);
            firstCallbacks.swap(firstRealizeSubscribers_);
        }
        for(auto & cb : firstCallbacks){
            if(cb) cb();
        }
    }

    // ---- GdkSurface signal trampolines ----

    static void onComputeSize(GdkToplevel *,GdkToplevelSize *size,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || size == nullptr) return;
        gdk_toplevel_size_set_size(size,self->requestedW_,self->requestedH_);
        if(self->minW_ > 0 && self->minH_ > 0){
            gdk_toplevel_size_set_min_size(size,self->minW_,self->minH_);
        }
    }

    static void onLayout(GdkSurface *,int width,int height,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || width <= 0 || height <= 0) return;
        const bool firstLayout = !self->firstRealizeFired_;
        const bool sizeChanged =
            (self->rect.w != (float)width) || (self->rect.h != (float)height);
        self->logicalW_ = width;
        self->logicalH_ = height;
        // Adopt the compositor-negotiated size as the new requested size.
        // Otherwise onComputeSize keeps proposing the construction-time
        // requestedW_/H_ on every cycle and the surface snaps back to it after
        // (or during) a compositor-driven interactive resize. A programmatic
        // setRect still overrides this by writing requestedW_/H_ then calling
        // request_layout.
        self->requestedW_ = width;
        self->requestedH_ = height;
        self->integerScale_ = std::max(1,gdk_surface_get_scale_factor(self->surface));
        self->applyWaylandBufferScale();
        self->rect.w = (float)width;
        self->rect.h = (float)height;
        if(self->rootView != nullptr){
            self->rootView->resize(self->rect);
        }
        // First configured layout is our realize signal: the protocol handle is
        // valid now, so feed the SurfaceHost and release the deferred paint.
        self->handleFirstRealize();

        // Once the surface is up, a *changed* layout size is a live,
        // compositor-driven resize (edge drag, maximize, tiling). Emit the
        // resize events AppWindow consumes so the swap chain rebuilds at the
        // new size — mirroring the GTK-3 configure-event path. Without this,
        // WTK keeps presenting the old-size buffer and the compositor reverts
        // the window to match it (the snap-back the diagnostics showed: the
        // layout grew to 682 but the buffer stayed 600, so it bounced back).
        // The initial layout establishes the startup size through the normal
        // bring-up path, so it is excluded.
        if(!firstLayout && sizeChanged){
            Composition::Rect resizeRect{Composition::Point2D{0.f,0.f},
                                         (float)width,(float)height};
            self->emitEvent(NativeEvent::WindowWillResize,
                new Native::WindowWillResize(resizeRect));
            self->queueResizeFinishedEvent();
        }
    }

    // (Re)arm the debounced WindowHasFinishedResize. Each live-resize "layout"
    // pushes the finish out 120ms; the event fires once the resize stream
    // settles, so AppWindow runs dispatchResizeEndToHosts exactly once.
    void queueResizeFinishedEvent(){
        if(resizeFinishDebounceSource_ != 0){
            g_source_remove(resizeFinishDebounceSource_);
            resizeFinishDebounceSource_ = 0;
        }
        resizeFinishDebounceSource_ = g_timeout_add(120,[](gpointer data) -> gboolean {
            auto *self = static_cast<GTKAppWindow *>(data);
            self->resizeFinishDebounceSource_ = 0;
            self->emitEvent(NativeEvent::WindowHasFinishedResize,nullptr);
            return G_SOURCE_REMOVE;
        },this);
    }

    // Construct the NativeEvent and emit it. NativeEvent owns `params` and
    // frees it in its destructor, so the no-receiver path leaks nothing — build
    // unconditionally, emit only when a receiver is attached.
    void emitEvent(NativeEvent::EventType type,NativeEventParams params){
        NativeEventPtr event(new NativeEvent(type,params));
        if(hasEventEmitter()){
            eventEmitter()->emit(event);
        }
    }

    // ---- G4-4: GdkEvent → NativeEvent translation ----
    //
    // The bare GdkSurface "event" signal is the single input source (Option B′:
    // no GtkWidget, hence no GtkEventController stack). Each handler mirrors the
    // GTK-3 backend's per-signal handlers (GTKAppWindow.cpp) but reads through
    // the GTK-4 typed-event accessors and emits through the window's own
    // eventEmitter so the stream lands where Win32 / macOS deliver it.
    //
    // GTK 4 GdkEvents carry no root/screen coordinate (deliberately dropped —
    // Wayland has no global coordinate space), so screenPosition mirrors the
    // surface-local position. Virtual hit-testing reads `position`; nothing in
    // the tree relies on a true screen origin today.

    gboolean handleMotion(GdkEvent *event){
        double x = 0, y = 0;
        gdk_event_get_position(event,&x,&y);
        lastPointerX_ = x; lastPointerY_ = y;
        // Edge-resize affordance: over the resize band of a resizable bare
        // surface, show the matching resize cursor (the window chrome WTK draws
        // itself); otherwise honour the active declarative cursor shape.
        if(resizable_ && surface != nullptr){
            GdkSurfaceEdge edge;
            const char *name = edgeForPoint(x,y,logicalW_,logicalH_,&edge)
                ? cursorNameForEdge(edge)
                : cursorNameForShape(currentCursorShape_);
            GdkCursor *cursor = gdk_cursor_new_from_name(name,nullptr);
            gdk_surface_set_cursor(surface,cursor);
            if(cursor) g_object_unref(cursor);
        }
        auto *params = new CursorMoveParams();
        params->position = Composition::Point2D{(float)x,(float)y};
        params->screenPosition = params->position;
        params->modifiers = modifierFlagsFromState(gdk_event_get_modifier_state(event));
        emitEvent(NativeEvent::CursorMove,params);
        return FALSE;
    }

    gboolean handleButton(GdkEvent *event,bool pressed){
        double x = 0, y = 0;
        gdk_event_get_position(event,&x,&y);
        lastPointerX_ = x; lastPointerY_ = y;
        guint button = gdk_button_event_get_button(event);
        // A primary-button press inside the resize band starts a compositor-
        // driven resize and is consumed before the virtual layer sees it (on
        // Wayland the compositor is the only thing that can drive a resize).
        if(pressed && resizable_ && surface != nullptr && button == GDK_BUTTON_PRIMARY){
            GdkSurfaceEdge edge;
            if(edgeForPoint(x,y,logicalW_,logicalH_,&edge)){
                gdk_toplevel_begin_resize(GDK_TOPLEVEL(surface),edge,
                    gdk_event_get_device(event),(int)button,x,y,
                    gdk_event_get_time(event));
                return TRUE;
            }
        }
        NativeEvent::EventType type;
        if(button == GDK_BUTTON_PRIMARY){
            type = pressed ? NativeEvent::LMouseDown : NativeEvent::LMouseUp;
        }
        else if(button == GDK_BUTTON_SECONDARY){
            type = pressed ? NativeEvent::RMouseDown : NativeEvent::RMouseUp;
        }
        else {
            // Middle / extra buttons have no NativeEvent mapping yet.
            return FALSE;
        }
        Composition::Point2D pos{(float)x,(float)y};
        ModifierFlags mods = modifierFlagsFromState(gdk_event_get_modifier_state(event));
        NativeEventParams params = nullptr;
        switch(type){
            case NativeEvent::LMouseDown: {
                auto *p = new LMouseDownParams();
                p->position = pos; p->screenPosition = pos; p->modifiers = mods; p->clickCount = 1u;
                params = p; break;
            }
            case NativeEvent::LMouseUp: {
                auto *p = new LMouseUpParams();
                p->position = pos; p->screenPosition = pos; p->modifiers = mods; p->clickCount = 1u;
                params = p; break;
            }
            case NativeEvent::RMouseDown: {
                auto *p = new RMouseDownParams();
                p->position = pos; p->screenPosition = pos; p->modifiers = mods; p->clickCount = 1u;
                params = p; break;
            }
            default: {
                auto *p = new RMouseUpParams();
                p->position = pos; p->screenPosition = pos; p->modifiers = mods; p->clickCount = 1u;
                params = p; break;
            }
        }
        emitEvent(type,params);
        return FALSE;
    }

    gboolean handleKey(GdkEvent *event,bool pressed){
        guint keyval = gdk_key_event_get_keyval(event);
        ModifierFlags mods = modifierFlagsFromState(gdk_event_get_modifier_state(event));
        KeyCode code = keyCodeFromGdk(keyval);
        auto key = static_cast<OmegaCommon::Unicode32Char>(gdk_keyval_to_unicode(keyval));
        if(pressed){
            auto *p = new KeyDownParams();
            p->code = code; p->key = key; p->modifiers = mods;
            // GTK 4 delivers auto-repeat as a stream of presses with no public
            // repeat flag; leave isRepeat false (consumers track their own).
            emitEvent(NativeEvent::KeyDown,p);
        }
        else {
            auto *p = new KeyUpParams();
            p->code = code; p->key = key; p->modifiers = mods;
            emitEvent(NativeEvent::KeyUp,p);
        }
        return FALSE;
    }

    gboolean handleScroll(GdkEvent *event){
        double x = 0, y = 0;
        // GTK 4 scroll (axis) events carry no position (gdk_event_get_position
        // returns FALSE, leaving x/y NaN). Fall back to the last pointer
        // location so the wheel target hit-tests against the view under the
        // cursor rather than (NaN,NaN) — which matches no view, so the
        // ScrollView never received the event.
        if(!gdk_event_get_position(event,&x,&y) || !std::isfinite(x) || !std::isfinite(y)){
            x = lastPointerX_;
            y = lastPointerY_;
        }
        Composition::Point2D scrollPos{(float)x,(float)y};
        constexpr double epsilon = 0.0001;
        GdkScrollDirection dir = gdk_scroll_event_get_direction(event);
        if(dir == GDK_SCROLL_SMOOTH){
            double dx = 0, dy = 0;
            gdk_scroll_event_get_deltas(event,&dx,&dy);
            if(std::fabs(dx) > epsilon || std::fabs(dy) > epsilon){
                emitEvent(NativeEvent::ScrollWheel,
                    new ScrollParams{(float)dx,(float)dy,scrollPos});
            }
            if(std::fabs(dx) > epsilon){
                emitEvent(dx > 0.0 ? NativeEvent::ScrollRight : NativeEvent::ScrollLeft,
                    new ScrollParams{(float)dx,0.f,scrollPos});
            }
            if(std::fabs(dy) > epsilon){
                emitEvent(dy > 0.0 ? NativeEvent::ScrollDown : NativeEvent::ScrollUp,
                    new ScrollParams{0.f,(float)dy,scrollPos});
            }
            return FALSE;
        }
        float dirDx = 0.f, dirDy = 0.f;
        switch(dir){
            case GDK_SCROLL_UP:
                dirDy = -1.f;
                emitEvent(NativeEvent::ScrollUp,new ScrollParams{0.f,-1.f,scrollPos});
                break;
            case GDK_SCROLL_DOWN:
                dirDy = 1.f;
                emitEvent(NativeEvent::ScrollDown,new ScrollParams{0.f,1.f,scrollPos});
                break;
            case GDK_SCROLL_LEFT:
                dirDx = -1.f;
                emitEvent(NativeEvent::ScrollLeft,new ScrollParams{-1.f,0.f,scrollPos});
                break;
            case GDK_SCROLL_RIGHT:
                dirDx = 1.f;
                emitEvent(NativeEvent::ScrollRight,new ScrollParams{1.f,0.f,scrollPos});
                break;
            default:
                break;
        }
        if(std::fabs(dirDx) > epsilon || std::fabs(dirDy) > epsilon){
            emitEvent(NativeEvent::ScrollWheel,
                new ScrollParams{dirDx * 10.f,dirDy * 10.f,scrollPos});
        }
        return FALSE;
    }

    gboolean handleCrossing(GdkEvent *event,bool enter){
        double x = 0, y = 0;
        gdk_event_get_position(event,&x,&y);
        lastPointerX_ = x; lastPointerY_ = y;
        if(enter){
            auto *p = new CursorEnterParams();
            p->position = Composition::Point2D{(float)x,(float)y};
            emitEvent(NativeEvent::CursorEnter,p);
        }
        else {
            auto *p = new CursorExitParams();
            p->position = Composition::Point2D{(float)x,(float)y};
            emitEvent(NativeEvent::CursorExit,p);
        }
        return FALSE;
    }

    static gboolean onEvent(GdkSurface *,GdkEvent *event,gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr || event == nullptr) return FALSE;
        switch(gdk_event_get_event_type(event)){
            case GDK_DELETE:
                // Compositor close request. Hide the surface; the app-level
                // close flow is driven through AppWindow.
                self->disable();
                return TRUE;
            case GDK_MOTION_NOTIFY:  return self->handleMotion(event);
            case GDK_BUTTON_PRESS:   return self->handleButton(event,true);
            case GDK_BUTTON_RELEASE: return self->handleButton(event,false);
            case GDK_KEY_PRESS:      return self->handleKey(event,true);
            case GDK_KEY_RELEASE:    return self->handleKey(event,false);
            case GDK_SCROLL:         return self->handleScroll(event);
            case GDK_ENTER_NOTIFY:   return self->handleCrossing(event,true);
            case GDK_LEAVE_NOTIFY:   return self->handleCrossing(event,false);
            default:
                return FALSE;
        }
    }

    static gboolean onFrameFlushIdle(gpointer data){
        auto *self = static_cast<GTKAppWindow *>(data);
        if(self == nullptr) return G_SOURCE_REMOVE;
        self->frameFlushIdleSource_ = 0;
        self->frameFlushQueued_.store(false, std::memory_order_release);
        if(self->frameFlushCallback_){
            self->frameFlushCallback_();
        }
        return G_SOURCE_REMOVE;
    }

public:
    GTKAppWindow(Composition::Rect &rectArg,NativeEventEmitter *emitter,const NativeScreenDesc *screen):
    NativeWindow(sanitizeRect(rectArg,Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f}), emitter){
        (void)screen;
        GdkDisplay *display = gdk_display_get_default();
        if(display == nullptr){
            std::cerr << "[OmegaWTK][GTK] No active GDK display. Skipping native window creation." << std::endl;
            return;
        }

        requestedW_ = std::max(1,(int)std::lround(this->rect.w));
        requestedH_ = std::max(1,(int)std::lround(this->rect.h));
        logicalW_ = requestedW_;
        logicalH_ = requestedH_;

        // Bare toplevel surface — no GtkWidget, hence no GskRenderer.
        surface = gdk_surface_new_toplevel(display);
        if(surface == nullptr){
            std::cerr << "[OmegaWTK][GTK] gdk_surface_new_toplevel failed." << std::endl;
            return;
        }

        // §2.13a: construct the SurfaceHost for the runtime-detected protocol.
        switch(detectWindowingBackend(display)){
#if WTK_NATIVE_X11
        case WindowingBackend::X11: {
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            ::Display *xdpy = gdk_x11_display_get_xdisplay(display);
G_GNUC_END_IGNORE_DEPRECATIONS
            if(xdpy != nullptr){
                surfaceHost_ = std::make_unique<X11SurfaceHost>(xdpy);
            }
            break;
        }
#endif
#if WTK_NATIVE_WAYLAND
        case WindowingBackend::Wayland: {
            wl_display *wdpy = gdk_wayland_display_get_wl_display(display);
            if(wdpy != nullptr){
                surfaceHost_ = std::make_unique<WaylandSurfaceHost>(wdpy);
            }
            break;
        }
#endif
        default:
            std::cerr << "[OmegaWTK][GTK] Unknown windowing backend; no SurfaceHost constructed." << std::endl;
            break;
        }

        integerScale_ = std::max(1,gdk_surface_get_scale_factor(surface));

        toplevelLayout = gdk_toplevel_layout_new();
        gdk_toplevel_layout_set_resizable(toplevelLayout,resizable_ ? TRUE : FALSE);

        // Borderless by default — the bare surface has no native chrome and
        // (Option B′) WTK draws its own. Decorations are a Part B / Panels
        // concern; keep CSD off so the compositor does not contend for the
        // surface.
        gdk_toplevel_set_decorated(GDK_TOPLEVEL(surface),FALSE);

        g_signal_connect(surface,"compute-size",G_CALLBACK(onComputeSize),this);
        g_signal_connect(surface,"layout",G_CALLBACK(onLayout),this);
        g_signal_connect(surface,"event",G_CALLBACK(onEvent),this);
        // §2.2 integer-scale change notifications (window dragged to a
        // different-DPI monitor, or the display scale crosses an integer step).
        g_signal_connect(surface,"notify::scale-factor",G_CALLBACK(onNotifyScaleFactor),this);

        // Root NativeItem wraps the bare surface (borrowed — we own its
        // lifetime). No GtkWidget, no input-signal attachment here.
        rootView = std::make_shared<GTKItem>(this->rect, surface);
    }

    NativeItemPtr getRootView() override {
        return std::static_pointer_cast<NativeItem>(rootView);
    }

    /// §2.13a accessor: the per-window SurfaceHost (X11 or Wayland).
    SurfaceHost *getSurfaceHost() { return surfaceHost_.get(); }

    void enable() override { presentToplevel(); }

    void disable() override {
        if(surface != nullptr){
            gdk_surface_hide(surface);
        }
    }

    void initialDisplay() override { presentToplevel(); }

    void close() override {
        if(surface != nullptr){
            gdk_surface_hide(surface);
        }
    }

    // Menus are removed by Panels-And-Window-Customization-Plan §B3; the bare
    // surface has no menu bar.
    void setMenu(NM menu) override { this->menu = menu; }

    void setTitle(OmegaCommon::StrRef title) override {
        if(surface != nullptr){
            gdk_toplevel_set_title(GDK_TOPLEVEL(surface),title.data());
        }
    }

    void setEnableWindowHeader(bool & enable) override {
        if(surface != nullptr){
            gdk_toplevel_set_decorated(GDK_TOPLEVEL(surface),enable ? TRUE : FALSE);
        }
    }

    void minimize() override {
        if(surface != nullptr){
            gdk_toplevel_minimize(GDK_TOPLEVEL(surface));
        }
    }

    void maximize() override {
        if(toplevelLayout != nullptr){
            gdk_toplevel_layout_set_maximized(toplevelLayout,TRUE);
            presentToplevel();
        }
    }

    void restore() override {
        if(toplevelLayout != nullptr){
            gdk_toplevel_layout_set_maximized(toplevelLayout,FALSE);
            gdk_toplevel_layout_set_fullscreen(toplevelLayout,FALSE,nullptr);
            presentToplevel();
        }
    }

    void toggleFullscreen() override {
        if(toplevelLayout != nullptr){
            gdk_toplevel_layout_set_fullscreen(toplevelLayout,!isFullscreen(),nullptr);
            presentToplevel();
        }
    }

    bool isMinimized() const override {
        return toplevel() != nullptr &&
            (gdk_toplevel_get_state(toplevel()) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
    }
    bool isMaximized() const override {
        return toplevel() != nullptr &&
            (gdk_toplevel_get_state(toplevel()) & GDK_TOPLEVEL_STATE_MAXIMIZED) != 0;
    }
    bool isFullscreen() const override {
        return toplevel() != nullptr &&
            (gdk_toplevel_get_state(toplevel()) & GDK_TOPLEVEL_STATE_FULLSCREEN) != 0;
    }
    bool isVisible() const override {
        return surface != nullptr && gdk_surface_get_mapped(surface) == TRUE;
    }

    Composition::Rect getRect() const override {
        Composition::Rect r = this->rect;
        r.w = (float)logicalW_;
        r.h = (float)logicalH_;
        return r;
    }

    void setRect(const Composition::Rect & r) override {
        Composition::Rect sane = sanitizeRect(r,this->rect);
        this->rect = sane;
        // Wayland exposes no absolute window position, and a GdkToplevel is
        // resized through size negotiation: update the requested size and ask
        // the compositor to recompute. Position is compositor-controlled.
        requestedW_ = std::max(1,(int)std::lround(sane.w));
        requestedH_ = std::max(1,(int)std::lround(sane.h));
        if(surface != nullptr){
            gdk_surface_request_layout(surface);
        }
    }

    void setMinSize(float w, float h) override {
        minW_ = std::max(0,(int)std::lround(w));
        minH_ = std::max(0,(int)std::lround(h));
    }

    // No GdkToplevel max-size API; tracked for parity but not enforceable here.
    void setMaxSize(float w, float h) override { (void)w; (void)h; }

    void setResizable(bool resizable) override {
        resizable_ = resizable;
        if(toplevelLayout != nullptr){
            gdk_toplevel_layout_set_resizable(toplevelLayout,resizable ? TRUE : FALSE);
            presentToplevel();
        }
    }

    void orderFront() override { presentToplevel(); }

    void orderBack() override {
        if(surface != nullptr){
            gdk_toplevel_lower(GDK_TOPLEVEL(surface));
        }
    }

    // GTK 4 has no per-GdkSurface opacity (it was a GtkWidget property); track
    // the value for getOpacity but it is not applied to the bare surface.
    void setOpacity(float alpha) override { opacity_ = alpha; }
    float getOpacity() const override { return opacity_; }

    void setCursorShape(CursorShape shape) override {
        currentCursorShape_ = shape;
        if(surface != nullptr){
            GdkCursor *cursor = gdk_cursor_new_from_name(cursorNameForShape(shape),nullptr);
            gdk_surface_set_cursor(surface,cursor);
            if(cursor) g_object_unref(cursor);
        }
    }

    // §2.2: per-surface integer render scale (gdk_surface_get_scale_factor).
    // Overrides the base currentScreen() forwarder so a multi-monitor window
    // reports the scale of the monitor IT is on, and stays consistent with the
    // integer buffer scale we commit (see applyWaylandBufferScale). The
    // fractional output scale is the compositor's concern, not a value the
    // render pipeline sizes against.
    float scaleFactor() const override { return (float)std::max(1,integerScale_); }

    bool isKeyWindow() const override {
        return toplevel() != nullptr &&
            (gdk_toplevel_get_state(toplevel()) & GDK_TOPLEVEL_STATE_FOCUSED) != 0;
    }

    void becomeKeyWindow() override {
        if(surface != nullptr){
            gdk_toplevel_focus(GDK_TOPLEVEL(surface),GDK_CURRENT_TIME);
        }
    }

    void requestFrameFlush() override {
        bool expected = false;
        if(!frameFlushQueued_.compare_exchange_strong(expected, true)){
            return;
        }
        frameFlushIdleSource_ = g_idle_add(&GTKAppWindow::onFrameFlushIdle, this);
        if(frameFlushIdleSource_ == 0){
            frameFlushQueued_.store(false, std::memory_order_release);
        }
    }

    bool isNativeReady() const override {
        return nativeReady_.load(std::memory_order_acquire);
    }

    void onFirstRealize(std::function<void()> cb) override {
        if(!cb) return;
        {
            std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
            if(!firstRealizeFired_){
                firstRealizeSubscribers_.push_back(std::move(cb));
                return;
            }
        }
        cb();
    }

    void onRealize(std::function<void()> cb) override {
        if(!cb) return;
        std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
        realizeSubscribers_.push_back(std::move(cb));
    }

    ~GTKAppWindow() override {
        if(frameFlushIdleSource_ != 0){
            g_source_remove(frameFlushIdleSource_);
            frameFlushIdleSource_ = 0;
        }
        if(resizeFinishDebounceSource_ != 0){
            g_source_remove(resizeFinishDebounceSource_);
            resizeFinishDebounceSource_ = 0;
        }
        // Tear the rootView down before the surface (it borrows the handle),
        // then the SurfaceHost after the surface so child surfaces release
        // against a live display.
        rootView = nullptr;
        if(toplevelLayout != nullptr){
            gdk_toplevel_layout_unref(toplevelLayout);
            toplevelLayout = nullptr;
        }
        if(surface != nullptr){
            gdk_surface_destroy(surface);
            surface = nullptr;
        }
        surfaceHost_.reset();
    }
};

GtkWindow *gtk_window_from_native(const NWH & window){
    // Option B′ has no GtkWindow backing the toplevel; dialogs that want a
    // transient parent get none on GTK 4 (handled at the dialog layer).
    (void)window;
    return nullptr;
}

SurfaceHost *surface_host_from_native(const NWH & window){
    auto appWindow = std::dynamic_pointer_cast<GTKAppWindow>(window);
    return appWindow ? appWindow->getSurfaceHost() : nullptr;
}

float gtk_menu_bar_inset_from_native(const NWH & window){
    // No native menu bar in the bare-surface model (menus are virtual — Panels
    // §B3); there is no inset to subtract.
    (void)window;
    return 0.f;
}

}

namespace OmegaWTK::Native {

NWH make_native_window(Composition::Rect &rect,NativeEventEmitter *emitter,const NativeScreenDesc *screen){
    return (NWH)new GTK::GTKAppWindow(rect,emitter,screen);
}

}
