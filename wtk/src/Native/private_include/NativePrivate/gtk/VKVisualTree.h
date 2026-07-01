#ifndef OMEGAWTK_NATIVEPRIVATE_GTK_VKVISUALTREE_H
#define OMEGAWTK_NATIVEPRIVATE_GTK_VKVISUALTREE_H

#include "omegaWTK/Native/NativeVisualTree.h"
#include "NativePrivate/gtk/GTKItem.h"
#include "NativePrivate/gtk/GTKWindowing.h"

#include <gdk/gdk.h>
// Independent #ifs (not #if/#elif): the §2.13a BOTH co-build defines both
// protocol macros and needs both surface-handle headers present so VKVisual
// can carry both accessor sets at once. Single-protocol builds still compile
// exactly one. Xlib.h #defines None/True/False, but VisualBinder.h / the GTE
// descriptor headers are always included ahead of this in the binder TU, so
// the macro-clobber surface is the same as today's verified X11 build.
#if WTK_NATIVE_WAYLAND
#  include <wayland-client.h>
#endif
#if WTK_NATIVE_X11
#  include <X11/Xlib.h>
#endif

namespace OmegaWTK::Native::GTK {

    /// Linux Visual subclass. Carries the GTKItem reference whose
    /// X11/Wayland handles back the platform present surface (§2.13
    /// X11SurfaceHost owns the toplevel `::Window` on X11; the root
    /// VKVisual re-surfaces it here for the Composition binder so the
    /// binder can wire up the GENativeRenderTarget).
    ///
    /// The accessors are compiled behind independent `WTK_NATIVE_X11` /
    /// `WTK_NATIVE_WAYLAND` `#if`s, matching `GTKItem`. Under §2.13a's BOTH
    /// co-build both sets are live at once and `backend()` (stamped from the
    /// GdkDisplay by `make_native_visual_tree`) tells the binder which one to
    /// read — replacing the pre-§2.13a one-protocol-per-build assumption.
    class VKVisual : public Native::Visual {
    public:
        VKVisual(SharedHandle<GTKItem> item, Composition::Rect rect,
                 WindowingBackend backend);

        /// The live windowing protocol this visual's handles belong to.
        /// The binder switches on this at runtime instead of a compile-time
        /// `#if`, so one `OmegaWTK_Native` serves both X11 and Wayland.
        WindowingBackend backend() const { return backend_; }

#if WTK_NATIVE_WAYLAND
        wl_surface * waylandSurface() const;
        wl_display * waylandDisplay() const;
#endif
#if WTK_NATIVE_X11
        ::Display * x11Display() const;
        ::Window    x11Window()  const;
#endif

        /// The owning GTKItem — held so the binder can re-query the
        /// X11/Wayland handles on a deferred-bind retry without
        /// reaching back through the AppWindow surface.
        const SharedHandle<GTKItem> & gtkItem() const { return item_; }

    private:
        SharedHandle<GTKItem> item_;
        WindowingBackend backend_;
    };

    /// Linux VisualTree. Holds the single root VKVisual; Pass 2 will
    /// add `addContentNode` / `removeContentNode` for the
    /// NativeViewHost adoption work — Pass 1 ships the root only.
    class VKVisualTree : public Native::VisualTree {
    public:
        VKVisualTree(SharedHandle<GTKItem> rootItem,
                     Composition::Rect rect,
                     float scale,
                     WindowingBackend backend);

        Native::Visual * rootVisual() const override;

        /// Combined logical→physical scale at construction time.
        /// Read by the binder when sizing the backing render target.
        float scale() const { return scale_; }

    private:
        SharedHandle<VKVisual> rootVisual_;
        float scale_ = 1.f;
    };

}

#endif
