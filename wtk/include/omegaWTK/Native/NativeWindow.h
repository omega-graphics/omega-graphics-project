#include "omegaWTK/Core/Core.h"
#include "NativeEvent.h"
#include "NativeItem.h"
#include "NativeMenu.h"

#include <functional>

#ifndef OMEGAWTK_NATIVE_NATIVEWINDOW_H
#define OMEGAWTK_NATIVE_NATIVEWINDOW_H

// X11's <X.h> defines a `CursorShape` macro (largest cursor size). It can
// be pulled into a translation unit transitively via Vulkan's xlib/xcb
// platform headers or GTK. Undef it so our enum survives.
#ifdef CursorShape
#undef CursorShape
#endif

namespace OmegaWTK {
    class AppWindow;
}
namespace OmegaWTK::Native {

    /// Cross-platform cursor shapes. Committed by the virtual hover
    /// dispatcher (WidgetTreeHost) to the single root NativeWindow via
    /// NativeWindow::setCursorShape — Views never touch the OS cursor
    /// directly.
    enum class CursorShape : int {
        Arrow,
        IBeam,
        Crosshair,
        PointingHand,
        ResizeLeftRight,
        ResizeUpDown,
        ResizeAll,
        NotAllowed,
        Wait,
        Custom
    };

    INTERFACE NativeWindow {
        friend class ::OmegaWTK::AppWindow;
        INTERFACE_METHOD NativeItemPtr getRootView() ABSTRACT;
    protected:
        OmegaCommon::Vector<NativeItemPtr> windowWidgetRootViews;
        NM menu = nullptr;
        Composition::Rect rect;
        NativeEventEmitter *windowEventEmitter = nullptr;
        /// Widget-View-Paint-Lifecycle-Plan Tier A: invoked once per
        /// coalesced frame request (see requestFrameFlush). Set by
        /// AppWindow to its flushFrame().
        std::function<void()> frameFlushCallback_;
    public:
        NativeWindow(Composition::Rect rect, NativeEventEmitter *emitter);

        bool hasEventEmitter() const;
        NativeEventEmitter *eventEmitter() const;

        /// Register the coalesced frame-flush callback (AppWindow's
        /// flushFrame). Invoked from requestFrameFlush.
        void setFrameFlushCallback(std::function<void()> cb);
        /// Request that the frame-flush callback run once on the next
        /// run-loop turn, coalescing a burst of requests into a single
        /// invocation. Base implementation invokes synchronously
        /// (no deferral); platforms with a run loop override to
        /// schedule + coalesce (macOS: CFRunLoopPerformBlock).
        virtual void requestFrameFlush();

        /// Whether the native surface is realized and the engine may
        /// render into it. Backends with a meaningful gap between
        /// window construction and surface availability (GTK/X11)
        /// override. The default `return true` is provisional
        /// — it lines up with today's macOS/Windows behavior only
        /// because `PaintOptions::autoWarmupOnInitialPaint` resubmits
        /// the initial frame; once warmup is removed
        /// (`Widget-View-Paint-Lifecycle-Plan.md` Tier D) every backend
        /// must override with a real check.
        ///
        /// May transiently return false during a re-realize cycle (DPI
        /// change, display reconfiguration, surface re-host) and then
        /// return true again; subscribers to `onRealize` observe that
        /// transition.
        ///
        /// Queried from the CompositorFrameWorker thread. Implementations
        /// should back this with an std::atomic<bool>.
        ///
        /// See `wtk/.plans/NativeWindow-Ready-Signal-Plan.md`.
        virtual bool isNativeReady() const { return true; }

        /// Register a one-shot callback that fires exactly once when
        /// the native surface is realized for the first time. If
        /// `isNativeReady()` is already true at registration time,
        /// fires synchronously on the calling thread (replays the
        /// past first-realize event for late subscribers). Otherwise
        /// fires on the platform's UI thread when the initial realize
        /// signal arrives. Never fires again — subsequent re-realize
        /// transitions are delivered through `onRealize`.
        ///
        /// Used by the Compositor to release the deferred initial
        /// paint queued in the window's CompositorSurface.
        ///
        /// Registration is permanent for the NativeWindow's lifetime;
        /// there is no unregister. Callbacks must capture weak
        /// references to any state they touch and must not take locks
        /// held by the caller (the synchronous-replay path runs on the
        /// caller's thread).
        virtual void onFirstRealize(std::function<void()> cb) {
            if(cb) cb();
        }

        /// Register a sticky (refireable) callback that fires on every
        /// *subsequent* re-realize transition — explicitly NOT on the
        /// first realize (that is delivered via `onFirstRealize`).
        /// Covers DPI scale change, Wayland scale/transform update,
        /// Windows display-change recreation, macOS layer re-host on
        /// space/fullscreen transitions. Callbacks fire on the
        /// platform's UI thread in registration order.
        ///
        /// Default impl never fires: most windows on most platforms
        /// have a single realize cycle. Backends that genuinely
        /// re-realize override.
        ///
        /// Consumed by `UIView-Render-Redesign-Plan.md` Phase F to
        /// force a full-tree repaint on every re-realize. The split
        /// from `onFirstRealize` is what guarantees Phase F's
        /// full-repaint walker does not run redundantly at startup
        /// alongside the deferred-paint release.
        ///
        /// Registration is permanent for the NativeWindow's lifetime;
        /// there is no unregister. Same callback discipline as
        /// `onFirstRealize`.
        virtual void onRealize(std::function<void()> cb) {
            (void)cb;
        }

        virtual void enable() = 0;
        virtual void disable() = 0;
        virtual void initialDisplay() = 0;
        virtual void close() = 0;
        virtual void setMenu(NM menu) = 0;
        virtual void setTitle(OmegaCommon::StrRef title) = 0;
        virtual void setEnableWindowHeader(bool & enable) = 0;

        // -- Window state --
        virtual void minimize() = 0;
        virtual void maximize() = 0;
        virtual void restore() = 0;
        virtual void toggleFullscreen() = 0;
        virtual bool isMinimized() const = 0;
        virtual bool isMaximized() const = 0;
        virtual bool isFullscreen() const = 0;
        virtual bool isVisible() const = 0;

        virtual Composition::Rect getRect() const = 0;
        virtual void setRect(const Composition::Rect & rect) = 0;

        /// DPI / backing scale. Changes at runtime emit
        /// NativeEvent::WindowScaleFactorChanged through eventEmitter().
        virtual float scaleFactor() const = 0;

        virtual void setMinSize(float w, float h) = 0;
        virtual void setMaxSize(float w, float h) = 0;

        virtual void setResizable(bool resizable) = 0;

        virtual void orderFront() = 0;
        virtual void orderBack() = 0;

        // -- OS sinks (single per-window) --
        /// Window-wide alpha. macOS: NSWindow.alphaValue. Win32:
        /// SetLayeredWindowAttributes. GTK: gtk_widget_set_opacity.
        virtual void setOpacity(float alpha) = 0;
        virtual float getOpacity() const = 0;

        /// Cursor sink. The virtual hover dispatcher commits the active
        /// view's CursorShape here. macOS: [NSCursor set]. Win32:
        /// SetCursor. GTK: gdk_window_set_cursor on the toplevel.
        virtual void setCursorShape(CursorShape shape) = 0;

        /// Key/main window state — drives where keyboard events land
        /// before the virtual focus manager picks a target.
        virtual bool isKeyWindow() const = 0;
        virtual void becomeKeyWindow() = 0;

        virtual ~NativeWindow() = default;
    };
    typedef SharedHandle<NativeWindow> NWH;
    NWH make_native_window(Composition::Rect & rect,NativeEventEmitter *emitter);
};

#endif
