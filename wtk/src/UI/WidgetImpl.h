#ifndef OMEGAWTK_UI_WIDGETIMPL_H
#define OMEGAWTK_UI_WIDGETIMPL_H

#include "omegaWTK/UI/Widget.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace OmegaWTK {

namespace WidgetInternal {

inline const char * geometryReasonLabel(GeometryChangeReason reason){
    switch(reason){
        case GeometryChangeReason::ParentLayout:
            return "ParentLayout";
        case GeometryChangeReason::ChildRequest:
            return "ChildRequest";
        case GeometryChangeReason::UserInput:
            return "UserInput";
    }
    return "Unknown";
}

inline bool geometryTraceEnvEnabled(){
    static int cached = -1;
    if(cached >= 0){
        return cached == 1;
    }
    auto envVar = OmegaCommon::getEnvVar("OMEGAWTK_GEOMETRY_TRACE");
    if(!envVar.has_value()){
        cached = 0;
        return false;
    }
    const char * envValue = envVar->c_str();
    const auto equalsIgnoreCase = [](const char *lhs,const char *rhs) -> bool {
        if(lhs == nullptr || rhs == nullptr){
            return false;
        }
        while(*lhs != '\0' && *rhs != '\0'){
            if(std::tolower(static_cast<unsigned char>(*lhs)) !=
               std::tolower(static_cast<unsigned char>(*rhs))){
                return false;
            }
            ++lhs;
            ++rhs;
        }
        return *lhs == '\0' && *rhs == '\0';
    };
    if(std::strcmp(envValue,"0") == 0 ||
       equalsIgnoreCase(envValue,"false") ||
       equalsIgnoreCase(envValue,"off") ||
       equalsIgnoreCase(envValue,"no")){
        cached = 0;
        return false;
    }
    cached = 1;
    return true;
}

inline void geometryTraceLog(const char * phase,
                             const Widget * widget,
                             const Widget * parent,
                             GeometryChangeReason reason,
                             const Composition::Rect & lhs,
                             const Composition::Rect & rhs,
                             const Widget::GeometryTraceContext & syncCtx){
    if(!geometryTraceEnvEnabled()){
        return;
    }
    std::fprintf(stderr,
                 "[OmegaWTKGeometry] phase=%s lane=%llu packet=%llu widget=%p parent=%p reason=%s lhs={x:%.3f y:%.3f w:%.3f h:%.3f} rhs={x:%.3f y:%.3f w:%.3f h:%.3f}\n",
                 phase,
                 static_cast<unsigned long long>(syncCtx.syncLaneId),
                 static_cast<unsigned long long>(syncCtx.predictedPacketId),
                 static_cast<const void *>(widget),
                 static_cast<const void *>(parent),
                 geometryReasonLabel(reason),
                 lhs.pos.x,lhs.pos.y,lhs.w,lhs.h,
                 rhs.pos.x,rhs.pos.y,rhs.w,rhs.h);
}

}

struct Widget::Impl {
    bool initialDrawComplete = false;
    bool hasMounted = false;
    // Widget-View-Paint-Lifecycle-Plan Tier D / D1 (2026-06-03):
    // the legacy reentrancy/coalesce/deferred-reason fields are
    // gone — `executePaint` and `flushPendingPaint` were the only
    // readers, and both are deleted. The Tier-A deferred path
    // (`invalidate()` → `requestFrame()` → `paintDirty()`) is now
    // stateless on the Widget side; dirty bits live entirely on the
    // backing View.
    PaintMode mode = PaintMode::Automatic;
    PaintOptions options {};

    LayoutStyle layoutStyle_ {};
    LayoutBehaviorPtr layoutBehavior_ = nullptr;
    bool hasExplicitLayoutStyle_ = false;

    OmegaCommon::Vector<WidgetObserverPtr> observers;

    /// Last view size (w,h) seen by the `onLayoutResolved` rebuild hook
    /// (Widget.Core.cpp). The hook re-authors content (`resize()`) only
    /// when the box changes SIZE — a stretch-to-fill / clamp — not when the
    /// layout merely repositions the widget (element geometry is authored
    /// view-local and is offset-invariant, so a move needs no re-author).
    /// -1 = uninitialised (no resolve seen yet).
    float lastResolvedW = -1.f;
    float lastResolvedH = -1.f;

    /// Native-API §2.3a T1: per-widget tooltip text. Empty = no tooltip
    /// (the hover dispatcher in `WidgetTreeHost` skips widgets with an
    /// empty string). Set via `Widget::setTooltip`, cleared via
    /// `clearTooltip`. The dispatcher reads it after resolving a hovered
    /// View back to its owning Widget through `View::ownerWidget()`.
    OmegaCommon::String tooltip_ {};
};

}

#endif
