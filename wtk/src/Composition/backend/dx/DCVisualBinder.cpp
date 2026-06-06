// §2.14 Pass 1 (Win32) — Composition side. Bridges
// `Native::Win::DCVisualTree`'s DComp topology into the per-frame
// `BackendRenderTargetContext`. Same shape as Linux's VKVisualBinder
// and macOS's MTLVisualBinder — downcast the abstract tree, read the
// platform handle (DComp visual + HWND), construct the
// GENativeRenderTarget through GTE, hand the swap chain back to the
// Native side for `SetContent` / `SetRoot` / `Commit`, build the RTC,
// install the `onResize` hook.
//
// COMPILE-UNVERIFIED off-platform.

#include "../VisualBinder.h"
#include "NativePrivate/win/DCVisualTree.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace OmegaWTK::Composition {

    namespace {

        unsigned toBackingDimension(float logical, float renderScale){
            if(!std::isfinite(logical) || logical <= 0.f) logical = 1.f;
            if(!std::isfinite(renderScale) || renderScale <= 0.f) renderScale = 1.f;
            return static_cast<unsigned>(std::max<long>(
                    1L,
                    static_cast<long>(std::lround(logical * renderScale))));
        }

    }

    std::unique_ptr<BackendRenderTargetContext>
    tryBindRootVisual(Native::VisualTree & tree){
        auto *dcTree = dynamic_cast<Native::Win::DCVisualTree *>(&tree);
        if(dcTree == nullptr){
            return nullptr;
        }
        auto *visual = dynamic_cast<Native::Win::DCVisual *>(dcTree->rootVisual());
        if(visual == nullptr){
            return nullptr;
        }
        if(visual->directCompositionVisual() == nullptr || dcTree->hwndTarget() == nullptr){
            // DComp device/target/visual creation failed in the Native
            // ctor — retrying won't recover.
            std::cerr << "[OmegaWTK][D3D12][DComp] Native DCVisual has no IDCompositionVisual2 / hwndTarget." << std::endl;
            return nullptr;
        }

        Composition::Rect rect = visual->rect();
        const float scale = dcTree->scale();

        // Field-by-name init (C++17): adding fields to
        // NativeRenderTargetDescriptor doesn't silently rebind
        // positional values. BGRA8Unorm matches the format the
        // compositor's pipelines, glyph atlas, and DComp content all
        // expect.
        OmegaGTE::NativeRenderTargetDescriptor desc{};
        desc.isHwnd = false;
        desc.hwnd = nullptr;
        desc.width  = toBackingDimension(rect.w, scale);
        desc.height = toBackingDimension(rect.h, scale);
        desc.pixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm;

        OmegaGTE::GECommandQueueDesc presentQueueDesc{};
        presentQueueDesc.type = OmegaGTE::GECommandQueueDesc::Type::Graphics;
        presentQueueDesc.priority = OmegaGTE::GECommandQueueDesc::Priority::High;
        presentQueueDesc.maxBufferCount = 64;
        presentQueueDesc.label = "WTK::DCVisualBinder presentQueue";
        auto presentQueue = gte.graphicsEngine->makeCommandQueue(presentQueueDesc);
        auto nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(desc, presentQueue);
        if(nativeTarget == nullptr){
            std::cerr << "[OmegaWTK][D3D12][DComp] makeNativeRenderTarget returned null." << std::endl;
            return nullptr;
        }
        IUnknown *swapChain = static_cast<IUnknown *>(nativeTarget->getSwapChain());
        if(swapChain == nullptr){
            std::cerr << "[OmegaWTK][D3D12][DComp] GENativeRenderTarget has no swap chain." << std::endl;
            return nullptr;
        }

        // §2.14 split: Native owns DComp visual ordering; Composition
        // owns GTE / swap-chain. Hand the swap chain back to Native to
        // run `SetContent` + `SetOpacityMode` + `SetRoot` + `Commit` +
        // `WaitForCommitCompletion` — same sequence
        // `DCVisualTree::setRootVisual` ran pre-§2.14.
        visual->bindSwapChain(swapChain,
                              dcTree->hwndTarget(),
                              Native::Win::DCVisualTree::desktopDevice());

        auto rtc = std::make_unique<BackendRenderTargetContext>(
            rect, nativeTarget, std::move(presentQueue), scale);

        // §2.14 Pass 1: forward Visual::resize ticks to
        // setRenderTargetSize. Compositor::detachVisualTree clears
        // this before the RTC drops, so the raw capture never
        // outlives the RTC.
        auto *rtcRaw = rtc.get();
        visual->setOnResize([rtcRaw](const Composition::Rect & newRect){
            Composition::Rect mut = newRect;
            rtcRaw->setRenderTargetSize(mut);
        });

        return rtc;
    }

}
