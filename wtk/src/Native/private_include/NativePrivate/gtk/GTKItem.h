#include "omegaWTK/Native/NativeItem.h"

#include <gdk/gdk.h>
#include <memory>

// Independent #ifs (not #if/#elif): the §2.13a BOTH co-build defines both
// WTK_NATIVE_WAYLAND and WTK_NATIVE_X11, and needs both protocol headers and
// both accessor sets present at once. Single-protocol builds still compile
// exactly one of each.
#if WTK_NATIVE_WAYLAND

#include <gdk/wayland/gdkwayland.h>

#endif
#if WTK_NATIVE_X11

#include <gdk/x11/gdkx.h>

#endif
#ifndef OMEGAWTK_NATIVE_GTK_GTKITEM_H
#define OMEGAWTK_NATIVE_GTK_GTKITEM_H

namespace OmegaWTK::Native::GTK {
    /// Under the virtual view model (see "Architecture note" at the top
    /// of `wtk/.plans/Native-API-Completion-Proposal.md`), there is
    /// exactly one NativeItem per window: the root, owned by
    /// `GTKAppWindow`. §2.15 (Option B′, GTK 3 retired) binds that root to
    /// the bare toplevel `GdkSurface` `GTKAppWindow` falls through to — no
    /// `GtkWindow`, no `GtkWidget`, no `GskRenderer`. No event-handler
    /// installation happens here either: input arrives on the surface's
    /// `event` signal, handled in `GTKAppWindow`. All this class does is:
    ///
    /// - Expose the toplevel's protocol present handle — X11 `Window` /
    ///   `Display` or Wayland `wl_surface` — to the §2.14 visual tree and
    ///   the compositor's surface-descriptor builder (`getBinding`).
    /// - Track the latest known rect for the root content area.
    ///
    /// The legacy (rect, ItemType) constructor and the
    /// `addChildNativeItem` / `setClippedView` / scroll plumbing remain
    /// for source-compatibility with the cross-platform
    /// `make_native_item` factory, but are unreachable under the virtual
    /// view tree — no caller asks GTK for a non-root NativeItem anymore.
    class GTKItem : public NativeItem {

        /// Option B′ (§2.15): the root item borrows the bare toplevel
        /// GdkSurface that GTKAppWindow falls through to — there is no
        /// GtkWidget and no GskRenderer. Child NativeItems are NOT widgets
        /// either; they are subsurfaces owned by the VisualTree via the
        /// §2.13a SurfaceHost child registry, so this class carries no child
        /// widget machinery. The surface is NOT owned here (GTKAppWindow
        /// manages its lifetime).
        GdkSurface *surface = nullptr;
        bool surfaceBorrowed = true;
        Composition::Rect rect;
        bool isVisible = true;
    public:
        /// The bare toplevel GdkSurface this root item wraps (null for the
        /// legacy stub item). Used by the VisualTree to resolve the
        /// GdkDisplay and the protocol present handle.
        GdkSurface *getSurface();
        void emitIfPossible(NativeEventPtr event);
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

        /// Root-item constructor (Option B′): binds the bare toplevel
        /// GdkSurface owned by GTKAppWindow. The surface is NOT owned here;
        /// input is delivered through GTKAppWindow's GdkSurface "event"
        /// signal, not attached here.
        GTKItem(Composition::Rect rect,GdkSurface *toplevel);

        // Protocol-explicit accessors, each behind its own #if so the BOTH
        // co-build carries both sets (the old `getDisplay()` collided —
        // `wl_display*` on Wayland vs `Display*` on X11 — see §2.13a blocker 2).
        // `getBinding()` picks the live one at runtime via detectWindowingBackend.
        #if WTK_NATIVE_WAYLAND
        wl_surface * getWaylandSurface();
        wl_display * getWaylandDisplay();
        #endif
        #if WTK_NATIVE_X11
        Display * getX11Display();
        Window getX11Window();
        #endif
        ~GTKItem();
    };
};

#endif
