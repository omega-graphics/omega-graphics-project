#ifndef OMEGAWTK_UI_VIEWIMPL_H
#define OMEGAWTK_UI_VIEWIMPL_H

#include "omegaWTK/UI/View.h"
#include "../Composition/backend/ResourceFactory.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace OmegaWTK {

namespace ViewInternal {

#if defined(TARGET_MACOS)
constexpr float kMaxViewDimension = 8192.f;
#else
constexpr float kMaxViewDimension = 16384.f;
#endif

inline bool finiteFloat(float value){
    return std::isfinite(value);
}

inline bool suspiciousDimensionPair(float w,float h){
    if(!finiteFloat(w) || !finiteFloat(h) || w <= 0.f || h <= 0.f){
        return true;
    }
    const float maxDim = std::max(w,h);
    const float minDim = std::min(w,h);
    if(maxDim >= (kMaxViewDimension * 0.5f) && minDim <= 2.f){
        return true;
    }
    if(maxDim >= 1024.f && minDim > 0.f){
        const float aspect = maxDim / minDim;
        if(aspect > 256.f){
            return true;
        }
    }
    return false;
}

inline Core::Rect sanitizeRect(const Core::Rect & candidate,const Core::Rect & fallback){
    Core::Rect saneFallback = fallback;
    if(!finiteFloat(saneFallback.pos.x)){
        saneFallback.pos.x = 0.f;
    }
    if(!finiteFloat(saneFallback.pos.y)){
        saneFallback.pos.y = 0.f;
    }
    if(!finiteFloat(saneFallback.w) || saneFallback.w <= 0.f){
        saneFallback.w = 1.f;
    }
    if(!finiteFloat(saneFallback.h) || saneFallback.h <= 0.f){
        saneFallback.h = 1.f;
    }
    saneFallback.w = std::clamp(saneFallback.w,1.f,kMaxViewDimension);
    saneFallback.h = std::clamp(saneFallback.h,1.f,kMaxViewDimension);
    if(suspiciousDimensionPair(saneFallback.w,saneFallback.h)){
        saneFallback.w = 1.f;
        saneFallback.h = 1.f;
    }

    Core::Rect sanitized = candidate;
    if(!finiteFloat(sanitized.pos.x)){
        sanitized.pos.x = saneFallback.pos.x;
    }
    if(!finiteFloat(sanitized.pos.y)){
        sanitized.pos.y = saneFallback.pos.y;
    }
    if(!finiteFloat(sanitized.w) || sanitized.w <= 0.f){
        sanitized.w = saneFallback.w;
    }
    if(!finiteFloat(sanitized.h) || sanitized.h <= 0.f){
        sanitized.h = saneFallback.h;
    }
    sanitized.w = std::clamp(sanitized.w,1.f,kMaxViewDimension);
    sanitized.h = std::clamp(sanitized.h,1.f,kMaxViewDimension);
    if(suspiciousDimensionPair(sanitized.w,sanitized.h)){
        sanitized.w = saneFallback.w;
        sanitized.h = saneFallback.h;
    }
    return sanitized;
}

inline bool sameRect(const Core::Rect & a,const Core::Rect & b){
    constexpr float kEpsilon = 0.001f;
    return std::fabs(a.pos.x - b.pos.x) <= kEpsilon &&
           std::fabs(a.pos.y - b.pos.y) <= kEpsilon &&
           std::fabs(a.w - b.w) <= kEpsilon &&
           std::fabs(a.h - b.h) <= kEpsilon;
}

inline float clampAxis(float value,float minValue,float maxValue){
    if(maxValue < minValue){
        maxValue = minValue;
    }
    return std::clamp(value,minValue,maxValue);
}

}

struct View::Impl {
    OmegaCommon::Vector<View *> subviews;
    /// Shared render target propagated from the window (Phase 3).
    /// Null until setWindowRenderTarget() is called. Not owned per-View.
    SharedHandle<Composition::ViewRenderTarget> renderTarget;
    Composition::CompositorClientProxy proxy;
    ViewResizeCoordinator resizeCoordinator;
    SharedHandle<Composition::LayerTree> ownLayerTree;
    View * parent_ptr = nullptr;
    Core::Rect rect {Core::Position{0.f,0.f},1.f,1.f};
    ViewDelegate * delegate = nullptr;
    bool enabled_ = true;

    /// Construct a purely virtual View (Phase 3). No NativeItem, no
    /// per-View render target. The render target is propagated from the
    /// window via setWindowRenderTarget().
    Impl(View & owner,
         const Core::Rect & initialRect,
         View * parent):
        renderTarget(nullptr),
        proxy(),
        ownLayerTree(std::make_shared<Composition::LayerTree>(initialRect)),
        parent_ptr(parent),
        rect(initialRect){
        resizeCoordinator.attachView(&owner);
    }

};

}

#endif
