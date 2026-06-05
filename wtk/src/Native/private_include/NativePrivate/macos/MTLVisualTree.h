// §2.14 Pass 1 (macOS) — Native side of the per-window compositor
// visual tree. Mirrors the Linux split: the Native layer owns the
// platform present surface (`CAMetalLayer` here, just as
// `Native::GTK::VKVisual` owns the X11/Wayland window handles); the
// Composition layer builds the `GENativeRenderTarget` +
// `BackendRenderTargetContext` from those handles via the per-backend
// VisualBinder.
//
// COMPILE-UNVERIFIED off-platform — this file ships on the same
// commit as the Linux migration that landed first. The macOS build is
// expected to be exercised on hardware before any release.

#ifndef OMEGAWTK_NATIVEPRIVATE_MACOS_MTLVISUALTREE_H
#define OMEGAWTK_NATIVEPRIVATE_MACOS_MTLVISUALTREE_H

#include "omegaWTK/Native/NativeVisualTree.h"
#include "NativePrivate/macos/CocoaItem.h"

#import <QuartzCore/QuartzCore.h>

namespace OmegaWTK::Native::Cocoa {

    /// macOS Visual subclass. Owns the `CAMetalLayer` that backs the
    /// window's WTK swap chain — the Composition binder
    /// (`MTLVisualBinder`) reads the layer pointer here to construct
    /// the `GENativeRenderTarget`. The layer is retained for the
    /// lifetime of the MTLVisual and released in the destructor.
    ///
    /// Construction-time layer configuration (autoresize mask,
    /// `contentsScale`, `drawableSize`, the BGRA8Unorm pixel format
    /// the WTK pipelines expect) is run by `CocoaItem::setRootLayer`
    /// when the tree attaches the layer to the host NSView — the
    /// same code path that pre-§2.14 ran via `MTLCALayerTree::
    /// setRootVisual`.
    class MTLVisual : public Native::Visual {
    public:
        MTLVisual(SharedHandle<CocoaItem> item,
                   Composition::Rect rect,
                   CAMetalLayer *layer);
        ~MTLVisual() override;

        /// CAMetalLayer pointer for the Composition binder. Lifetime
        /// tied to the MTLVisual; the binder must not retain past its
        /// own teardown.
        CAMetalLayer * metalLayer() const { return metalLayer_; }

        /// The owning CocoaItem — held so the binder (or §2.14 Pass 2
        /// NativeViewHost work) can re-fetch the host view without
        /// reaching back through the AppWindow surface.
        const SharedHandle<CocoaItem> & cocoaItem() const { return item_; }

    private:
        SharedHandle<CocoaItem> item_;
        CAMetalLayer * metalLayer_;  // strong reference via CFRetain
    };

    /// macOS VisualTree. Holds the single root MTLVisual and the
    /// combined logical→physical scale captured at construction time
    /// (the binder reads this when sizing the backing render target).
    /// Pass 2 will extend this to register/unregister content nodes.
    class MTLVisualTree : public Native::VisualTree {
    public:
        MTLVisualTree(SharedHandle<CocoaItem> rootItem,
                       Composition::Rect rect,
                       float scale);

        Native::Visual * rootVisual() const override;

        float scale() const { return scale_; }

    private:
        SharedHandle<MTLVisual> rootVisual_;
        float scale_ = 1.f;
    };

}

#endif
