#include "DCVisualTree.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include "NativePrivate/win/HWNDItem.h"



namespace OmegaWTK::Composition {

    IDCompositionDevice3 *comp_device = nullptr;
    IDCompositionDesktopDevice *comp_device_desktop = nullptr;
    // IDCompositionDevice3 *comp_device_2 = nullptr;

    namespace {
        static unsigned toBackingDimension(float logical,float renderScale){
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
    }

    SharedHandle<BackendVisualTree> BackendVisualTree::Create(SharedHandle<ViewRenderTarget> & view){
        return SharedHandle<BackendVisualTree>(new DCVisualTree(view));
    }

    
    DCVisualTree::DCVisualTree(SharedHandle<ViewRenderTarget> & view){
        if(comp_device == nullptr) {
            IDCompositionDesktopDevice *dev;
            auto hr = DCompositionCreateDevice3(NULL,IID_PPV_ARGS(&dev));
            if(FAILED(hr)){

            };
            dev->QueryInterface(IID_PPV_ARGS(&comp_device));
            comp_device_desktop = dev;
        }

        auto hwndItem = std::dynamic_pointer_cast<Native::Win::HWNDItem>(view->getNativePtr());
        if(hwndItem != nullptr && hwndItem->hwnd != nullptr){
            const auto dpi = GetDpiForWindow(hwndItem->hwnd);
            if(dpi > 0){
                renderScale = static_cast<float>(dpi) / 96.f;
            }
        }
        HRESULT res = comp_device_desktop->CreateTargetForHwnd(hwndItem->hwnd,FALSE,&hwndTarget.comPtr);
        if(FAILED(res)){
            OMEGAWTK_DEBUG("Failed to Create Render Target for HWND");
        }
    };

     DCVisualTree::RootVisual::RootVisual(Composition::Point2D &pos,
                                  BackendRenderTargetContext & context,
                                  IDCompositionVisual2 * visual,
                                  float renderScale):
     Parent::Visual(pos,context),
     visual(visual),
     renderScale(renderScale){
     };

    void DCVisualTree::RootVisual::resize(Composition::Rect &newRect){
        renderTarget.setRenderTargetSize(newRect);
    }

    DCVisualTree::RootVisual::~RootVisual(){
        if(visual != nullptr){
            visual->RemoveAllVisuals();
            Core::SafeRelease(&visual);
        }
    };

    DCVisualTree::SurfaceVisual::SurfaceVisual(Composition::Point2D &pos,
                                  BackendRenderTargetContext & context,
                                  float renderScale):
     Parent::Visual(pos,context),
     renderScale(renderScale){
     };

    void DCVisualTree::SurfaceVisual::resize(Composition::Rect &newRect){
        renderTarget.setRenderTargetSize(newRect);
    }

    Core::SharedPtr<BackendVisualTree::Visual> DCVisualTree::makeRootVisual(
            Composition::Rect & rect,Composition::Point2D & pos,
            ViewPresentTarget & outPresentTarget){

        OmegaGTE::NativeRenderTargetDescriptor desc {};
        desc.isHwnd = false;
        desc.hwnd = nullptr;
        desc.height = toBackingDimension(rect.h,renderScale);
        desc.width = toBackingDimension(rect.w,renderScale);

        // Create the native render target — owned by ViewPresentTarget.
        auto nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(desc);
        auto swapChain = (IDXGISwapChain3 *)nativeTarget->getSwapChain();

        outPresentTarget.nativeTarget = nativeTarget;
        outPresentTarget.backingWidth = desc.width;
        outPresentTarget.backingHeight = desc.height;

        // Root visual renders directly to the native drawable (Phase A-1).
        BackendRenderTargetContext context {rect,nativeTarget,renderScale};

        HRESULT hr;
        IDCompositionVisual2 *v;
        hr = comp_device->CreateVisual(&v);
        if(FAILED(hr)){
            OMEGAWTK_DEBUG("Failed to Create Visual For DCVisualTree");
        };

        hr = v->SetContent(swapChain);
        if(FAILED(hr)){
            std::stringstream ss;
            // ss << std::hex << hr;
            // MessageBoxA(HWND_DESKTOP,(std::string("Failed to set Content of Visual. ERROR:") + ss.str()).c_str(),NULL,MB_OK);
        };

        return SharedHandle<Parent::Visual>(new DCVisualTree::RootVisual {pos,context,v,renderScale});
    };

    Core::SharedPtr<BackendVisualTree::Visual> DCVisualTree::makeSurfaceVisual(
            Composition::Rect & rect,Composition::Point2D & pos){

        // Surface-only: texture + texture render target, no swap chain, no DComp visual.
        SharedHandle<OmegaGTE::GENativeRenderTarget> nullNative = nullptr;
        BackendRenderTargetContext context {rect,nullNative,renderScale};

        return SharedHandle<Parent::Visual>(new DCVisualTree::SurfaceVisual {pos,context,renderScale});
    };

    void DCVisualTree::setRootVisual(Core::SharedPtr<Parent::Visual> & visual){
        root = visual;
        auto v = std::dynamic_pointer_cast<RootVisual>(visual);
        if(v != nullptr){
            ResourceTrace::emit("Bind",
                                "BackendVisual",
                                v->traceResourceId,
                                "DCVisualTree::Root",
                                this);
            v->visual->SetOpacityMode(DCOMPOSITION_OPACITY_MODE_LAYER);
            hwndTarget->SetRoot(v->visual);
            comp_device_desktop->Commit();
            comp_device_desktop->WaitForCommitCompletion();
        }
    };


    void DCVisualTree::addVisual(Core::SharedPtr<Parent::Visual> & visual){
        body.push_back(visual);
        if(visual != nullptr){
            ResourceTrace::emit("Bind",
                                "BackendVisual",
                                visual->traceResourceId,
                                "DCVisualTree::Body",
                                this);
        }
        // Surface-only visuals have no DComp visual representation.
        // Their content is composited via viewport override into the root surface.
    };



};
