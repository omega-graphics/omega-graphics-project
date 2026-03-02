#include "omegaWTK/Native/NativeItem.h"

#include <gtk/gtk.h>
#include <memory>

#if WTK_NATIVE_WAYLAND

#include <gdk/gdkwayland.h>

#elif WTK_NATIVE_X11

#include <gdk/gdkx.h>


#endif
#ifndef OMEGAWTK_NATIVE_GTK_GTKITEM_H
#define OMEGAWTK_NATIVE_GTK_GTKITEM_H

namespace OmegaWTK::Native::GTK {
    class GTKItem : public NativeItem {

        GtkWidget *widget = nullptr;
        GtkWidget *contentWidget = nullptr;
        Core::Rect rect;
        bool isVisible = true;
        bool isScrollItem = false;
        bool horizontalScrollEnabled = false;
        bool verticalScrollEnabled = false;
        OmegaCommon::Vector<SharedHandle<GTKItem>> childItems;
        SharedHandle<GTKItem> clippedView = nullptr;

        GdkWindow *resolveGdkWindow();
        void moveInParent();
        void updateWidgetSize();
        void applyScrollPolicy();
    public:
        GtkWidget *getWidget();
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
        GTKItem(Core::Rect rect,Native::ItemType type);
        #if WTK_NATIVE_WAYLAND
        wl_surface * getSurface();
        wl_display * getDisplay();
        #elif WTK_NATIVE_X11
        Display * getDisplay();
        Window getX11Window();
        #endif
        ~GTKItem();
    };
};

#endif
