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
        [[maybe_unused]] static unsigned toBackingDimension(float logical, float renderScale){
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

        class VKFallbackVisualTree : public BackendVisualTree {
            SharedHandle<Native::GTK::GTKItem> view;
            // Logical->physical pixel scale, sourced from the native window
            // via ViewRenderTarget::getRenderScale(). All backing-dimension
            // math and BackendRenderTargetContext construction use this.
            float renderScale_ = 1.f;
            bool warnedMissingNativeHandle = false;
            bool warnedNativeTargetInitFailure = false;

            bool resolveNativeRenderTargetDescriptor(const Composition::Rect &rect,OmegaGTE::NativeRenderTargetDescriptor &desc){
                desc = {};
                if(view == nullptr){
                    return false;
                }
                // BGRA8Unorm is the format the compositor's pipelines and
                // glyph atlas expect. Set it explicitly so the surface
                // format isn't tied to whatever GTE's default happens to
                // be — GEVulkanEngine::makeNativeRenderTarget will now
                // reject the descriptor if the surface doesn't advertise
                // it (vs. silently picking RGBA8 like the old code).
                desc.pixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm;

#if defined(VULKAN_TARGET_WAYLAND)
                desc.wl_surface = view->getSurface();
                desc.wl_display = view->getDisplay();
                desc.width = toBackingDimension(rect.w, renderScale_);
                desc.height = toBackingDimension(rect.h, renderScale_);
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
                renderScale_ = renderTarget->getRenderScale();
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
                SharedHandle<OmegaGTE::GECommandQueue> presentQueue;
                if(outPresentTarget.nativeTarget == nullptr){
                    OmegaGTE::NativeRenderTargetDescriptor desc {};
                    if(resolveNativeRenderTargetDescriptor(rect,desc)){
                        presentQueue = gte.graphicsEngine->makeCommandQueue(64);
                        outPresentTarget.nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(desc, presentQueue);
                        outPresentTarget.backingWidth = toBackingDimension(rect.w, renderScale_);
                        outPresentTarget.backingHeight = toBackingDimension(rect.h, renderScale_);
                    }
                }
                else {
                    // The native target was created earlier by
                    // resolveDeferredNativeTarget (the GdkWindow wasn't
                    // realized at construction time). That path created the
                    // present queue and bound it to the swap chain, but
                    // didn't surface the queue back to us. Retrieve it from
                    // the native target so the BackendRenderTargetContext
                    // can submit and present on the same queue the swap
                    // chain expects — otherwise begin()/end() bail on a
                    // null queue and present() runs without ever calling
                    // vkAcquireNextImageKHR.
                    presentQueue = outPresentTarget.nativeTarget->presentQueue();
                }

                // Root visual renders directly to the native drawable (Phase A-1).
                auto context = std::make_unique<BackendRenderTargetContext>(rect,outPresentTarget.nativeTarget,std::move(presentQueue),renderScale_);
                return Core::SharedPtr<BackendVisualTree::Visual>(new Visual(pos,std::move(context)));
            }

            Core::SharedPtr<BackendVisualTree::Visual> makeSurfaceVisual(
                    Composition::Rect &rect,Composition::Point2D &pos) override {
                SharedHandle<OmegaGTE::GENativeRenderTarget> nullNative = nullptr;
                SharedHandle<OmegaGTE::GECommandQueue> nullQueue = nullptr;
                auto context = std::make_unique<BackendRenderTargetContext>(rect,nullNative,nullQueue,renderScale_);
                return Core::SharedPtr<BackendVisualTree::Visual>(new Visual(pos,std::move(context)));
            }

            void setRootVisual(Core::SharedPtr<BackendVisualTree::Visual> &visual) override {
                root = visual;
            }

            void applyNativeContentCarveouts(BackendRenderTargetContext & ctx) override {
                // Tier 3 Phase 3.7: per-frame drain hook. Pulls the
                // carve-outs the renderToTarget switch accumulated
                // (translated to backing pixel coords by the context)
                // and prepares to translate each record into a
                // Wayland subsurface set_position / X11 child window
                // ConfigureWindow against this tree's `view`'s native
                // surface. The hostId → subsurface / child-window
                // mapping is owned by GTKItem (registered there by
                // NativeViewHost-Adoption-Plan Phases V2 / G2); until
                // that registry lands, this drain logs the records
                // and clears the list so the next frame starts clean.
                const auto & regions = ctx.pendingNativeContent();
                if(!regions.empty()){
#ifdef OMEGAWTK_TRACE_RENDER
                    for(const auto & r : regions){
                        std::cerr << "[VKFallbackVisualTree] carve-out hostId="
                                  << r.hostId << " z=" << r.zOrderHint
                                  << " px=(" << r.destRectPixels.pos.x
                                  << "," << r.destRectPixels.pos.y << " "
                                  << r.destRectPixels.w << "x"
                                  << r.destRectPixels.h << ")  [no producer"
                                  << " wired — awaiting NativeViewHost V2/G2]"
                                  << std::endl;
                    }
#endif
                }
                ctx.clearPendingNativeContent();
            }

            void resolveDeferredNativeTarget(ViewPresentTarget & outPresentTarget) override {
                if(outPresentTarget.nativeTarget != nullptr || view == nullptr){
                    return;
                }
                Composition::Rect r = view->getRect();
                OmegaGTE::NativeRenderTargetDescriptor desc {};
                if(resolveNativeRenderTargetDescriptor(r,desc)){
                    auto presentQueue = gte.graphicsEngine->makeCommandQueue(64);
                    outPresentTarget.nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(desc, presentQueue);
                    outPresentTarget.backingWidth = toBackingDimension(r.w, renderScale_);
                    outPresentTarget.backingHeight = toBackingDimension(r.h, renderScale_);
                    // Note: deferred resolve doesn't update the BackendRenderTargetContext;
                    // see VKLayerTree's first-frame fallback for where the queue is wired.
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
