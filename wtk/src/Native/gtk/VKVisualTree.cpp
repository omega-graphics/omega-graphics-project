#include "NativePrivate/gtk/VKVisualTree.h"

namespace OmegaWTK::Native::GTK {

    VKVisual::VKVisual(SharedHandle<GTKItem> item, Composition::Rect rect,
                       WindowingBackend backend):
        Visual(rect),
        item_(std::move(item)),
        backend_(backend) {}

// Independent #ifs so the BOTH co-build defines both accessor sets; the
// runtime backend() picks which one the binder reads.
#if WTK_NATIVE_WAYLAND
    wl_surface * VKVisual::waylandSurface() const {
        return item_ != nullptr ? item_->getWaylandSurface() : nullptr;
    }
    wl_display * VKVisual::waylandDisplay() const {
        return item_ != nullptr ? item_->getWaylandDisplay() : nullptr;
    }
#endif
#if WTK_NATIVE_X11
    ::Display * VKVisual::x11Display() const {
        return item_ != nullptr ? item_->getX11Display() : nullptr;
    }
    ::Window VKVisual::x11Window() const {
        return item_ != nullptr ? item_->getX11Window() : 0;
    }
#endif

    VKVisualTree::VKVisualTree(SharedHandle<GTKItem> rootItem,
                                Composition::Rect rect,
                                float scale,
                                WindowingBackend backend):
        rootVisual_(std::make_shared<VKVisual>(std::move(rootItem), rect, backend)),
        scale_(scale) {}

    Native::Visual * VKVisualTree::rootVisual() const {
        return rootVisual_.get();
    }

}

namespace OmegaWTK::Native {

    NativeVisualTreePtr make_native_visual_tree(NativeItemPtr rootItem,
                                                 const Composition::Rect & rect,
                                                 float scale){
        // The Linux factory needs the GTKItem-typed root so the binder
        // can fish the X11/Wayland handles out. Downcast guarded — a
        // caller that hands us a non-GTKItem is a programming error.
        auto gtkItem = std::dynamic_pointer_cast<GTK::GTKItem>(rootItem);
        if(gtkItem == nullptr){
            return nullptr;
        }
        // §2.13a: resolve the live windowing protocol once from the root
        // item's GdkDisplay and stamp it onto the tree's VKVisual, so the
        // binder fills the surface descriptor by runtime backend rather than
        // a compile-time #if. GDK has already bound to exactly one protocol,
        // so this is deterministic (see detectWindowingBackend).
        GdkSurface *surface = gtkItem->getSurface();
        GdkDisplay *display = surface != nullptr ? gdk_surface_get_display(surface) : nullptr;
        GTK::WindowingBackend backend = GTK::detectWindowingBackend(display);
        return std::make_shared<GTK::VKVisualTree>(std::move(gtkItem), rect, scale, backend);
    }

}
