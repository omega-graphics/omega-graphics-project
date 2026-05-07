#include "../RenderTarget.h"
#include "../VisualTree.h"
#include "NativePrivate/gtk/GTKItem.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <utility>

namespace OmegaWTK::Composition {

    namespace {
        [[maybe_unused]] static unsigned toBackingDimension(float logical){
            if(!std::isfinite(logical) || logical <= 0.f){
                logical = 1.f;
            }
            return static_cast<unsigned>(std::max<long>(
                    1L,
                    static_cast<long>(std::lround(logical))));
        }

        class VKFallbackVisualTree : public BackendVisualTree {
            SharedHandle<Native::GTK::GTKItem> view;
            bool warnedMissingNativeHandle = false;
            bool warnedNativeTargetInitFailure = false;

            bool resolveNativeRenderTargetDescriptor(const Composition::Rect &rect,OmegaGTE::NativeRenderTargetDescriptor &desc){
                desc = {};
                if(view == nullptr){
                    return false;
                }

#if defined(VULKAN_TARGET_WAYLAND)
                desc.wl_surface = view->getSurface();
                desc.wl_display = view->getDisplay();
                desc.width = toBackingDimension(rect.w);
                desc.height = toBackingDimension(rect.h);
                if(desc.wl_surface != nullptr && desc.wl_display != nullptr){
                    return true;
                }
#endif
#if defined(VULKAN_TARGET_X11)
                desc.x_window = view->getX11Window();
                desc.x_display = view->getDisplay();
                if(desc.x_window != 0 && desc.x_display != nullptr){
                    return true;
                }
#endif
                return false;
            }

            struct Visual : public BackendVisualTree::Visual {
                explicit Visual(Composition::Point2D &pos, std::unique_ptr<BackendRenderTargetContext> context):
                BackendVisualTree::Visual(pos,std::move(context)){
                }

                void resize(Composition::Rect &newRect) override {
                    renderTarget->setRenderTargetSize(newRect);
                }
            };
        public:
            explicit VKFallbackVisualTree(SharedHandle<ViewRenderTarget> &renderTarget){
                view = std::dynamic_pointer_cast<Native::GTK::GTKItem>(renderTarget->getNativePtr());
            }

            void addVisual(Core::SharedPtr<BackendVisualTree::Visual> &visual) override {
                body.push_back(visual);
            }

            Core::SharedPtr<BackendVisualTree::Visual> makeRootVisual(
                    Composition::Rect &rect,Composition::Point2D &pos,
                    ViewPresentTarget & outPresentTarget) override {
                // Only allocate a new GENativeRenderTarget when one isn't
                // already attached. resolveDeferredNativeTarget may have
                // resolved the surface ahead of this call, and Vulkan's
                // vkCreateSwapchainKHR fails with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR
                // when called twice on the same X11/Wayland surface.
                //
                // If the GdkWindow isn't realized yet (eager call from
                // setRootWidget before displayRootWindow), the descriptor
                // resolve fails silently. The factory checks
                // outPresentTarget.nativeTarget afterward and discards the
                // partial root visual; the compositor's first-frame
                // fallback path then re-runs this through resolveDeferredNativeTarget
                // → createRootVisual after the window has been displayed.
                if(outPresentTarget.nativeTarget == nullptr){
                    OmegaGTE::NativeRenderTargetDescriptor desc {};
                    if(resolveNativeRenderTargetDescriptor(rect,desc)){
                        outPresentTarget.nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(desc);
                        outPresentTarget.backingWidth = toBackingDimension(rect.w);
                        outPresentTarget.backingHeight = toBackingDimension(rect.h);
                    }
                }

                // Root visual renders directly to the native drawable (Phase A-1).
                auto context = std::make_unique<BackendRenderTargetContext>(rect,outPresentTarget.nativeTarget,1.f);
                return Core::SharedPtr<BackendVisualTree::Visual>(new Visual(pos,std::move(context)));
            }

            Core::SharedPtr<BackendVisualTree::Visual> makeSurfaceVisual(
                    Composition::Rect &rect,Composition::Point2D &pos) override {
                SharedHandle<OmegaGTE::GENativeRenderTarget> nullNative = nullptr;
                auto context = std::make_unique<BackendRenderTargetContext>(rect,nullNative,1.f);
                return Core::SharedPtr<BackendVisualTree::Visual>(new Visual(pos,std::move(context)));
            }

            void setRootVisual(Core::SharedPtr<BackendVisualTree::Visual> &visual) override {
                root = visual;
            }

            void resolveDeferredNativeTarget(ViewPresentTarget & outPresentTarget) override {
                if(outPresentTarget.nativeTarget != nullptr || view == nullptr){
                    return;
                }
                Composition::Rect r = view->getRect();
                OmegaGTE::NativeRenderTargetDescriptor desc {};
                if(resolveNativeRenderTargetDescriptor(r,desc)){
                    outPresentTarget.nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(desc);
                    outPresentTarget.backingWidth = toBackingDimension(r.w);
                    outPresentTarget.backingHeight = toBackingDimension(r.h);
                }
                else if(!warnedMissingNativeHandle){
                    // Deferred resolve failed too — the GdkWindow still
                    // isn't realized at first-frame time, which means the
                    // window was never displayed (e.g. headless run). The
                    // compositor will skip rendering rather than crash.
                    std::cout << "[OmegaWTK][Vulkan][GTK] Native window handle unavailable for Vulkan render target." << std::endl;
                    warnedMissingNativeHandle = true;
                }
            }
        };
    }

    SharedHandle<BackendVisualTree> BackendVisualTree::Create(SharedHandle<ViewRenderTarget> &view){
        return SharedHandle<BackendVisualTree>(new VKFallbackVisualTree(view));
    }
}
