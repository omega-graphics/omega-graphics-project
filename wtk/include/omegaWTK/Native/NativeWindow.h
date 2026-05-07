#include "omegaWTK/Core/Core.h"
#include "NativeEvent.h"
#include "NativeItem.h"
#include "NativeMenu.h"

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
    public:
        NativeWindow(Composition::Rect rect, NativeEventEmitter *emitter);

        bool hasEventEmitter() const;
        NativeEventEmitter *eventEmitter() const;

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
