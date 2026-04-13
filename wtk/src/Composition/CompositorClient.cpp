#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/Composition/Canvas.h"
#include "Compositor.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace OmegaWTK::Composition {

    namespace {
        std::atomic<uint64_t> g_syncLaneSeed {1};

        constexpr float kMaxTextureDimension = 16384.f;
#if defined(TARGET_MACOS)
        constexpr float kLogicalScaleFloor = 2.f;
#else
        constexpr float kLogicalScaleFloor = 1.f;
#endif
        constexpr float kMaxLogicalLayerDimension = kMaxTextureDimension / kLogicalScaleFloor;

        Composition::Rect sanitizeRect(const Composition::Rect & candidate,
                                       const Composition::Rect & fallback){
            Composition::Rect saneFallback = fallback;
            if(!std::isfinite(saneFallback.pos.x)){
                saneFallback.pos.x = 0.f;
            }
            if(!std::isfinite(saneFallback.pos.y)){
                saneFallback.pos.y = 0.f;
            }
            if(!std::isfinite(saneFallback.w) || saneFallback.w <= 0.f){
                saneFallback.w = 1.f;
            }
            if(!std::isfinite(saneFallback.h) || saneFallback.h <= 0.f){
                saneFallback.h = 1.f;
            }
            saneFallback.w = std::clamp(saneFallback.w,1.f,kMaxLogicalLayerDimension);
            saneFallback.h = std::clamp(saneFallback.h,1.f,kMaxLogicalLayerDimension);

            Composition::Rect sane = candidate;
            if(!std::isfinite(sane.pos.x)){
                sane.pos.x = saneFallback.pos.x;
            }
            if(!std::isfinite(sane.pos.y)){
                sane.pos.y = saneFallback.pos.y;
            }
            if(!std::isfinite(sane.w) || sane.w <= 0.f){
                sane.w = saneFallback.w;
            }
            if(!std::isfinite(sane.h) || sane.h <= 0.f){
                sane.h = saneFallback.h;
            }
            sane.w = std::clamp(sane.w,1.f,kMaxLogicalLayerDimension);
            sane.h = std::clamp(sane.h,1.f,kMaxLogicalLayerDimension);
            return sane;
        }
    }

    CompositorClientProxy::CompositorClientProxy(SharedHandle<CompositionRenderTarget> renderTarget):
    renderTarget(std::move(renderTarget)),
    syncLaneId(g_syncLaneSeed.fetch_add(1)) {

    }

    CompositorClientProxy::CompositorClientProxy():
    renderTarget(nullptr),
    syncLaneId(g_syncLaneSeed.fetch_add(1)) {

    }

    void CompositorClientProxy::setRenderTarget(SharedHandle<CompositionRenderTarget> newRenderTarget){
        std::lock_guard<std::mutex> lk(commandMutex);
        renderTarget = std::move(newRenderTarget);
    }

    void CompositorClientProxy::setSyncLaneId(uint64_t syncLaneId){
        std::lock_guard<std::mutex> lk(commandMutex);
        this->syncLaneId = syncLaneId;
    }

    uint64_t CompositorClientProxy::getSyncLaneId() const {
        std::lock_guard<std::mutex> lk(commandMutex);
        return syncLaneId;
    }

    SyncLaneDiagnostics CompositorClientProxy::getSyncLaneDiagnostics() const {
        SyncLaneDiagnostics diagnostics {};
        std::lock_guard<std::mutex> lk(commandMutex);
        diagnostics.syncLaneId = syncLaneId;
        return diagnostics;
    }

    Compositor *CompositorClientProxy::getFrontendPtr() const {
        std::lock_guard<std::mutex> lk(commandMutex);
        return frontend;
    }

    void CompositorClientProxy::setActiveCompositeFrame(CompositeFrame *frame){
        activeCompositeFrame_ = frame;
    }

    void CompositorClientProxy::setFrontendPtr(Compositor *frontend){
        std::lock_guard<std::mutex> lk(commandMutex);
        this->frontend = frontend;
    };

    CompositorClient::CompositorClient(CompositorClientProxy &proxy):
    parentProxy(proxy){

    }

    void CompositorClient::pushLayerResizeCommand(Layer *target, int delta_x, int delta_y,
                                                  int delta_w, int delta_h, Timestamp &/*start*/,
                                                  Timestamp &/*deadline*/) {
        if(target == nullptr){
            return;
        }
        auto priorRect = sanitizeRect(
                target->getLayerRect(),
                Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});
        auto layerRect = priorRect;
        layerRect.pos.x += static_cast<float>(delta_x);
        layerRect.pos.y += static_cast<float>(delta_y);
        layerRect.w += static_cast<float>(delta_w);
        layerRect.h += static_cast<float>(delta_h);
        layerRect = sanitizeRect(layerRect,priorRect);
        target->resize(layerRect);
    }

    void CompositorClient::pushLayerEffectCommand(Layer */*target*/,
                                                  SharedHandle<LayerEffect> &/*effect*/,
                                                  Timestamp &/*start*/,
                                                  Timestamp &/*deadline*/) {
        // Layer effects (shadow, transform) are now draw-time Canvas
        // operations. The legacy command path is a no-op until the
        // Animation API rework wires effects through the new path.
    }

    void CompositorClient::pushViewResizeCommand(Native::NativeItemPtr nativeView,int delta_x,int delta_y,int delta_w,int delta_h,Timestamp &/*start*/,Timestamp &/*deadline*/){
        if(nativeView == nullptr){
            return;
        }
        auto currentRect = sanitizeRect(
                nativeView->getRect(),
                Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});
        auto nextRect = currentRect;
        nextRect.pos.x += static_cast<float>(delta_x);
        nextRect.pos.y += static_cast<float>(delta_y);
        nextRect.w += static_cast<float>(delta_w);
        nextRect.h += static_cast<float>(delta_h);
        nextRect = sanitizeRect(nextRect,currentRect);
        nativeView->resize(nextRect);
    }

    void CompositorClient::pushFrame(SharedHandle<CanvasFrame> &frame, Timestamp &/*start*/) {
        if(parentProxy.activeCompositeFrame_ == nullptr || frame == nullptr){
            return;
        }
        CompositeFrame::WidgetSlice slice;
        slice.bounds = frame->rect;
        slice.windowOffset = frame->windowOffset;
        slice.commands = frame->currentVisuals;
        slice.effects = frame->currentEffects;
        slice.background = {frame->background.r,
                            frame->background.g,
                            frame->background.b,
                            frame->background.a};
        slice.targetLayer = frame->targetLayer;
        parentProxy.activeCompositeFrame_->slices.push_back(std::move(slice));
    }

    ViewRenderTarget::ViewRenderTarget(Native::NativeItemPtr _native) : native(std::move(_native)){};
    Native::NativeItemPtr ViewRenderTarget::getNativePtr(){ return native;};
    ViewRenderTarget::~ViewRenderTarget(){};

}
