// §2.14 Pass 1 (macOS) — Composition side. Bridges
// `Native::Cocoa::MTLVisualTree`'s `CAMetalLayer` into the per-frame
// `BackendRenderTargetContext` the compositor's renderer drives. Same
// shape as the Linux `VKVisualBinder` — read the platform handle off
// the per-backend Visual subclass, wrap it in
// `OmegaGTE::GENativeRenderTarget`, build the RTC, install the
// `onResize` hook.
//
// COMPILE-UNVERIFIED off-platform. The pattern is identical to the
// Linux binder (already verified) modulo the platform handle: a
// `CAMetalLayer *` here vs. an X11 `Window` + `Display *` there.
// Drift between them would be the first thing to audit if it lands
// red on a macOS build.

#include "../VisualBinder.h"
#include "NativePrivate/macos/MTLVisualTree.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace OmegaWTK::Composition {

    namespace {

        constexpr CGFloat kMaxDrawableDimension = 16384.f;

        unsigned toBackingDimension(float logical, float renderScale){
            if(!std::isfinite(logical) || logical <= 0.f) logical = 1.f;
            if(!std::isfinite(renderScale) || renderScale <= 0.f) renderScale = 1.f;
            const CGFloat raw = static_cast<CGFloat>(logical) * static_cast<CGFloat>(renderScale);
            const CGFloat clamped = std::clamp(raw, static_cast<CGFloat>(1.f), kMaxDrawableDimension);
            return static_cast<unsigned>(clamped);
        }

    }

    std::unique_ptr<BackendRenderTargetContext>
    tryBindRootVisual(Native::VisualTree & tree){
        // Downcast to the macOS-specific subclass. A non-MTLVisualTree
        // reaching this binder is a programming error (per-backend
        // factory + binder always paired in the build).
        auto *mtlTree = dynamic_cast<Native::Cocoa::MTLVisualTree *>(&tree);
        if(mtlTree == nullptr){
            return nullptr;
        }
        auto *visual = dynamic_cast<Native::Cocoa::MTLVisual *>(mtlTree->rootVisual());
        if(visual == nullptr){
            return nullptr;
        }

        CAMetalLayer *layer = visual->metalLayer();
        if(layer == nil){
            // Native side did not produce a layer (CocoaItem mis-cast
            // at tree construction). No retry will succeed; bail.
            std::cerr << "[OmegaWTK][Metal][Cocoa] MTLVisual has no CAMetalLayer." << std::endl;
            return nullptr;
        }

        Composition::Rect rect = visual->rect();
        const float scale = mtlTree->scale();

        // Field-by-name init (C++17): adding fields to
        // NativeRenderTargetDescriptor doesn't silently rebind
        // positional values. BGRA8Unorm matches the layer's
        // pixelFormat (set in Native MTLVisualTree's
        // buildMetalLayer) and is the universally-supported
        // drawable format.
        OmegaGTE::GECommandQueueDesc presentQueueDesc{};
        presentQueueDesc.type = OmegaGTE::GECommandQueueDesc::Type::Graphics;
        presentQueueDesc.priority = OmegaGTE::GECommandQueueDesc::Priority::High;
        presentQueueDesc.maxBufferCount = 64;
        presentQueueDesc.label = "WTK::MTLVisualBinder presentQueue";
        auto presentQueue = gte.graphicsEngine->makeCommandQueue(presentQueueDesc);
        OmegaGTE::NativeRenderTargetDescriptor desc{};
        desc.pixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm;
        desc.metalLayer = layer;
        auto nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(desc, presentQueue);
        if(nativeTarget == nullptr){
            std::cerr << "[OmegaWTK][Metal][Cocoa] makeNativeRenderTarget returned null." << std::endl;
            return nullptr;
        }

        auto rtc = std::make_unique<BackendRenderTargetContext>(
            rect, nativeTarget, std::move(presentQueue), scale);

        // §2.14 Pass 1: forward Visual::resize ticks to
        // setRenderTargetSize. Same lifetime rule as the Linux
        // binder — Compositor::detachVisualTree clears this hook
        // before the RTC drops, so the raw capture never outlives
        // the RTC.
        (void)toBackingDimension; // first-cut macOS path reads backing dims via the layer's drawableSize; helper retained for parity with Linux.
        auto *rtcRaw = rtc.get();
        visual->setOnResize([rtcRaw](const Composition::Rect & newRect){
            Composition::Rect mut = newRect;
            rtcRaw->setRenderTargetSize(mut);
        });

        return rtc;
    }

}
