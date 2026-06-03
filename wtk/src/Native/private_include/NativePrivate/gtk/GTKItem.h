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
    /// Under the virtual view model (see "Architecture note" at the top
    /// of `wtk/.plans/Native-API-Completion-Proposal.md`), there is
    /// exactly one NativeItem per window: the root, owned by
    /// `GTKAppWindow`. §2.13 collapses that root into a thin handle
    /// around the GtkWindow's own toplevel widget — no `GtkDrawingArea`,
    /// no extra GdkWindow, no event-handler installation here (input
    /// signals attach to the GtkWindow in `GTKAppWindow`). All this
    /// class still does is:
    ///
    /// - Resolve the toplevel's GdkWindow on demand (`resolveGdkWindow`).
    /// - Expose the toplevel X11 Display + Window (or Wayland surface)
    ///   to the compositor's surface-descriptor builder.
    /// - Track the latest known rect for the root content area.
    ///
    /// The legacy (rect, ItemType) constructor and the
    /// `addChildNativeItem` / `setClippedView` / scroll plumbing remain
    /// for source-compatibility with the cross-platform
    /// `make_native_item` factory, but are unreachable under the virtual
    /// view tree — no caller asks GTK for a non-root NativeItem anymore.
    class GTKItem : public NativeItem {

        GtkWidget *widget = nullptr;
        GtkWidget *renderWidget = nullptr;
        GtkWidget *contentWidget = nullptr;
        /// True when the underlying `widget` is borrowed from
        /// GTKAppWindow (the toplevel GtkWindow), not owned by this
        /// GTKItem. Borrowed widgets are NOT destroyed in the dtor.
        bool widgetBorrowed = false;
        Composition::Rect rect;
        bool isVisible = true;
        bool isScrollItem = false;
        bool horizontalScrollEnabled = false;
        bool verticalScrollEnabled = false;
        double lastHorizontalScrollValue = 0.0;
        double lastVerticalScrollValue = 0.0;
        OmegaCommon::Vector<SharedHandle<GTKItem>> childItems;
        SharedHandle<GTKItem> clippedView = nullptr;

        GdkWindow *resolveGdkWindow();
        void moveInParent();
        void updateWidgetSize();
        void applyScrollPolicy();
    public:
        GtkWidget *getWidget();
        void emitIfPossible(NativeEventPtr event);
        void handleAllocation(const GtkAllocation &allocation);
        void handleScrollAdjustmentValue(double value,bool horizontal);
        void enable() override;
        void disable() override;
        void resize(const Composition::Rect &newRect) override;
        void * getBinding() override;
        void addChildNativeItem(SharedHandle<NativeItem> nativeItem) override;
        void removeChildNativeItem(SharedHandle<NativeItem> nativeItem) override;
        void setClippedView(SharedHandle<NativeItem> clippedView) override;
        void toggleHorizontalScrollBar(bool &state) override;
        void toggleVerticalScrollBar(bool &state) override;
        Composition::Rect & getRect() override {
            return rect;
        };
        /// Legacy constructor — kept for source-compatibility with the
        /// cross-platform `make_native_item` factory. Under §2.13 this
        /// path is no longer used by GTKAppWindow (which calls the
        /// toplevel-binding ctor below) and no other caller exists
        /// because the virtual view tree never asks the backend for a
        /// non-root NativeItem.
        GTKItem(Composition::Rect rect,Native::ItemType type);

        /// §2.13 root-item constructor: binds to an existing toplevel
        /// GtkWidget instead of allocating a `GtkDrawingArea`. The
        /// `toplevel` widget is NOT owned by this GTKItem — its
        /// lifetime is managed by GTKAppWindow. Input signals are NOT
        /// attached here either; they attach to the GtkWindow directly
        /// in GTKAppWindow's ctor.
        GTKItem(Composition::Rect rect,GtkWidget *toplevel);

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
