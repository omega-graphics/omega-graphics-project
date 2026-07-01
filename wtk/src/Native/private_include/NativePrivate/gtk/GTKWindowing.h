#ifndef OMEGAWTK_NATIVE_GTK_GTKWINDOWING_H
#define OMEGAWTK_NATIVE_GTK_GTKWINDOWING_H

#include <gtk/gtk.h>

// The GDK_IS_*_DISPLAY type-check macros live in the per-protocol GDK
// backend headers. Pull in whichever protocol(s) this build compiled for;
// in the §2.13a BOTH co-build both are present and both arms below are live.
#if WTK_NATIVE_WAYLAND
#  include <gdk/wayland/gdkwayland.h>
#endif
#if WTK_NATIVE_X11
#  include <gdk/x11/gdkx.h>
#endif

namespace OmegaWTK::Native::GTK {

    /// The live Linux windowing protocol behind a GdkDisplay.
    enum class WindowingBackend { Unknown, X11, Wayland };

    /// Resolve the live windowing protocol from a realized-or-unrealized
    /// GdkDisplay. GDK has already bound to exactly one backend by the time
    /// any GdkDisplay exists, so this is deterministic — no XDG_SESSION_TYPE
    /// fallback needed (that env var is advisory and can lie under XWayland).
    /// Under XWayland (an X11 app on a Wayland compositor) GDK reports an
    /// X11 display, which is correct: the app genuinely talks X11.
    ///
    /// The #if guards mean an X11-only build (backend mode `X11`) compiles
    /// this to "X11 or Unknown", a Wayland-only build to "Wayland or
    /// Unknown", and the `BOTH` co-build to the full three-way dispatch.
    inline WindowingBackend detectWindowingBackend(GdkDisplay *display) {
#if WTK_NATIVE_WAYLAND
        if (display != nullptr && GDK_IS_WAYLAND_DISPLAY(display)) {
            return WindowingBackend::Wayland;
        }
#endif
#if WTK_NATIVE_X11
        if (display != nullptr && GDK_IS_X11_DISPLAY(display)) {
            return WindowingBackend::X11;
        }
#endif
        (void)display;
        return WindowingBackend::Unknown;
    }

}

#endif
