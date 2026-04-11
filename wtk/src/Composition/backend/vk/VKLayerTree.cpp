#include "../RenderTarget.h"
#include "../VisualTree.h"
#include "NativePrivate/gtk/GTKItem.h"

#include <algorithm>
#include <cmath>
#include <iostream>

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
                explicit Visual(Composition::Point2D &pos, BackendRenderTargetContext &context):
                BackendVisualTree::Visual(pos,context){
                }

                void resize(Composition::Rect &newRect) override {
                    renderTarget.setRenderTargetSize(newRect);
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
                OmegaGTE::NativeRenderTargetDescriptor desc {};
                if(resolveNativeRenderTargetDescriptor(rect,desc)){
                    outPresentTarget.nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(desc);
                    outPresentTarget.backingWidth = toBackingDimension(rect.w);
                    outPresentTarget.backingHeight = toBackingDimension(rect.h);
                }
                else {
                    if(!warnedMissingNativeHandle){
                        std::cout << "[OmegaWTK][Vulkan][GTK] Native window handle unavailable for Vulkan render target." << std::endl;
                        warnedMissingNativeHandle = true;
                    }
                }

                SharedHandle<OmegaGTE::GENativeRenderTarget> nullNative = nullptr;
                BackendRenderTargetContext context {rect,nullNative,1.f};
                return Core::SharedPtr<BackendVisualTree::Visual>(new Visual(pos,context));
            }

            Core::SharedPtr<BackendVisualTree::Visual> makeSurfaceVisual(
                    Composition::Rect &rect,Composition::Point2D &pos) override {
                SharedHandle<OmegaGTE::GENativeRenderTarget> nullNative = nullptr;
                BackendRenderTargetContext context {rect,nullNative,1.f};
                return Core::SharedPtr<BackendVisualTree::Visual>(new Visual(pos,context));
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
            }
        };
    }

    SharedHandle<BackendVisualTree> BackendVisualTree::Create(SharedHandle<ViewRenderTarget> &view){
        return SharedHandle<BackendVisualTree>(new VKFallbackVisualTree(view));
    }
}
