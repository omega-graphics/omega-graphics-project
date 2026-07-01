#include "../VisualBinder.h"
#include "NativePrivate/gtk/VKVisualTree.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace OmegaWTK::Composition {

    namespace {

        [[maybe_unused]] unsigned toBackingDimension(float logical, float renderScale){
            if(!std::isfinite(logical) || logical <= 0.f){
                logical = 1.f;
            }
            if(!std::isfinite(renderScale) || renderScale <= 0.f){
                renderScale = 1.f;
            }
            return static_cast<unsigned>(std::max<long>(
                    1L,
                    static_cast<long>(std::lround(logical * renderScale))));
        }

        bool fillDescriptor(Native::GTK::VKVisual & visual,
                            const Composition::Rect & rect,
                            float scale,
                            OmegaGTE::NativeRenderTargetDescriptor & desc){
            desc = {};
            // BGRA8Unorm: the compositor's pipelines + glyph atlas
            // expect this. Setting it explicitly so the surface format
            // isn't tied to GTE's default (avoids the pre-§2.14 silent
            // RGBA8 pickup when the surface didn't advertise BGRA8).
            desc.pixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm;

            // §2.13a: dispatch on the visual's runtime protocol instead of a
            // compile-time #if, so a single OmegaWTK_Native fills the right
            // descriptor field set on either an X11 or a Wayland session.
            // GTE's makeNativeRenderTarget already switches by populated
            // handle (GEVulkan.cpp), so populating exactly one set is all the
            // co-build needs. Each case is still #if-guarded so a single-
            // protocol build only references the accessors it compiled.
            (void)rect;
            (void)scale;
            switch(visual.backend()){
#if WTK_NATIVE_WAYLAND
            case Native::GTK::WindowingBackend::Wayland:
                desc.wl_surface = visual.waylandSurface();
                desc.wl_display = visual.waylandDisplay();
                // Vulkan WSI on Wayland reports no surface extent
                // (currentExtent == 0xFFFFFFFF), so the swap chain must be
                // sized from the descriptor; X11 reads extent from the Window.
                desc.width  = toBackingDimension(rect.w, scale);
                desc.height = toBackingDimension(rect.h, scale);
                return desc.wl_surface != nullptr && desc.wl_display != nullptr;
#endif
#if WTK_NATIVE_X11
            case Native::GTK::WindowingBackend::X11:
                desc.x_window  = visual.x11Window();
                desc.x_display = visual.x11Display();
                return desc.x_window != 0 && desc.x_display != nullptr;
#endif
            default:
                return false;
            }
        }

    }

    std::unique_ptr<BackendRenderTargetContext>
    tryBindRootVisual(Native::VisualTree & tree){
        // Downcast to the Linux-specific subclass. A non-VKVisualTree
        // reaching this binder is a programming error (the per-backend
        // `make_native_visual_tree` factory and the per-backend binder
        // are always paired in the build).
        auto *vkTree = dynamic_cast<Native::GTK::VKVisualTree *>(&tree);
        if(vkTree == nullptr){
            return nullptr;
        }
        auto *visual = dynamic_cast<Native::GTK::VKVisual *>(vkTree->rootVisual());
        if(visual == nullptr){
            return nullptr;
        }

        Composition::Rect rect = visual->rect();
        const float scale = vkTree->scale();

        OmegaGTE::NativeRenderTargetDescriptor desc{};
        if(!fillDescriptor(*visual, rect, scale, desc)){
            // Pre-realize — caller retries.
            return nullptr;
        }

        // The Vulkan swap chain is bound to a dedicated 64-deep present
        // queue, matching the pre-§2.14 path in VKLayerTree::makeRootVisual.
        OmegaGTE::GECommandQueueDesc presentQueueDesc{};
        presentQueueDesc.type = OmegaGTE::GECommandQueueDesc::Type::Graphics;
        presentQueueDesc.priority = OmegaGTE::GECommandQueueDesc::Priority::High;
        presentQueueDesc.maxBufferCount = 64;
        presentQueueDesc.label = "WTK::VKVisualBinder presentQueue";
        auto presentQueue = gte.graphicsEngine->makeCommandQueue(presentQueueDesc);
        auto nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(desc, presentQueue);
        if(nativeTarget == nullptr){
            // GTE rejected the descriptor — surface format mismatch or
            // missing extension. Log once-ish (the compositor will keep
            // retrying so spamming would be ugly; one diagnostic line
            // is enough to flag the misconfiguration).
            std::cerr << "[OmegaWTK][Vulkan][GTK] makeNativeRenderTarget returned null." << std::endl;
            return nullptr;
        }

        auto rtc = std::make_unique<BackendRenderTargetContext>(
            rect, nativeTarget, std::move(presentQueue), scale);

        // §2.14 Pass 1: wire `Visual::resize` to the RTC's
        // `setRenderTargetSize` so resize ticks propagate to the
        // backing render target. The compositor's `detachVisualTree`
        // clears this hook before the RTC drops, so the raw capture
        // never outlives the RTC.
        auto *rtcRaw = rtc.get();
        visual->setOnResize([rtcRaw](const Composition::Rect & newRect){
            // `setRenderTargetSize` takes a non-const reference (legacy
            // signature); copy to a mutable local first.
            Composition::Rect mut = newRect;
            rtcRaw->setRenderTargetSize(mut);
        });

        return rtc;
    }

}
