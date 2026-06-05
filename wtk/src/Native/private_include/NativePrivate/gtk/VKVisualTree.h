#ifndef OMEGAWTK_NATIVEPRIVATE_GTK_VKVISUALTREE_H
#define OMEGAWTK_NATIVEPRIVATE_GTK_VKVISUALTREE_H

#include "omegaWTK/Native/NativeVisualTree.h"
#include "NativePrivate/gtk/GTKItem.h"

#include <gdk/gdk.h>
#if WTK_NATIVE_WAYLAND
#  include <wayland-client.h>
#elif WTK_NATIVE_X11
#  include <X11/Xlib.h>
#endif

namespace OmegaWTK::Native::GTK {

    /// Linux Visual subclass. Carries the GTKItem reference whose
    /// X11/Wayland handles back the platform present surface (§2.13
    /// X11SurfaceHost owns the toplevel `::Window` on X11; the root
    /// VKVisual re-surfaces it here for the Composition binder so the
    /// binder can wire up the GENativeRenderTarget).
    ///
    /// The accessors are conditionally compiled on the same
    /// `WTK_NATIVE_X11` / `WTK_NATIVE_WAYLAND` switch as `GTKItem` —
    /// a single build never has both sets live, matching the
    /// per-protocol surface descriptor `GENativeRenderTarget` expects.
    class VKVisual : public Native::Visual {
    public:
        VKVisual(SharedHandle<GTKItem> item, Composition::Rect rect);

#if WTK_NATIVE_WAYLAND
        wl_surface * waylandSurface() const;
        wl_display * waylandDisplay() const;
#elif WTK_NATIVE_X11
        ::Display * x11Display() const;
        ::Window    x11Window()  const;
#endif

        /// The owning GTKItem — held so the binder can re-query the
        /// X11/Wayland handles on a deferred-bind retry without
        /// reaching back through the AppWindow surface.
        const SharedHandle<GTKItem> & gtkItem() const { return item_; }

    private:
        SharedHandle<GTKItem> item_;
    };

    /// Linux VisualTree. Holds the single root VKVisual; Pass 2 will
    /// add `addContentNode` / `removeContentNode` for the
    /// NativeViewHost adoption work — Pass 1 ships the root only.
    class VKVisualTree : public Native::VisualTree {
    public:
        VKVisualTree(SharedHandle<GTKItem> rootItem,
                     Composition::Rect rect,
                     float scale);

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
