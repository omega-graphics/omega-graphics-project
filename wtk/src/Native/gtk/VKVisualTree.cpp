#include "NativePrivate/gtk/VKVisualTree.h"

namespace OmegaWTK::Native::GTK {

    VKVisual::VKVisual(SharedHandle<GTKItem> item, Composition::Rect rect):
        Visual(rect),
        item_(std::move(item)) {}

#if WTK_NATIVE_WAYLAND
    wl_surface * VKVisual::waylandSurface() const {
        return item_ != nullptr ? item_->getSurface() : nullptr;
    }
    wl_display * VKVisual::waylandDisplay() const {
        return item_ != nullptr ? item_->getDisplay() : nullptr;
    }
#elif WTK_NATIVE_X11
    ::Display * VKVisual::x11Display() const {
        return item_ != nullptr ? item_->getDisplay() : nullptr;
    }
    ::Window VKVisual::x11Window() const {
        return item_ != nullptr ? item_->getX11Window() : 0;
    }
#endif

    VKVisualTree::VKVisualTree(SharedHandle<GTKItem> rootItem,
                                Composition::Rect rect,
                                float scale):
        rootVisual_(std::make_shared<VKVisual>(std::move(rootItem), rect)),
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
        return std::make_shared<GTK::VKVisualTree>(std::move(gtkItem), rect, scale);
    }

}
