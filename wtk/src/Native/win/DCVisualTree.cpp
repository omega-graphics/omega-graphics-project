// §2.14 Pass 1 (Win32) — Native side. DirectComposition topology.
// Process-global IDCompositionDevice3 + IDCompositionDesktopDevice
// initialized lazily on the first tree, mirroring the pre-§2.14
// `DCVisualTree::DCVisualTree` constructor. Per-window
// IDCompositionTarget bound to the HWND. Root IDCompositionVisual2.
// The Composition layer creates the IDXGISwapChain and hands it back
// to `DCVisual::bindSwapChain` here, which runs the SetContent →
// SetOpacityMode → hwndTarget->SetRoot → device Commit /
// WaitForCommitCompletion sequence.
//
// COMPILE-UNVERIFIED off-platform. Pattern-matched against the
// shipped Linux migration; same SharedHandle ownership, same
// onResize install site in the binder.

#include "NativePrivate/win/DCVisualTree.h"

#include <mutex>
#include <utility>

#pragma comment(lib, "dcomp.lib")

namespace OmegaWTK::Native::Win {

    namespace {

        // Process-global DComp devices, created on first tree
        // construction. Matches the pre-§2.14 `comp_device` /
        // `comp_device_desktop` statics in
        // `Composition::DCVisualTree`. The DComp device is a single
        // process-wide composition surface; sharing it across
        // windows is the canonical pattern.
        IDCompositionDesktopDevice *gDesktopDevice = nullptr;
        IDCompositionDevice3       *gDevice        = nullptr;
        std::once_flag              gDeviceInitOnce;

        void initDevicesIfNeeded(){
            std::call_once(gDeviceInitOnce, []{
                IDCompositionDesktopDevice *dev = nullptr;
                HRESULT hr = DCompositionCreateDevice3(NULL, IID_PPV_ARGS(&dev));
                if(FAILED(hr) || dev == nullptr){
                    // Same diagnostic shape the pre-§2.14 code used:
                    // a one-line debug print plus silent fall-through
                    // — DComp device creation effectively never fails
                    // on a sane Windows install; we keep going so the
                    // rest of the AppWindow ctor doesn't crash on a
                    // null device pointer (subsequent calls just
                    // no-op).
                    OMEGAWTK_DEBUG("DCompositionCreateDevice3 failed");
                    return;
                }
                dev->QueryInterface(IID_PPV_ARGS(&gDevice));
                gDesktopDevice = dev;
            });
        }

    }

    IDCompositionDesktopDevice * DCVisualTree::desktopDevice(){
        initDevicesIfNeeded();
        return gDesktopDevice;
    }

    // ---- DCVisual ----

    DCVisual::DCVisual(SharedHandle<HWNDItem> item,
                        IDCompositionVisual2 *visual,
                        Composition::Rect rect):
        Visual(rect),
        item_(std::move(item)),
        visual_(visual) {}

    DCVisual::~DCVisual(){
        if(visual_ != nullptr){
            visual_->RemoveAllVisuals();
            Core::SafeRelease(&visual_);
        }
    }

    HWND DCVisual::hwnd() const {
        return item_ != nullptr ? item_->hwnd : nullptr;
    }

    void DCVisual::bindSwapChain(IUnknown *swapChain,
                                  IDCompositionTarget *hwndTarget,
                                  IDCompositionDesktopDevice *desktopDevice){
        if(visual_ == nullptr || swapChain == nullptr || hwndTarget == nullptr){
            return;
        }
        // SetContent binds the swap chain as this visual's drawable;
        // SetOpacityMode = LAYER lets the DWM composite the visual
        // with alpha (same value the pre-§2.14 code used).
        HRESULT hr = visual_->SetContent(swapChain);
        if(FAILED(hr)){
            OMEGAWTK_DEBUG("DCVisual::bindSwapChain: SetContent failed");
        }
        visual_->SetOpacityMode(DCOMPOSITION_OPACITY_MODE_LAYER);
        hwndTarget->SetRoot(visual_);
        if(desktopDevice != nullptr){
            // Synchronous commit matches the pre-§2.14 ordering — the
            // wait keeps subsequent paint draws from racing the
            // visual's first present.
            desktopDevice->Commit();
            desktopDevice->WaitForCommitCompletion();
        }
    }

    // ---- DCVisualTree ----

    DCVisualTree::DCVisualTree(SharedHandle<HWNDItem> rootItem,
                                Composition::Rect rect,
                                float scale):
        scale_(scale > 0.f ? scale : 1.f)
    {
        initDevicesIfNeeded();

        // Per-window hwnd target — needs to be created before the
        // DComp visual is rooted onto it.
        if(gDesktopDevice != nullptr && rootItem != nullptr && rootItem->hwnd != nullptr){
            HRESULT hr = gDesktopDevice->CreateTargetForHwnd(rootItem->hwnd, FALSE, &hwndTarget_.comPtr);
            if(FAILED(hr)){
                OMEGAWTK_DEBUG("DCVisualTree: CreateTargetForHwnd failed");
            }
        }

        // Root DComp visual. The swap chain is attached later by the
        // Composition binder via `DCVisual::bindSwapChain`.
        IDCompositionVisual2 *visual = nullptr;
        if(gDevice != nullptr){
            HRESULT hr = gDevice->CreateVisual(&visual);
            if(FAILED(hr) || visual == nullptr){
                OMEGAWTK_DEBUG("DCVisualTree: CreateVisual failed");
                visual = nullptr;
            }
        }

        rootVisual_ = std::make_shared<DCVisual>(std::move(rootItem), visual, rect);
    }

    DCVisualTree::~DCVisualTree() = default;

    Native::Visual * DCVisualTree::rootVisual() const {
        return rootVisual_.get();
    }

}

namespace OmegaWTK::Native {

    NativeVisualTreePtr make_native_visual_tree(NativeItemPtr rootItem,
                                                 const Composition::Rect & rect,
                                                 float scale){
        auto hwndItem = std::dynamic_pointer_cast<Win::HWNDItem>(rootItem);
        if(hwndItem == nullptr){
            return nullptr;
        }
        return std::make_shared<Win::DCVisualTree>(std::move(hwndItem), rect, scale);
    }

}
