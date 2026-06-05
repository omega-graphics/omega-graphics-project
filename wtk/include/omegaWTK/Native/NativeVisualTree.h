#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Geometry.h"
#include "NativeItem.h"

#include <functional>

#ifndef OMEGAWTK_NATIVE_NATIVEVISUALTREE_H
#define OMEGAWTK_NATIVE_NATIVEVISUALTREE_H

namespace OmegaWTK::Native {

    /// One node in the per-window compositor visual tree (§2.14).
    ///
    /// `Visual` is the cross-platform abstraction over the platform's
    /// layer / visual / window primitive — `CALayer` on macOS,
    /// `IDCompositionVisual2` on Windows, X11 `Window` on Linux. The
    /// abstract base intentionally exposes no platform handle; consumers
    /// that need one (the Composition binder, NativeViewHost) include the
    /// per-backend subclass header (`Native/macos/MTLVisualTree.h` etc.)
    /// and downcast.
    ///
    /// The Composition layer holds its `BackendRenderTargetContext`s in
    /// a side map keyed by `const Visual *`, populated by the per-backend
    /// VisualBinder at tree attach time. `Visual` therefore owns nothing
    /// Composition-side — moving it into Native does not invert the
    /// layering. See the §2.14 "Visual / BackendRenderTargetContext
    /// decoupling" note in `Native-API-Completion-Proposal.md`.
    INTERFACE OMEGAWTK_EXPORT Visual {
    protected:
        /// Geometry in pixels (post-scale). The binder reads this when
        /// building the backing render target so the backing dimensions
        /// match the visual's actual surface area.
        Composition::Rect rectPixels;

        /// Render-order hint applied by `reconfigureContentNode` in §2.14
        /// Pass 2 (NativeViewHost adoption). Plain integer here so the
        /// base type carries the field without depending on any per-
        /// backend ordering primitive; backends translate it to their
        /// native equivalent (`CALayer.zPosition`, DComp child order,
        /// X11 `XRestackWindows`).
        int zOrderHint = 0;

    private:
        /// Composition-installed hook that forwards a Visual::resize
        /// into the BackendRenderTargetContext kept in the compositor's
        /// `Visual* → RTC` side map. One hook per Visual; empty
        /// pre-attach / post-detach. Touched only via `setOnResize`.
        std::function<void(const Composition::Rect &)> onResize_;

    public:
        Visual();
        explicit Visual(Composition::Rect rect, int zOrderHint = 0);

        Composition::Rect rect() const;
        int zOrder() const;
        void setZOrder(int z);

        /// Resize the visual. Default base impl updates `rectPixels`
        /// and triggers `onResize` so the Composition binder can
        /// re-run `BackendRenderTargetContext::setRenderTargetSize`
        /// for the next frame. Backends may override to additionally
        /// resize a platform handle, though none do today — the
        /// per-window resize already updates the native present layer
        /// through `NativeItem::resizeNativeLayer`.
        virtual void resize(const Composition::Rect & newRect);

        /// Install the Composition-side resize hook. Called once at
        /// attach time by the per-backend VisualBinder; replaces any
        /// previous hook. Pass an empty `std::function` to detach.
        void setOnResize(std::function<void(const Composition::Rect &)> cb);

        virtual ~Visual() = default;
    };

    /// Per-window compositor visual tree owned by `AppWindow`.
    ///
    /// One per `AppWindow` — construction order in the AppWindow ctor
    /// is `NativeWindow` → (Linux) `X11SurfaceHost` → `Native::VisualTree`,
    /// destruction is the reverse. The Composition layer holds a
    /// non-owning `Native::VisualTree *` and a `Visual* → RTC` side map
    /// keyed off this tree's visuals; that side map is drained in the
    /// compositor's per-window teardown before AppWindow releases the
    /// tree.
    ///
    /// All mutations happen on the main thread — the same rule that
    /// already governs DComp `Commit`, CALayer updates inside
    /// `CATransaction`, and X11 `XMoveResizeWindow`. The compositor's
    /// frame worker reads only through its RTC side map and never
    /// touches the tree's structure; no extra locking required.
    INTERFACE OMEGAWTK_EXPORT VisualTree {
    public:
        /// The single root visual hosting the window's WTK present
        /// surface (swap chain / CAMetalLayer). The Composition binder
        /// wraps this in a `BackendRenderTargetContext` at attach time.
        /// Lifetime is tied to the tree; the pointer stays valid until
        /// `~VisualTree` runs.
        INTERFACE_METHOD Visual * rootVisual() const ABSTRACT;

        /// Resize the root visual to `newRect`. The binder picks up the
        /// new geometry on the next frame via `rootVisual()->rect()`.
        /// Convenience method — equivalent to
        /// `rootVisual()->resize(newRect)` with a null guard.
        void resize(const Composition::Rect & newRect);

        virtual ~VisualTree() = default;
    };
    typedef SharedHandle<VisualTree> NativeVisualTreePtr;

    /// Construct the per-window visual tree.
    ///
    /// `rootItem` is the window's root `NativeItem` (the same one
    /// `NativeWindow::getRootView()` returns). The tree wraps its
    /// platform handle as the root visual — `CocoaItem`'s NSView for
    /// macOS, `HWNDItem`'s HWND for Windows, `GTKItem`'s GtkWidget /
    /// X11 Window for Linux.
    ///
    /// `rect` is in DIPs (post-scale logical pixels). `scale` is the
    /// window's combined logical→physical scale at construction time
    /// (matches `NativeWindow::scaleFactor()` / `currentScreen().scaleFactor`).
    /// The tree pre-multiplies these for the platform handle's backing
    /// dimensions so the first frame lands at the right density.
    ///
    /// One factory per platform: `Native::make_native_visual_tree` is
    /// defined in `wtk/src/Native/macos/MTLVisualTree.mm` /
    /// `wtk/src/Native/win/DCVisualTree.cpp` /
    /// `wtk/src/Native/gtk/VKVisualTree.cpp`. Only one is linked into
    /// any single build.
    OMEGAWTK_EXPORT NativeVisualTreePtr make_native_visual_tree(
        NativeItemPtr rootItem,
        const Composition::Rect & rect,
        float scale);

}

#endif
