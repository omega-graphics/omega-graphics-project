#include "omegaWTK/UI/LayoutManager.h"
#include "ViewImpl.h"   // ViewInternal::sanitizeRect / clampAxis / kMaxViewDimension / sameRect

#include <algorithm>
#include <cmath>
#include <limits>

namespace OmegaWTK {

namespace {

// Local helpers identical to the BasicWidgets.cpp safeFloat /
// suspiciousBounds / contentBoundsFromHost — moved here because the
// Container clamp policy moved here. The originals stay in
// BasicWidgets.cpp until 4.5 deletes `Container::layoutChildren`.
constexpr float kMaxContainerDimension = ViewInternal::kMaxViewDimension;

inline float safeFloat(float value, float fallback){
    return std::isfinite(value) ? value : fallback;
}

inline bool suspiciousBounds(const Composition::Rect & r){
    if(!std::isfinite(r.pos.x) || !std::isfinite(r.pos.y) ||
       !std::isfinite(r.w)     || !std::isfinite(r.h))      return true;
    if(r.w <= 0.f || r.h <= 0.f) return true;
    return false;
}

inline ContainerInsets sanitizeInsets(const ContainerInsets & insets){
    ContainerInsets out = insets;
    out.left   = safeFloat(out.left,   0.f);
    out.top    = safeFloat(out.top,    0.f);
    out.right  = safeFloat(out.right,  0.f);
    out.bottom = safeFloat(out.bottom, 0.f);
    return out;
}

inline Composition::Rect sanitizeHostBounds(const Composition::Rect & rect){
    Composition::Rect out = rect;
    out.pos.x = safeFloat(out.pos.x, 0.f);
    out.pos.y = safeFloat(out.pos.y, 0.f);
    out.w     = std::clamp(safeFloat(out.w, 1.f), 1.f, kMaxContainerDimension);
    out.h     = std::clamp(safeFloat(out.h, 1.f), 1.f, kMaxContainerDimension);
    return out;
}

inline Composition::Rect contentBoundsFromHost(const Composition::Rect & host,
                                                const ContainerClampPolicy & policy){
    auto insets = sanitizeInsets(policy.contentInsets);
    Composition::Rect content {
        Composition::Point2D{host.pos.x + insets.left, host.pos.y + insets.top},
        host.w - insets.left - insets.right,
        host.h - insets.top  - insets.bottom
    };
    content.pos.x = safeFloat(content.pos.x, host.pos.x);
    content.pos.y = safeFloat(content.pos.y, host.pos.y);
    const float minW = policy.enforceMinSize ? std::max(1.f, safeFloat(policy.minWidth,  1.f)) : 0.f;
    const float minH = policy.enforceMinSize ? std::max(1.f, safeFloat(policy.minHeight, 1.f)) : 0.f;
    content.w = std::clamp(safeFloat(content.w, minW), minW, kMaxContainerDimension);
    content.h = std::clamp(safeFloat(content.h, minH), minH, kMaxContainerDimension);
    return content;
}

inline float clampAxisPosition(float pos, float minPos, float maxPos){
    if(maxPos < minPos){
        maxPos = minPos;
    }
    return std::clamp(pos, minPos, maxPos);
}

} // namespace

// ---------------------------------------------------------------------------
// LayoutManager::clampRectToParent — lifted verbatim from the deleted
// `ViewResizeCoordinator::clampRectToParent`. Static, used by every
// 4.5 manager + by `UIView::paint` (intra-element clamp) + by
// `StackWidget::layoutChildren` (until Phase 4.6 replaces it).
// ---------------------------------------------------------------------------

Composition::Rect LayoutManager::clampRectToParent(const Composition::Rect & requested,
                                                    const Composition::Rect & parentContentRect,
                                                    const ChildResizeSpec & spec){
    auto fallback = ViewInternal::sanitizeRect(parentContentRect,
        Composition::Rect{Composition::Point2D{0.f, 0.f}, 1.f, 1.f});
    auto parent   = ViewInternal::sanitizeRect(parentContentRect, fallback);
    auto output   = ViewInternal::sanitizeRect(requested, parent);

    const float minWidth  = std::max(1.f, std::isfinite(spec.clamp.minWidth)  ? spec.clamp.minWidth  : 1.f);
    const float minHeight = std::max(1.f, std::isfinite(spec.clamp.minHeight) ? spec.clamp.minHeight : 1.f);
    const float maxWidth  = std::isfinite(spec.clamp.maxWidth)  ? spec.clamp.maxWidth  : ViewInternal::kMaxViewDimension;
    const float maxHeight = std::isfinite(spec.clamp.maxHeight) ? spec.clamp.maxHeight : ViewInternal::kMaxViewDimension;

    if(spec.resizable){
        output.w = ViewInternal::clampAxis(output.w, minWidth,  std::max(minWidth,  maxWidth));
        output.h = ViewInternal::clampAxis(output.h, minHeight, std::max(minHeight, maxHeight));
    }
    else {
        output.w = std::max(1.f, output.w);
        output.h = std::max(1.f, output.h);
    }

    output.w = std::min(output.w, std::max(1.f, parent.w));
    output.h = std::min(output.h, std::max(1.f, parent.h));

    const float minX = parent.pos.x;
    const float minY = parent.pos.y;
    const float maxX = parent.pos.x + std::max(0.f, parent.w - output.w);
    const float maxY = parent.pos.y + std::max(0.f, parent.h - output.h);
    output.pos.x = ViewInternal::clampAxis(output.pos.x, minX, maxX);
    output.pos.y = ViewInternal::clampAxis(output.pos.y, minY, maxY);
    return output;
}

// ---------------------------------------------------------------------------
// AbsoluteLayout — process-wide singleton; default if no manager set.
// ---------------------------------------------------------------------------

AbsoluteLayout & AbsoluteLayout::instance(){
    static AbsoluteLayout sInstance;
    return sInstance;
}

LayoutSize AbsoluteLayout::measure(View & /*node*/, const Composition::Rect & avail){
    return {avail.w, avail.h};
}

void AbsoluteLayout::arrange(View & node, const Composition::Rect & nodeLocalRect){
    // Default semantic: each child keeps its own rect, optionally
    // clamped to the parent's content area via FitContent semantics
    // (the pre-migration default `ChildResizeSpec`).
    //
    // Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
    // `nodeLocalRect` is guaranteed origin-(0,0) by the walker
    // (`FrameBuilder::layoutSubtree` derives it from
    // `nodeRectInParent`'s dimensions before dispatch). Pre-D8 this
    // manager carried a workaround that built its own local-origin
    // clamp box — that workaround retired with D8.6's walker-level
    // hardening, so the incoming rect can be used directly.
    ChildResizeSpec spec {};
    spec.policy = ChildResizePolicy::FitContent;
    for(auto * child : node.subviews()){
        if(child == nullptr){
            continue;
        }
        auto requested = child->getRect();
        auto clamped   = clampRectToParent(requested, nodeLocalRect, spec);
        if(!ViewInternal::sameRect(child->getRect(), clamped)){
            child->resize(clamped);
        }
    }
}

// ---------------------------------------------------------------------------
// FillLayout — every child stretched to the parent content rect.
// ---------------------------------------------------------------------------

LayoutSize FillLayout::measure(View & /*node*/, const Composition::Rect & avail){
    return {avail.w, avail.h};
}

void FillLayout::arrange(View & node, const Composition::Rect & nodeLocalRect){
    // Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
    // walker hands us a local-origin rect (`nodeLocalRect.pos ==
    // {0,0}`). Pre-D8 we rebuilt one with a `max(1, dim)` guard for
    // empty parents; the guard moves inline here on the dimension
    // axis.
    const Composition::Rect childRect {
        nodeLocalRect.pos,
        std::max(1.f, nodeLocalRect.w),
        std::max(1.f, nodeLocalRect.h)
    };
    for(auto * child : node.subviews()){
        if(child == nullptr){
            continue;
        }
        if(!ViewInternal::sameRect(child->getRect(), childRect)){
            child->resize(childRect);
        }
    }
}

// ---------------------------------------------------------------------------
// StackLayout — H/V no-flex sequential placement.
// ---------------------------------------------------------------------------

StackLayout::StackLayout(LayoutAxis axis, float spacing):
    axis_(axis),
    spacing_(std::max(0.f, std::isfinite(spacing) ? spacing : 0.f)) {}

LayoutSize StackLayout::measure(View & /*node*/, const Composition::Rect & avail){
    return {avail.w, avail.h};
}

void StackLayout::arrange(View & node, const Composition::Rect & nodeLocalRect){
    // Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
    // `nodeLocalRect.pos == {0,0}` post-D8.6 (walker guarantees a
    // local-origin rect). Pre-D8 this manager seeded the sequential
    // cursor from `finalRectLocal.pos.x/y` directly — when that was
    // parent-space, the first child landed at the parent's offset
    // and downstream layouts double-counted.
    const auto subs = node.subviews();
    if(subs.size() == 0){
        return;
    }

    ChildResizeSpec spec {};
    spec.policy = ChildResizePolicy::FitContent;

    float cursor = (axis_ == LayoutAxis::Horizontal)
        ? nodeLocalRect.pos.x : nodeLocalRect.pos.y;

    for(auto * child : subs){
        if(child == nullptr){
            continue;
        }
        const auto current = child->getRect();
        Composition::Rect target = current;
        if(axis_ == LayoutAxis::Horizontal){
            target.pos.x = cursor;
            target.pos.y = nodeLocalRect.pos.y;
        }
        else {
            target.pos.x = nodeLocalRect.pos.x;
            target.pos.y = cursor;
        }
        target = clampRectToParent(target, nodeLocalRect, spec);
        if(!ViewInternal::sameRect(child->getRect(), target)){
            child->resize(target);
        }
        if(axis_ == LayoutAxis::Horizontal){
            cursor = target.pos.x + target.w + spacing_;
        }
        else {
            cursor = target.pos.y + target.h + spacing_;
        }
    }
}

// ---------------------------------------------------------------------------
// ContainerLayout — the lifted Container::clampChildRect, plus an
// arrange() that applies the policy to every child uniformly.
// ---------------------------------------------------------------------------

ContainerLayout::ContainerLayout(const ContainerClampPolicy & policy){
    setPolicy(policy);
}

void ContainerLayout::setPolicy(const ContainerClampPolicy & policy){
    policy_ = policy;
    if(policy_.enforceMinSize){
        policy_.minWidth  = std::max(1.f, safeFloat(policy_.minWidth,  1.f));
        policy_.minHeight = std::max(1.f, safeFloat(policy_.minHeight, 1.f));
    }
}

LayoutSize ContainerLayout::measure(View & /*node*/, const Composition::Rect & avail){
    return {avail.w, avail.h};
}

Composition::Rect ContainerLayout::clampChild(const Composition::Rect & requested,
                                               const Composition::Rect & hostRect) const{
    auto clamped = requested;
    clamped.pos.x = safeFloat(clamped.pos.x, 0.f);
    clamped.pos.y = safeFloat(clamped.pos.y, 0.f);
    clamped.w     = safeFloat(clamped.w, policy_.enforceMinSize ? std::max(1.f, policy_.minWidth)  : 0.f);
    clamped.h     = safeFloat(clamped.h, policy_.enforceMinSize ? std::max(1.f, policy_.minHeight) : 0.f);

    const float minW = policy_.enforceMinSize ? std::max(1.f, safeFloat(policy_.minWidth,  1.f)) : 0.f;
    const float minH = policy_.enforceMinSize ? std::max(1.f, safeFloat(policy_.minHeight, 1.f)) : 0.f;

    clamped.w = std::clamp(clamped.w, minW, kMaxContainerDimension);
    clamped.h = std::clamp(clamped.h, minH, kMaxContainerDimension);

    auto hostBounds    = sanitizeHostBounds(hostRect);
    auto contentBounds = contentBoundsFromHost(hostBounds, policy_);

    if(!suspiciousBounds(contentBounds)){
        lastStableContentBounds_    = contentBounds;
        hasLastStableContentBounds_ = true;
    }
    else if(policy_.keepLastStableBoundsOnInvalidResize && hasLastStableContentBounds_){
        contentBounds = lastStableContentBounds_;
    }

    if(policy_.clampSizeToBounds){
        if(policy_.horizontalOverflow == ContainerOverflowMode::Clamp){
            clamped.w = std::min(clamped.w, contentBounds.w);
        }
        if(policy_.verticalOverflow == ContainerOverflowMode::Clamp){
            clamped.h = std::min(clamped.h, contentBounds.h);
        }
    }

    if(policy_.clampPositionToBounds){
        if(policy_.horizontalOverflow == ContainerOverflowMode::Clamp){
            const float minX = contentBounds.pos.x;
            const float maxX = contentBounds.pos.x + contentBounds.w - clamped.w;
            clamped.pos.x = clampAxisPosition(clamped.pos.x, minX, maxX);
        }
        if(policy_.verticalOverflow == ContainerOverflowMode::Clamp){
            const float minY = contentBounds.pos.y;
            const float maxY = contentBounds.pos.y + contentBounds.h - clamped.h;
            clamped.pos.y = clampAxisPosition(clamped.pos.y, minY, maxY);
        }
    }

    return clamped;
}

void ContainerLayout::arrange(View & node, const Composition::Rect & finalRectLocal){
    for(auto * child : node.subviews()){
        if(child == nullptr){
            continue;
        }
        auto clamped = clampChild(child->getRect(), finalRectLocal);
        if(!ViewInternal::sameRect(child->getRect(), clamped)){
            child->resize(clamped);
        }
    }
}

// ---------------------------------------------------------------------------
// FlexLayout — Phase 4.6. The main-axis distribution + cross-axis alignment
// algorithm lifted from `StackWidget::layoutChildren()`. Per-child state
// lives on the manager (keyed by `View *`); the algorithm is identical to
// the pre-migration StackWidget path including the "suspicious / placeholder
// rect" cache fall-back. Owners (StackWidget, etc.) call `setChildSpec` on
// addChild and `removeChildSpec` on removeChild; both invalidate the
// cached preferred size for the affected child so the next arrange picks
// up the new spec without staleness.
// ---------------------------------------------------------------------------

namespace {

#if defined(TARGET_MACOS)
constexpr float kMaxFlexDimension = 8192.f;
#else
constexpr float kMaxFlexDimension = 16384.f;
#endif

inline float clampSize(float value,
                       const Core::Optional<float> & minValue,
                       const Core::Optional<float> & maxValue){
    if(minValue){
        value = std::max(value, *minValue);
    }
    if(maxValue){
        value = std::min(value, *maxValue);
    }
    return value;
}

inline bool finiteRectAll(const Composition::Rect & rect){
    return std::isfinite(rect.pos.x) &&
           std::isfinite(rect.pos.y) &&
           std::isfinite(rect.w)     &&
           std::isfinite(rect.h);
}

inline bool suspiciousFlexFrame(const Composition::Rect & rect){
    if(!finiteRectAll(rect)){
        return true;
    }
    if(rect.w <= 0.f || rect.h <= 0.f){
        return true;
    }
    const float maxDim = std::max(rect.w, rect.h);
    const float minDim = std::min(rect.w, rect.h);
    if(maxDim >= (kMaxFlexDimension * 0.5f) && minDim <= 2.f){
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

inline bool suspiciousFlexSize(float mainSize, float crossSize){
    if(!std::isfinite(mainSize) || !std::isfinite(crossSize)){
        return true;
    }
    if(mainSize <= 0.f || crossSize <= 0.f){
        return true;
    }
    const float maxDim = std::max(mainSize, crossSize);
    const float minDim = std::min(mainSize, crossSize);
    if(maxDim >= (kMaxFlexDimension * 0.5f) && minDim <= 2.f){
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

inline bool tinyFlexPlaceholder(float mainSize, float crossSize){
    return std::isfinite(mainSize) &&
           std::isfinite(crossSize) &&
           mainSize  <= 1.5f &&
           crossSize <= 1.5f;
}

inline Composition::Rect sanitizeFlexFrame(const Composition::Rect & candidate){
    Composition::Rect frame = candidate;
    if(!std::isfinite(frame.pos.x)) frame.pos.x = 0.f;
    if(!std::isfinite(frame.pos.y)) frame.pos.y = 0.f;
    if(!std::isfinite(frame.w))     frame.w     = 1.f;
    if(!std::isfinite(frame.h))     frame.h     = 1.f;
    frame.w = std::clamp(frame.w, 1.f, kMaxFlexDimension);
    frame.h = std::clamp(frame.h, 1.f, kMaxFlexDimension);
    return frame;
}

// Per-child plan built each pass. Mirrors the LayoutItem the pre-
// migration StackWidget code used; left in the unnamed namespace so the
// FlexLayout method bodies read like the original algorithm.
struct FlexItem {
    View *               view              = nullptr;
    FlexChildSpec        spec              {};
    float                currentMain       = 0.f;
    float                currentCross      = 0.f;
    float                resolvedMain      = 0.f;
    float                resolvedCross     = 0.f;
    float                minMain           = 0.f;
    float                marginMainBefore  = 0.f;
    float                marginMainAfter   = 0.f;
    float                marginCrossBefore = 0.f;
    float                marginCrossAfter  = 0.f;
};

} // namespace

FlexLayout::FlexLayout(const FlexOptions & options): options_(options) {}

void FlexLayout::setOptions(const FlexOptions & options){
    options_ = options;
    // Options change the desired-size math (basis lock, etc.), so the
    // cached preferred sizes can no longer be trusted.
    for(auto & kv : entries_){
        kv.second.hasPreferredSize = false;
    }
}

void FlexLayout::setChildSpec(View * child, const FlexChildSpec & spec){
    if(child == nullptr){
        return;
    }
    auto & entry = entries_[child];
    entry.spec             = spec;
    entry.hasPreferredSize = false;
}

void FlexLayout::removeChildSpec(View * child){
    if(child == nullptr){
        return;
    }
    entries_.erase(child);
}

FlexChildSpec FlexLayout::childSpec(View * child) const{
    auto it = entries_.find(child);
    if(it == entries_.end()){
        return {};
    }
    return it->second.spec;
}

LayoutSize FlexLayout::measure(View & node, const Composition::Rect & avail){
    // Bottom-up: collect each child's preferred main / cross size into
    // the per-child cache. Phase 4.6 leaves the cache populated; 4.7
    // will invalidate it via DirtyBit::Layout. The aggregate returned
    // is the parent's desired size — sum of preferred main + spacing +
    // padding on the main axis, max of preferred cross + padding on
    // the cross axis. The avail rect bounds the cross-axis aggregate
    // (stretch children cannot ask for more than `avail`).
    const auto subs = node.subviews();
    if(subs.size() == 0){
        return {avail.w, avail.h};
    }

    const bool horizontal = (options_.axis == LayoutAxis::Horizontal);

    float mainSum  = 0.f;
    float crossMax = 0.f;
    std::size_t count = 0;

    for(auto * child : subs){
        if(child == nullptr){
            continue;
        }
        auto & entry = entries_[child];
        const auto & spec = entry.spec;

        auto childRect = child->getRect();
        float curMain  = horizontal ? childRect.w : childRect.h;
        float curCross = horizontal ? childRect.h : childRect.w;

        const bool curSuspicious  = suspiciousFlexSize(curMain, curCross);
        const bool curPlaceholder = tinyFlexPlaceholder(curMain, curCross);
        const bool cacheUsable    = entry.hasPreferredSize &&
                                    !suspiciousFlexSize(entry.preferredMain,
                                                        entry.preferredCross) &&
                                    !tinyFlexPlaceholder(entry.preferredMain,
                                                         entry.preferredCross);

        if(!curSuspicious && !curPlaceholder){
            entry.preferredMain    = curMain;
            entry.preferredCross   = curCross;
            entry.hasPreferredSize = true;
        }
        else if(cacheUsable){
            curMain  = entry.preferredMain;
            curCross = entry.preferredCross;
        }

        curMain  = std::clamp(std::isfinite(curMain)  ? curMain  : 1.f,
                              1.f, kMaxFlexDimension);
        curCross = std::clamp(std::isfinite(curCross) ? curCross : 1.f,
                              1.f, kMaxFlexDimension);

        // Apply basis / lock-to-preferred + spec min/max so the
        // aggregate reflects the same numbers arrange() will use.
        float resolvedMain  = curMain;
        float resolvedCross = curCross;
        if(spec.resizable){
            const bool lockMainToPreferred =
                    spec.flexGrow   <= 0.f &&
                    spec.flexShrink <= 0.f &&
                    entry.hasPreferredSize;
            if(spec.basis){
                resolvedMain = *spec.basis;
            }
            else if(lockMainToPreferred){
                resolvedMain = entry.preferredMain;
            }
            resolvedMain  = clampSize(resolvedMain,  spec.minMain,  spec.maxMain);
            resolvedCross = clampSize(resolvedCross, spec.minCross, spec.maxCross);
        }

        const float marginMain  = (horizontal ? spec.margin.left + spec.margin.right
                                              : spec.margin.top  + spec.margin.bottom);
        const float marginCross = (horizontal ? spec.margin.top  + spec.margin.bottom
                                              : spec.margin.left + spec.margin.right);
        mainSum += marginMain + resolvedMain;
        crossMax = std::max(crossMax, marginCross + resolvedCross);
        ++count;
    }

    if(count > 1){
        mainSum += options_.spacing * static_cast<float>(count - 1);
    }
    const float padMain  = horizontal
        ? (options_.padding.left + options_.padding.right)
        : (options_.padding.top  + options_.padding.bottom);
    const float padCross = horizontal
        ? (options_.padding.top  + options_.padding.bottom)
        : (options_.padding.left + options_.padding.right);

    LayoutSize desired {};
    if(horizontal){
        desired.w = mainSum + padMain;
        desired.h = crossMax + padCross;
    }
    else {
        desired.w = crossMax + padCross;
        desired.h = mainSum + padMain;
    }

    // The available rect bounds the desired size (a parent should not
    // ask its children to extend past the rect they were given).
    desired.w = std::min(desired.w, std::max(1.f, avail.w));
    desired.h = std::min(desired.h, std::max(1.f, avail.h));
    return desired;
}

void FlexLayout::arrange(View & node, const Composition::Rect & finalRectLocal){
    const auto subs = node.subviews();
    if(subs.size() == 0){
        return;
    }

    // Refresh the preferred-size cache before distributing free space.
    // 4.7 will hoist this to a top-down FrameBuilder pass; 4.6 keeps
    // measure + arrange paired so existing entry points (resize,
    // relayout) continue to drive a complete pass.
    measure(node, finalRectLocal);

    auto frameCandidate = sanitizeFlexFrame(finalRectLocal);
    if(suspiciousFlexFrame(frameCandidate)){
        if(!hasLastStableFrame_){
            // No prior stable frame — leave children alone, a future
            // resize with a clean rect will retry.
            return;
        }
        frameCandidate = lastStableFrame_;
    }
    else {
        hasLastStableFrame_ = true;
        lastStableFrame_    = frameCandidate;
    }

    const bool  horizontal  = (options_.axis == LayoutAxis::Horizontal);
    const auto & frame      = frameCandidate;

    const float contentMain = std::max(
        0.f,
        (horizontal ? frame.w : frame.h) -
        (horizontal ? (options_.padding.left + options_.padding.right)
                    : (options_.padding.top  + options_.padding.bottom)));
    const float contentCross = std::max(
        0.f,
        (horizontal ? frame.h : frame.w) -
        (horizontal ? (options_.padding.top  + options_.padding.bottom)
                    : (options_.padding.left + options_.padding.right)));

    // Children are positioned in this node's own (parent-relative)
    // space: rects are written directly from cursor / crossPos and
    // clamped against bounds living in the SAME origin-0 space. This
    // mirrors the fix recorded in StackWidget::layoutChildren — using
    // frame.pos here would shift every child of a non-origin (nested)
    // node by the node's own offset.
    Composition::Rect contentBoundsRect {
        Composition::Point2D{options_.padding.left, options_.padding.top},
        std::max(0.f, frame.w - options_.padding.left - options_.padding.right),
        std::max(0.f, frame.h - options_.padding.top  - options_.padding.bottom)
    };

    const float mainStart  = horizontal ? options_.padding.left : options_.padding.top;
    const float crossStart = horizontal ? options_.padding.top  : options_.padding.left;

    OmegaCommon::Vector<FlexItem> items;
    items.reserve(subs.size());

    for(auto * child : subs){
        if(child == nullptr){
            continue;
        }
        auto entryIt = entries_.find(child);
        const bool hasEntry = (entryIt != entries_.end());
        const FlexChildSpec & spec = hasEntry ? entryIt->second.spec : FlexChildSpec{};

        float curMain  = 0.f;
        float curCross = 0.f;
        if(hasEntry && entryIt->second.hasPreferredSize){
            curMain  = entryIt->second.preferredMain;
            curCross = entryIt->second.preferredCross;
        }
        else {
            auto childRect = child->getRect();
            curMain  = horizontal ? childRect.w : childRect.h;
            curCross = horizontal ? childRect.h : childRect.w;
            curMain  = std::clamp(std::isfinite(curMain)  ? curMain  : 1.f,
                                  1.f, kMaxFlexDimension);
            curCross = std::clamp(std::isfinite(curCross) ? curCross : 1.f,
                                  1.f, kMaxFlexDimension);
        }

        FlexItem item {};
        item.view         = child;
        item.spec         = spec;
        item.currentMain  = curMain;
        item.currentCross = curCross;

        item.marginMainBefore  = horizontal ? spec.margin.left : spec.margin.top;
        item.marginMainAfter   = horizontal ? spec.margin.right : spec.margin.bottom;
        item.marginCrossBefore = horizontal ? spec.margin.top  : spec.margin.left;
        item.marginCrossAfter  = horizontal ? spec.margin.bottom : spec.margin.right;

        item.resolvedMain  = curMain;
        item.resolvedCross = curCross;

        if(spec.resizable){
            const bool lockMainToPreferred =
                    spec.flexGrow   <= 0.f &&
                    spec.flexShrink <= 0.f &&
                    hasEntry && entryIt->second.hasPreferredSize;
            if(spec.basis){
                item.resolvedMain = *spec.basis;
            }
            else if(lockMainToPreferred){
                item.resolvedMain = entryIt->second.preferredMain;
            }
            item.resolvedMain = clampSize(item.resolvedMain, spec.minMain, spec.maxMain);

            if((!std::isfinite(item.resolvedCross) || item.resolvedCross <= 1.f) &&
               hasEntry && entryIt->second.hasPreferredSize){
                item.resolvedCross = entryIt->second.preferredCross;
            }
            item.resolvedCross = clampSize(item.resolvedCross, spec.minCross, spec.maxCross);
        }

        item.minMain = spec.minMain.value_or(0.f);
        items.push_back(item);
    }

    if(items.empty()){
        return;
    }

    // Main-axis distribution: grow when free space is positive, shrink
    // when it is negative. The shrink weight is `flexShrink *
    // currentMain` (CSS-style — bigger items absorb more of the
    // overflow).
    float usedMain = options_.spacing *
            static_cast<float>(items.size() > 0 ? items.size() - 1 : 0);
    float totalGrow         = 0.f;
    float totalShrinkWeight = 0.f;
    for(const auto & item : items){
        usedMain += item.marginMainBefore + item.resolvedMain + item.marginMainAfter;
        if(item.spec.resizable){
            totalGrow         += std::max(item.spec.flexGrow,   0.f);
            totalShrinkWeight += std::max(item.spec.flexShrink, 0.f) *
                                 std::max(item.resolvedMain,     0.f);
        }
    }

    float freeMain = contentMain - usedMain;
    if(freeMain > 0.f && totalGrow > 0.f){
        for(auto & item : items){
            if(!item.spec.resizable || item.spec.flexGrow <= 0.f){
                continue;
            }
            const float delta = freeMain * (item.spec.flexGrow / totalGrow);
            item.resolvedMain += delta;
            item.resolvedMain  = clampSize(item.resolvedMain,
                                           item.spec.minMain,
                                           item.spec.maxMain);
        }
    }
    else if(freeMain < 0.f && totalShrinkWeight > 0.f){
        const float overflow = -freeMain;
        for(auto & item : items){
            if(!item.spec.resizable || item.spec.flexShrink <= 0.f){
                continue;
            }
            const float weight = std::max(item.spec.flexShrink, 0.f) *
                                 std::max(item.resolvedMain,     0.f);
            if(weight <= 0.f){
                continue;
            }
            const float shrink = overflow * (weight / totalShrinkWeight);
            item.resolvedMain = std::max(item.minMain, item.resolvedMain - shrink);
            item.resolvedMain = clampSize(item.resolvedMain,
                                          item.spec.minMain,
                                          item.spec.maxMain);
        }
    }

    // Recompute usedMain after distribution; the leftover (extraMain)
    // feeds the main-axis alignment policy.
    usedMain = options_.spacing *
            static_cast<float>(items.size() > 0 ? items.size() - 1 : 0);
    for(const auto & item : items){
        usedMain += item.marginMainBefore + item.resolvedMain + item.marginMainAfter;
    }

    float extraMain     = std::max(0.f, contentMain - usedMain);
    float layoutSpacing = options_.spacing;
    float startOffset   = 0.f;

    switch(options_.mainAlign){
        case FlexMainAlign::Start:
            break;
        case FlexMainAlign::Center:
            startOffset = extraMain * 0.5f;
            break;
        case FlexMainAlign::End:
            startOffset = extraMain;
            break;
        case FlexMainAlign::SpaceBetween:
            if(items.size() > 1){
                layoutSpacing += extraMain / static_cast<float>(items.size() - 1);
            }
            break;
        case FlexMainAlign::SpaceAround:
            if(!items.empty()){
                const float gap = extraMain / static_cast<float>(items.size());
                layoutSpacing += gap;
                startOffset    = gap * 0.5f;
            }
            break;
        case FlexMainAlign::SpaceEvenly:
            if(!items.empty()){
                const float gap = extraMain / static_cast<float>(items.size() + 1);
                layoutSpacing += gap;
                startOffset    = gap;
            }
            break;
    }

    float cursor = mainStart + startOffset;
    for(auto & item : items){
        auto childRect = item.view->getRect();
        const float mainSize = item.spec.resizable ? item.resolvedMain : item.currentMain;

        cursor += item.marginMainBefore;

        const auto align  = item.spec.alignSelf.value_or(options_.crossAlign);
        float       crossSize = item.spec.resizable ? item.resolvedCross : item.currentCross;
        const float crossAvailable = std::max(
            0.f,
            contentCross - (item.marginCrossBefore + item.marginCrossAfter));
        if(item.spec.resizable){
            crossSize = std::max(0.f, crossSize);
            crossSize = std::min(crossSize, crossAvailable);
        }

        float crossPos = crossStart + item.marginCrossBefore;
        switch(align){
            case FlexCrossAlign::Start:
                break;
            case FlexCrossAlign::Center:
                crossPos += std::max(0.f, (crossAvailable - crossSize) * 0.5f);
                break;
            case FlexCrossAlign::End:
                crossPos += std::max(0.f, crossAvailable - crossSize);
                break;
            case FlexCrossAlign::Stretch:
                if(item.spec.resizable){
                    crossSize = crossAvailable;
                }
                break;
        }

        Composition::Rect targetRect = childRect;
        if(horizontal){
            targetRect.pos.x = cursor;
            targetRect.pos.y = crossPos;
            targetRect.w     = mainSize;
            targetRect.h     = item.spec.resizable ? crossSize : childRect.h;
        }
        else {
            targetRect.pos.x = crossPos;
            targetRect.pos.y = cursor;
            targetRect.w     = item.spec.resizable ? crossSize : childRect.w;
            targetRect.h     = mainSize;
        }

        ChildResizeSpec resizeSpec {};
        resizeSpec.resizable = item.spec.resizable;
        resizeSpec.policy    = item.spec.resizable
            ? ChildResizePolicy::FitContent
            : ChildResizePolicy::Fixed;
        if(horizontal){
            resizeSpec.clamp.minWidth  = std::max(1.f, item.spec.minMain.value_or(1.f));
            resizeSpec.clamp.minHeight = std::max(1.f, item.spec.minCross.value_or(1.f));
            resizeSpec.clamp.maxWidth  = item.spec.maxMain.value_or(
                    std::numeric_limits<float>::infinity());
            resizeSpec.clamp.maxHeight = item.spec.maxCross.value_or(
                    std::numeric_limits<float>::infinity());
        }
        else {
            resizeSpec.clamp.minWidth  = std::max(1.f, item.spec.minCross.value_or(1.f));
            resizeSpec.clamp.minHeight = std::max(1.f, item.spec.minMain.value_or(1.f));
            resizeSpec.clamp.maxWidth  = item.spec.maxCross.value_or(
                    std::numeric_limits<float>::infinity());
            resizeSpec.clamp.maxHeight = item.spec.maxMain.value_or(
                    std::numeric_limits<float>::infinity());
        }
        targetRect = LayoutManager::clampRectToParent(targetRect,
                                                      contentBoundsRect,
                                                      resizeSpec);

        if(!ViewInternal::sameRect(item.view->getRect(), targetRect)){
            item.view->resize(targetRect);
        }

        cursor += mainSize + item.marginMainAfter + layoutSpacing;
    }
}

} // namespace OmegaWTK
