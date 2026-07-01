#ifndef OMEGAWTK_NATIVE_GTK_X11SURFACEHOST_H
#define OMEGAWTK_NATIVE_GTK_X11SURFACEHOST_H

// Geometry.h is the smallest OmegaWTK header that defines
// Composition::Rect. Pulling Layer.h or NativeWindow.h here would drag
// in Brush.h / NativeMenu.h, which both declare enum members named
// `None`, `True`, `False` — names X11/Xlib.h clobbers with #define.
// Keeping the include surface tight lets X11SurfaceHost.h be safely
// re-included alongside any other private GTK header.
#include "omegaWTK/Composition/Geometry.h"
#include "NativePrivate/gtk/SurfaceHost.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if WTK_NATIVE_X11
#include <X11/Xlib.h>
#endif

/// Single per-window owner of every native X11 surface beneath the GTK
/// toplevel. Lives under Native (not Composition) because surface ownership
/// is a Native concern under the virtual view model — the per-View
/// NativeItem is gone, so child surfaces no longer hide inside GTKItem.
///
/// `X11SurfaceHost` is constructed by `GTKAppWindow` in its ctor (display
/// handle in hand) and outlives every consumer that allocates from it
/// (the visual tree, NativeContentNodes). Two responsibilities:
///
/// 1. Track the toplevel X11 Window the GtkWindow is bound to. The Window
///    XID is only valid after the GtkWindow has been realized, so
///    `runOnRealize` defers any work that needs the XID until
///    `onToplevelRealized` fires (which `GTKAppWindow` calls from its
///    "realize" signal handler). Callbacks queued post-realize fire
///    immediately on the calling thread.
/// 2. Allocate, reconfigure, and destroy child X11 Windows under the
///    toplevel. These are the surfaces that §2.14 NativeVisualTree
///    (Linux branch) and NativeViewHost-Adoption-Plan use for embedded
///    GPU / video surfaces. §2.13 ships the API; §2.14 lights up the
///    callers.
///
/// On Wayland builds (`WTK_NATIVE_WAYLAND` defined, `WTK_NATIVE_X11`
/// undefined), every method is a no-op. §2.13 commits to X11 only — see
/// the "X11-only" subsection of the Native API Completion Proposal.
namespace OmegaWTK::Native::GTK {

class X11SurfaceHost : public SurfaceHost {
public:
#if WTK_NATIVE_X11
    explicit X11SurfaceHost(Display *dpy);
#else
    X11SurfaceHost();
#endif

    X11SurfaceHost(const X11SurfaceHost &) = delete;
    X11SurfaceHost &operator=(const X11SurfaceHost &) = delete;
    X11SurfaceHost(X11SurfaceHost &&) = delete;
    X11SurfaceHost &operator=(X11SurfaceHost &&) = delete;

    ~X11SurfaceHost();

#if WTK_NATIVE_X11
    /// Called by `GTKAppWindow` from its "realize" signal handler. Caches
    /// the toplevel XID and drains any callbacks queued via
    /// `runOnRealize`. Idempotent — re-realize cycles re-cache the XID
    /// (it may have changed) but do NOT re-fire queued callbacks; those
    /// drain once on the first realize and clear.
    void onToplevelRealized(::Window toplevel);

    Display *display() const { return dpy_; }
    ::Window toplevel() const { return toplevel_; }

    /// Allocate a child X11 Window under the toplevel at `rect`. Returns
    /// `0` if the toplevel is not yet realized — callers that need
    /// pre-realize semantics should queue work through `runOnRealize`.
    ::Window createChildWindow(const Composition::Rect &rect);

    /// Release a child Window previously returned by `createChildWindow`.
    /// Safe to call after toplevel destruction (no-op once cleared).
    void destroyChildWindow(::Window child);

    /// Move and re-stack a child Window. `zOrder` is an opaque integer —
    /// larger values stack above smaller ones, matching DComp / CALayer
    /// `zPosition` semantics. The host caches the latest z-order per
    /// child and re-runs `XRestackWindows` against the sorted set on
    /// every reconfigure (cheap; typical content-node count is single
    /// digits).
    void reconfigureChildWindow(::Window child,
                                 const Composition::Rect &rect,
                                 int zOrder);
#endif

    /// True once `onToplevelRealized` has been called at least once.
    bool isRealized() const override;

    /// Defer a callback until the toplevel is realized. If already
    /// realized, runs synchronously on the calling thread. Otherwise
    /// the callback is moved into a queue drained by the next
    /// `onToplevelRealized` call. Empty `action` is a no-op.
    void runOnRealize(std::function<void()> action) override;

    /// SurfaceHost `hostId`-keyed child API. Thin forwarders over the
    /// XID-keyed child registry above: `reconfigureChild` allocates the
    /// child `::Window` on first call for a `hostId` (via `createChildWindow`)
    /// and caches the mapping, then forwards to `reconfigureChildWindow`;
    /// `destroyChild` forwards to `destroyChildWindow` and drops the mapping.
    /// On a non-X11 build both are no-ops (the whole host is). Present-but-
    /// unused until §2.14 Pass 2, matching the child-window API itself.
    void reconfigureChild(std::uint64_t hostId,
                          const Composition::Rect &rectPx,
                          int zOrder) override;
    void destroyChild(std::uint64_t hostId) override;

private:
#if WTK_NATIVE_X11
    Display *dpy_ = nullptr;
    ::Window toplevel_ = 0;
    std::unordered_set<::Window> ownedChildren_;
    struct ChildState {
        ::Window window;
        int zOrder;
    };
    std::vector<ChildState> childOrder_;
    /// hostId → child `::Window`, populated lazily by `reconfigureChild`.
    /// Translates the SurfaceHost `hostId`-keyed API onto the XID-keyed
    /// child registry without touching the §2.13-verified child logic.
    std::unordered_map<std::uint64_t, ::Window> hostChildren_;
    void restackChildren();
#endif

    mutable std::mutex realizeMutex_;
    std::vector<std::function<void()>> pendingRealize_;
    bool realized_ = false;
};

}

#endif
