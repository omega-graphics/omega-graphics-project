#ifndef OMEGAWTK_NATIVE_GTK_SURFACEHOST_H
#define OMEGAWTK_NATIVE_GTK_SURFACEHOST_H

// Geometry.h is the smallest OmegaWTK header that defines Composition::Rect;
// see the include-hygiene note in X11SurfaceHost.h for why we keep this
// surface tight (X11/Xlib.h #defines None/True/False, which clobber enum
// members in heavier OmegaWTK headers). GTKWindowing.h is the canonical home
// of WindowingBackend, which this interface stores by value.
#include "omegaWTK/Composition/Geometry.h"
#include "NativePrivate/gtk/GTKWindowing.h"

#include <cstdint>
#include <functional>

namespace OmegaWTK::Native::GTK {

/// Per-window owner of every native child surface beneath the GTK toplevel,
/// abstracted over the windowing protocol. §2.13's `X11SurfaceHost` and
/// §2.13a's `WaylandSurfaceHost` both implement this so `GTKAppWindow` can
/// hold either behind one `std::unique_ptr<SurfaceHost>` and the realize
/// handler can drive whichever the detected backend selected.
///
/// The realize gate (`isRealized` / `runOnRealize`) carries the identical
/// contract `X11SurfaceHost` shipped under §2.13. The `hostId`-keyed child
/// API is the Pass-2 (NativeViewHost adoption) surface — present on the
/// interface now, lit up by §2.14 Pass 2; the platform child handle
/// (`::Window` / `wl_subsurface*`) stays inside each impl, keyed by the
/// `NativeContentNode`'s `hostId`.
class SurfaceHost {
public:
    virtual ~SurfaceHost() = default;

    /// Realize gate — identical contract to `X11SurfaceHost`'s §2.13 behavior.
    /// `isRealized()` is true once the toplevel handle is known; `runOnRealize`
    /// defers an action until then (or runs it synchronously if already
    /// realized).
    virtual bool isRealized() const = 0;
    virtual void runOnRealize(std::function<void()> action) = 0;

    /// Pass-2 child-surface API. `hostId` is the `NativeContentNode` key.
    /// `reconfigureChild` lazily allocates the child surface on first call for
    /// a given `hostId`, then positions it (`rectPx`, parent-relative pixels)
    /// and z-stacks it (`zOrder` — larger stacks above, matching DComp /
    /// CALayer `zPosition`). `destroyChild` releases the surface for `hostId`.
    virtual void reconfigureChild(std::uint64_t hostId,
                                  const Composition::Rect &rectPx,
                                  int zOrder) = 0;
    virtual void destroyChild(std::uint64_t hostId) = 0;

    /// The protocol this host owns surfaces for. Set once at construction.
    WindowingBackend backend() const { return backend_; }

protected:
    explicit SurfaceHost(WindowingBackend b) : backend_(b) {}
    WindowingBackend backend_;
};

}

#endif
