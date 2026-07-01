#ifndef OMEGAWTK_NATIVE_GTK_WAYLANDSURFACEHOST_H
#define OMEGAWTK_NATIVE_GTK_WAYLANDSURFACEHOST_H

// Geometry.h is the smallest OmegaWTK header that defines
// Composition::Rect — the same tight include surface X11SurfaceHost.h
// keeps. The Wayland protocol headers do not #define None/True/False the
// way Xlib.h does, so there is no macro-clobber hazard here; we keep the
// surface minimal anyway so this header re-includes cleanly alongside the
// other private GTK headers.
#include "omegaWTK/Composition/Geometry.h"
#include "NativePrivate/gtk/SurfaceHost.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#if WTK_NATIVE_WAYLAND
#include <wayland-client.h>
#endif

/// Single per-window owner of every native Wayland surface beneath the GTK
/// toplevel — the §2.13a sibling of `X11SurfaceHost`, built to the same
/// `SurfaceHost` contract so `GTKAppWindow` can hold either behind one
/// `std::unique_ptr<SurfaceHost>` and the realize handler can drive whichever
/// the detected backend selected.
///
/// `WaylandSurfaceHost` mirrors `X11SurfaceHost`'s two responsibilities:
///
/// 1. Track the toplevel `wl_surface` the GtkWindow is bound to. The surface
///    handle is only valid after the GtkWindow has been realized, so
///    `runOnRealize` defers any work that needs it until `onToplevelRealized`
///    fires (which `GTKAppWindow`'s realize handler will call in W5).
///    Callbacks queued post-realize fire immediately on the calling thread —
///    identical semantics to `X11SurfaceHost`.
/// 2. Allocate, reconfigure, and destroy child surfaces under the toplevel.
///    Wayland has no child windows; the analog is `wl_subsurface` — a
///    `wl_surface` from `wl_compositor.create_surface`, parented with
///    `wl_subcompositor.get_subsurface`, positioned parent-relative with
///    `wl_subsurface.set_position`, and stacked with `place_above`/
///    `place_below`. The subsurfaces run in desync mode so a content node can
///    commit independently of the toplevel. The registry *shape* —
///    `hostId → platform handle`, reconfigure-on-layout, destroy-on-teardown —
///    is identical to `X11SurfaceHost`, which is why the `SurfaceHost`
///    interface fits both.
///
/// **Pass-1 reality check (§2.13a).** §2.14 shipped root-only; child content
/// nodes are Pass 2 (`NativeViewHost-Adoption-Plan.md` V2/G2). So the
/// subsurface registry below is *present-but-unused* in the first cut —
/// exactly the staging `X11SurfaceHost`'s child-window registry already
/// follows. Pass 1 renders the root Vulkan swap chain directly into GDK's
/// toplevel `wl_surface`; the subsurface machinery is written to the
/// interface but not exercised until the same consumer that lights up the X11
/// child path lights up the Wayland one. The commit-cadence coordination
/// between a desync subsurface's first map and the parent commit is a Pass-2
/// concern (§2.13a risk 2) and is deferred with the rest of the child path.
///
/// On non-Wayland builds (`WTK_NATIVE_WAYLAND` undefined), every method is a
/// no-op, the same way `X11SurfaceHost` no-ops on Wayland-only builds.
namespace OmegaWTK::Native::GTK {

class WaylandSurfaceHost : public SurfaceHost {
public:
#if WTK_NATIVE_WAYLAND
    /// `dpy` is GDK's `wl_display` (`gdk_wayland_display_get_wl_display`).
    /// `comp` / `subcomp` are the `wl_compositor` / `wl_subcompositor`
    /// globals used to allocate child subsurfaces; they may be null in a
    /// root-only (Pass-1) configuration, in which case the child API no-ops.
    explicit WaylandSurfaceHost(wl_display *dpy,
                                wl_compositor *comp = nullptr,
                                wl_subcompositor *subcomp = nullptr);
#else
    WaylandSurfaceHost();
#endif

    WaylandSurfaceHost(const WaylandSurfaceHost &) = delete;
    WaylandSurfaceHost &operator=(const WaylandSurfaceHost &) = delete;
    WaylandSurfaceHost(WaylandSurfaceHost &&) = delete;
    WaylandSurfaceHost &operator=(WaylandSurfaceHost &&) = delete;

    ~WaylandSurfaceHost();

#if WTK_NATIVE_WAYLAND
    /// Called by `GTKAppWindow` (W5) from its "realize" signal handler with
    /// the toplevel's GDK-owned `wl_surface` (`gdk_wayland_window_get_wl_surface`).
    /// Caches the surface and drains any callbacks queued via `runOnRealize`.
    /// Idempotent — re-realize cycles re-cache the surface but do NOT re-fire
    /// queued callbacks; those drain once on the first realize and clear.
    /// Mirrors `X11SurfaceHost::onToplevelRealized`.
    void onToplevelRealized(wl_surface *toplevelSurface);

    wl_display *display() const { return dpy_; }
    wl_surface *toplevel() const { return toplevel_; }

    /// Allocate a child subsurface under the toplevel at `rect`. Returns
    /// `nullptr` if the toplevel is not yet realized or no
    /// `wl_compositor`/`wl_subcompositor` was supplied — callers that need
    /// pre-realize semantics should queue work through `runOnRealize`.
    /// The subsurface is placed in desync mode so it can commit independently
    /// of the parent. Returns the child `wl_surface` as the registry handle
    /// (the analog of `X11SurfaceHost::createChildWindow`'s `::Window`).
    wl_surface *createChildSurface(const Composition::Rect &rect);

    /// Release a child surface previously returned by `createChildSurface`.
    /// Safe to call after toplevel destruction (no-op once cleared).
    void destroyChildSurface(wl_surface *child);

    /// Move and re-stack a child subsurface. `zOrder` is an opaque integer —
    /// larger values stack above smaller ones, matching DComp / CALayer
    /// `zPosition` and `X11SurfaceHost`'s `XRestackWindows` semantics. The
    /// host caches the latest z-order per child and re-runs the parent-
    /// relative `place_above` chain against the sorted set on every
    /// reconfigure.
    void reconfigureChildSurface(wl_surface *child,
                                 const Composition::Rect &rect,
                                 int zOrder);
#endif

    /// True once `onToplevelRealized` has been called at least once.
    bool isRealized() const override;

    /// Defer a callback until the toplevel is realized. If already realized,
    /// runs synchronously on the calling thread. Otherwise the callback is
    /// moved into a queue drained by the next `onToplevelRealized` call. Empty
    /// `action` is a no-op. Identical contract to `X11SurfaceHost`.
    void runOnRealize(std::function<void()> action) override;

    /// SurfaceHost `hostId`-keyed child API. Thin forwarders over the
    /// surface-keyed child registry above: `reconfigureChild` allocates the
    /// child subsurface on first call for a `hostId` (via `createChildSurface`)
    /// and caches the mapping, then forwards to `reconfigureChildSurface`;
    /// `destroyChild` forwards to `destroyChildSurface` and drops the mapping.
    /// On a non-Wayland build both are no-ops (the whole host is). Present-but-
    /// unused until §2.14 Pass 2, matching `X11SurfaceHost`.
    void reconfigureChild(std::uint64_t hostId,
                          const Composition::Rect &rectPx,
                          int zOrder) override;
    void destroyChild(std::uint64_t hostId) override;

private:
#if WTK_NATIVE_WAYLAND
    wl_display       *dpy_ = nullptr;
    wl_compositor    *comp_ = nullptr;     // for wl_compositor.create_surface
    wl_subcompositor *subcomp_ = nullptr;  // for child wl_subsurfaces
    wl_surface       *toplevel_ = nullptr;
    struct ChildSurface {
        wl_surface    *surface;
        wl_subsurface *subsurface;
        int            zOrder;
    };
    std::vector<ChildSurface> childOrder_;
    /// hostId → child `wl_surface*`, populated lazily by `reconfigureChild`.
    /// Translates the SurfaceHost `hostId`-keyed API onto the surface-keyed
    /// child registry without changing the Pass-2 child logic.
    std::unordered_map<std::uint64_t, wl_surface *> hostChildren_;
    void restackChildren();
#endif

    mutable std::mutex realizeMutex_;
    std::vector<std::function<void()>> pendingRealize_;
    bool realized_ = false;
};

}

#endif
