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

        class VKCanvasEffectProcessor : public BackendCanvasEffectProcessor {
        public:
            explicit VKCanvasEffectProcessor(SharedHandle<OmegaGTE::GEFence> &fence):
            BackendCanvasEffectProcessor(fence){
            }

            void applyEffects(SharedHandle<OmegaGTE::GETexture> &dest,
                              SharedHandle<OmegaGTE::GETextureRenderTarget> &textureTarget,
                              OmegaCommon::Vector<CanvasEffect> &effects) override {
                (void)dest;
                (void)textureTarget;
                (void)effects;
            }
        };

        class VKFallbackVisualTree : public BackendVisualTree {
            SharedHandle<Native::GTK::GTKItem> view;
            SharedHandle<OmegaGTE::GENativeRenderTarget> nativeTarget;
            bool warnedMissingNativeHandle = false;
            bool warnedNativeTargetInitFailure = false;

            bool resolveNativeRenderTargetDescriptor(const Core::Rect &rect,OmegaGTE::NativeRenderTargetDescriptor &desc){
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

            SharedHandle<OmegaGTE::GENativeRenderTarget> getOrCreateNativeTarget(const Core::Rect &rect){
                if(nativeTarget != nullptr){
                    return nativeTarget;
                }
                OmegaGTE::NativeRenderTargetDescriptor desc {};
                if(!resolveNativeRenderTargetDescriptor(rect,desc)){
                    if(!warnedMissingNativeHandle){
                        std::cout << "[OmegaWTK][Vulkan][GTK] Native window handle unavailable for Vulkan render target." << std::endl;
                        warnedMissingNativeHandle = true;
                    }
                    return nullptr;
                }
                nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(desc);
                if(nativeTarget == nullptr && !warnedNativeTargetInitFailure){
                    std::cout << "[OmegaWTK][Vulkan][GTK] Failed to create Vulkan native render target from GTK descriptor." << std::endl;
                    warnedNativeTargetInitFailure = true;
                }
                return nativeTarget;
            }

            struct Visual : public BackendVisualTree::Visual {
                explicit Visual(Core::Position &pos, BackendRenderTargetContext &context):
                BackendVisualTree::Visual(pos,context){
                }

                void resize(Core::Rect &newRect) override {
                    renderTarget.setRenderTargetSize(newRect);
                }

                void updateShadowEffect(LayerEffect::DropShadowParams &params) override {
                    (void)params;
                }

                void updateTransformEffect(LayerEffect::TransformationParams &params) override {
                    (void)params;
                }
            };
        public:
            explicit VKFallbackVisualTree(SharedHandle<ViewRenderTarget> &renderTarget){
                view = std::dynamic_pointer_cast<Native::GTK::GTKItem>(renderTarget->getNativePtr());
            }

            void addVisual(Core::SharedPtr<BackendVisualTree::Visual> &visual) override {
                body.push_back(visual);
            }

            Core::SharedPtr<BackendVisualTree::Visual> makeVisual(Core::Rect &rect,Core::Position &pos) override {
                auto target = getOrCreateNativeTarget(rect);
                BackendRenderTargetContext context {rect,target,1.f};

                return Core::SharedPtr<BackendVisualTree::Visual>(new Visual(pos,context));
            }

            void setRootVisual(Core::SharedPtr<BackendVisualTree::Visual> &visual) override {
                root = visual;
            }
        };
    }

    SharedHandle<BackendCanvasEffectProcessor>
    BackendCanvasEffectProcessor::Create(SharedHandle<OmegaGTE::GEFence> &fence) {
        return SharedHandle<BackendCanvasEffectProcessor>(new VKCanvasEffectProcessor(fence));
    }

    SharedHandle<BackendVisualTree> BackendVisualTree::Create(SharedHandle<ViewRenderTarget> &view){
        return SharedHandle<BackendVisualTree>(new VKFallbackVisualTree(view));
    }
}
