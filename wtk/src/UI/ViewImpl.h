#ifndef OMEGAWTK_UI_VIEWIMPL_H
#define OMEGAWTK_UI_VIEWIMPL_H

#include "omegaWTK/UI/View.h"
#include "../Composition/backend/ResourceFactory.h"
#include "AnimationScheduler.h"   // Phase 4.4: allocateNodeId()

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_set>
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

inline Composition::Rect sanitizeRect(const Composition::Rect & candidate,const Composition::Rect & fallback){
    Composition::Rect saneFallback = fallback;
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

    Composition::Rect sanitized = candidate;
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

inline bool sameRect(const Composition::Rect & a,const Composition::Rect & b){
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
    // Phase 4.5: `ViewResizeCoordinator` is deleted. Child layout now
    // routes through `View::layoutManager()` (see `LayoutManager.h`).
    // Phase 4.8: `ownLayerTree` deleted — the window-level
    // `AppWindow::Impl::windowLayerTree_` is the single tree the
    // post-4.7 paint pipeline targets.
    View * parent_ptr = nullptr;
    Composition::Rect rect {Composition::Point2D{0.f,0.f},1.f,1.f};
    ViewDelegate * delegate = nullptr;
    bool enabled_ = true;
    /// Widget-View-Paint-Lifecycle-Plan Tier D / D6.4 (2026-06-03):
    /// pseudo-class state bitmask. Bit layout matches the
    /// `StyleSheets::PseudoClass` enum in
    /// `omegaWTK/UI/StyleSheet.h` (Hover=1, Pressed=2, Focused=4,
    /// Disabled=8). The input dispatcher in
    /// `WidgetTreeHost::dispatchInputEvent` flips Hover / Pressed
    /// when the hovered View changes or a mouse button transitions;
    /// `setEnabled()` flips Disabled. The resolver consults this via
    /// `View::pseudoClassBits()` during selector match.
    std::uint8_t pseudoClassBits_ = 0;
    /// Widget-View-Paint-Lifecycle-Plan Tier D / D7.4 (2026-06-04):
    /// `:state(name)` custom-state set. Sibling to `pseudoClassBits_`,
    /// but string-keyed and open-ended: widget / app code names states
    /// like `:state(loading)` or `:state(selected)` and flips them via
    /// `View::setState(name, on)`. The resolver subset-matches selector
    /// `customStates` against this set; specificity counts each entry
    /// at one-class weight. Empty set = no custom states are active on
    /// this view. Stored as `unordered_set<std::string>` because the
    /// set is expected to be small (a handful of names) and the only
    /// runtime queries are "is name X present" — O(1) hash lookup
    /// without an order requirement.
    std::unordered_set<OmegaCommon::String> customStates_ {};
    /// Widget-View-Paint-Lifecycle-Plan Tier A: deferred-paint dirty mask.
    /// Per-node bits (Style / Layout / Content / Paint) set by
    /// `markDirty(bits)` on the node that actually changed.
    uint8_t dirtyBits_ = 0;
    /// Phase 4.7.3: propagated mask. Tracks "any *descendant* of this
    /// node carries a dirty bit". OR-set on every ancestor by
    /// `markDirty()` so the root mask is the union of every dirty bit
    /// anywhere in the tree. `FrameBuilder::buildFrame` reads this to
    /// gate per-pass subtree pruning: a subtree whose
    /// `(dirtyBits_ | descendantDirty_) & passBit == 0` is skipped
    /// entirely. Cleared together with `dirtyBits_` by
    /// `View::clearDirtyBits()` (which the frame-end pass walks).
    uint8_t descendantDirty_ = 0;
    /// Phase 4.4: stable per-View NodeId for the AnimationScheduler. Read
    /// via the public `View::nodeId()`. Allocated at construction so
    /// `applyLayoutDelta` can register tweens without a lookup.
    std::uint64_t nodeId_;
    /// UIView-Render-Redesign Phase G.3.0: monotonic generation counter
    /// for the View's painted content. Incremented by `View::markDirty`
    /// whenever the bits include `Paint`. NOT cleared by
    /// `View::clearDirtyBits` — the counter is a generation number, not
    /// a flag, and the G.3 content cache keys against
    /// `(nodeId_, contentVersion_)` so an unchanged View at the same
    /// version hits the cached texture across frames. Starts at 0; first
    /// Paint-dirty mark moves it to 1, so a fresh-never-painted entry
    /// is naturally distinguishable from a cleared-after-paint entry.
    std::uint64_t contentVersion_ = 0;
    /// Phase 4.5: parent-owned child-layout strategy. nullptr = use the
    /// process-wide `AbsoluteLayout::instance()` default. View does NOT
    /// own this pointer — caller is responsible for the manager's
    /// lifetime (which is usually a member of the owning Widget, e.g.
    /// `Container`'s `ContainerLayout` field).
    LayoutManager * layoutManager_ = nullptr;

    /// Construct a purely virtual View (Phase 3). No NativeItem, no
    /// per-View render target. The render target is propagated from the
    /// window via setWindowRenderTarget().
    Impl(View & owner,
         const Composition::Rect & initialRect,
         View * parent):
        renderTarget(nullptr),
        proxy(),
        parent_ptr(parent),
        rect(initialRect),
        nodeId_(allocateNodeId()){
        // Phase 4.5: no per-View resize coordinator to attach. The
        // parent's LayoutManager discovers children through
        // `parent->subviews()` at arrange time.
        (void)owner;
    }

};

}

#endif
