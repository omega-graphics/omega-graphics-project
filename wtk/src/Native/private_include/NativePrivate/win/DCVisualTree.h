// §2.14 Pass 1 (Win32) — Native side. Owns the DirectComposition
// topology: process-global IDCompositionDesktopDevice, per-window
// IDCompositionTarget bound to the HWND, root IDCompositionVisual2.
// The IDXGISwapChain that backs the WTK present surface is created
// in the Composition layer (see `backend/dx/DCVisualBinder.cpp`) and
// handed back to `DCVisual::bindSwapChain` here — that's the boundary
// the strict §2.14 layering draws: DComp visuals are Native (they're
// platform layer topology); the swap chain is Composition (it's a
// GTE-owned GPU render target).
//
// COMPILE-UNVERIFIED off-platform. The Linux (and macOS) migrations
// on the same commit are pattern-matched against; drift is the first
// thing to audit if the Windows build lands red.

#ifndef OMEGAWTK_NATIVEPRIVATE_WIN_DCVISUALTREE_H
#define OMEGAWTK_NATIVEPRIVATE_WIN_DCVISUALTREE_H

#include "omegaWTK/Native/NativeVisualTree.h"
#include "omegaWTK/Core/Microsoft.h"
#include "NativePrivate/win/HWNDItem.h"

#include <dcomp.h>

namespace OmegaWTK::Native::Win {

    /// Win32 Visual subclass. Owns one `IDCompositionVisual2` plus a
    /// strong reference to its host `HWNDItem`. The Composition binder
    /// (`DCVisualBinder`) constructs the IDXGISwapChain backing this
    /// visual and hands it back via `bindSwapChain`, which runs the
    /// DComp wiring sequence: `SetContent` → `SetOpacityMode` →
    /// `hwndTarget->SetRoot` → device `Commit` +
    /// `WaitForCommitCompletion`.
    class DCVisual : public Native::Visual {
    public:
        DCVisual(SharedHandle<HWNDItem> item,
                  IDCompositionVisual2 *visual,
                  Composition::Rect rect);
        ~DCVisual() override;

        /// Raw DComp visual pointer for the binder / NativeViewHost
        /// Pass 2 sublayer ordering. Lifetime tied to the DCVisual;
        /// the binder must not retain past its own teardown.
        IDCompositionVisual2 * directCompositionVisual() const { return visual_; }

        HWND hwnd() const;

        /// Bind the swap chain to the DComp visual and root the
        /// visual on the per-window hwnd target. Called once by the
        /// Composition binder after constructing the
        /// `GENativeRenderTarget`. The swap chain pointer is held by
        /// the GTE render target (not retained here); the DComp
        /// visual's `SetContent` adds its own ref.
        void bindSwapChain(IUnknown *swapChain,
                           IDCompositionTarget *hwndTarget,
                           IDCompositionDesktopDevice *desktopDevice);

        /// The owning HWNDItem — held so the binder (or Pass 2
        /// NativeViewHost work) can re-fetch the HWND without
        /// reaching back through AppWindow.
        const SharedHandle<HWNDItem> & hwndItem() const { return item_; }

    private:
        SharedHandle<HWNDItem> item_;
        IDCompositionVisual2 *visual_;  // strong ref via CreateVisual
    };

    /// Win32 VisualTree. Holds the single root DCVisual, the per-
    /// window `IDCompositionTarget` bound to the HWND, and the
    /// combined logical→physical scale captured at construction time.
    /// Process-global `IDCompositionDevice3` / `IDCompositionDesktopDevice`
    /// are created lazily on the first tree (matches the pre-§2.14
    /// behaviour); they outlive any individual tree.
    class DCVisualTree : public Native::VisualTree {
    public:
        DCVisualTree(SharedHandle<HWNDItem> rootItem,
                      Composition::Rect rect,
                      float scale);
        ~DCVisualTree() override;

        Native::Visual * rootVisual() const override;

        float scale() const { return scale_; }

        /// Per-window hwnd target — the Composition binder needs it
        /// to forward into `DCVisual::bindSwapChain`. Owned by the
        /// tree; lifetime tied to it.
        IDCompositionTarget * hwndTarget() const { return hwndTarget_.comPtr.Get(); }

        /// Process-global desktop device. Same lifetime / Commit
        /// channel for every tree in the process.
        static IDCompositionDesktopDevice * desktopDevice();

    private:
        SharedHandle<DCVisual> rootVisual_;
        Core::UniqueComPtr<IDCompositionTarget> hwndTarget_;
        float scale_ = 1.f;
    };

}

#endif
