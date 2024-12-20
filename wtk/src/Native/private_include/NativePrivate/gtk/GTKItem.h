#include "omegaWTK/Native/NativeItem.h"

#include <gtk/gtk.h>

#if WTK_NATIVE_WAYLAND

#include <gdk/gdkwayland.h>

#elif WTK_NATIVE_X11

#include <gdk/gdkx.h>


#endif
#ifndef OMEGAWTK_NATIVE_GTK_GTKITEM_H
#define OMEGAWTK_NATIVE_GTK_GTKITEM_H

namespace OmegaWTK::Native::GTK {
    class GTKItem : public NativeItem {

        GdkWindow *window;
        GdkWindowClass * wnd_class;

        OmegaCommon::Vector<SharedHandle<GTKItem>> childWindows;
        Core::Rect rect;
        public:
        void enable() override;
        void disable() override;
        void resize(const Core::Rect &newRect) override;
        void * getBinding() override;
        void addChildNativeItem(SharedHandle<NativeItem> nativeItem) override;
        void removeChildNativeItem(SharedHandle<NativeItem> nativeItem) override;
        void setClippedView(SharedHandle<NativeItem> clippedView) override;
        void toggleHorizontalScrollBar(bool &state) override;
        void toggleVerticalScrollBar(bool &state) override;
        Core::Rect & getRect() override {
            return rect;
        };
        GTKItem(Core::Rect rect,Native::ItemType type,NativeItemPtr parent);
        #if WTK_NATIVE_WAYLAND
        wl_surface * getSurface();
        #elif WTK_NATIVE_X11
        Display * getDisplay();
        Window getX11Window();
        #endif
    };
};

#endif
