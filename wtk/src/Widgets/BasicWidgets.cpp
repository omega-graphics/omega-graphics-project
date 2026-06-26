#include "omegaWTK/Widgets/BasicWidgets.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/UI/ScrollView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace OmegaWTK {

namespace {

#if defined(TARGET_MACOS)
constexpr float kMaxContainerDimension = 8192.f;
#else
constexpr float kMaxContainerDimension = 16384.f;
#endif

static inline bool rectChanged(const Composition::Rect &a,const Composition::Rect &b){
    constexpr float kEpsilon = 0.001f;
    return std::fabs(a.pos.x - b.pos.x) > kEpsilon ||
           std::fabs(a.pos.y - b.pos.y) > kEpsilon ||
           std::fabs(a.w - b.w) > kEpsilon ||
           std::fabs(a.h - b.h) > kEpsilon;
}

static inline bool finiteFloat(float value){
    return std::isfinite(value);
}

static inline float safeFloat(float value,float fallback){
    return finiteFloat(value) ? value : fallback;
}

static inline bool suspiciousBounds(const Composition::Rect & rect){
    if(!finiteFloat(rect.w) || !finiteFloat(rect.h) || rect.w <= 0.f || rect.h <= 0.f){
        return true;
    }
    const float maxDim = std::max(rect.w,rect.h);
    const float minDim = std::min(rect.w,rect.h);
    if(maxDim >= (kMaxContainerDimension * 0.5f) && minDim <= 1.5f){
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

static inline Composition::Rect sanitizeHostBounds(const Composition::Rect & candidate){
    Composition::Rect out = candidate;
    out.pos.x = safeFloat(out.pos.x,0.f);
    out.pos.y = safeFloat(out.pos.y,0.f);
    out.w = safeFloat(out.w,1.f);
    out.h = safeFloat(out.h,1.f);
    out.w = std::clamp(out.w,1.f,kMaxContainerDimension);
    out.h = std::clamp(out.h,1.f,kMaxContainerDimension);
    return out;
}

static inline ContainerInsets sanitizeInsets(const ContainerInsets &insets){
    ContainerInsets out = insets;
    out.left = safeFloat(out.left,0.f);
    out.top = safeFloat(out.top,0.f);
    out.right = safeFloat(out.right,0.f);
    out.bottom = safeFloat(out.bottom,0.f);
    return out;
}

// Phase 4.5: `contentBoundsFromHost` + `clampAxisPosition` moved into
// `LayoutManager.cpp` alongside `ContainerLayout`. Keep `safeFloat`,
// `sanitizeInsets`, and `sanitizeHostBounds` local — they are used by
// the geometry-trace label code below the Container body.

static inline const char * geometryReasonLabel(GeometryChangeReason reason){
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

}

// SharedHandle<Widget> Container::Create(ViewPtr view){
//     return make<Container>(std::move(view));
// }

Container::Container(Composition::Rect rect):
Widget(rect){
    // Phase 4.5: install the per-container ContainerLayout on the
    // backing View so the parent's LayoutManager owns child layout.
    if(view != nullptr){
        view->setLayoutManager(&containerLayout_);
    }
}

Container::Container(ViewPtr view):
Widget(std::move(view)){
    if(this->view != nullptr){
        this->view->setLayoutManager(&containerLayout_);
    }
}

void Container::setClampPolicy(const ContainerClampPolicy & policy){
    // Phase 4.5: the policy now lives on `ContainerLayout`.
    containerLayout_.setPolicy(policy);
    relayout();
}

const ContainerClampPolicy & Container::getClampPolicy() const{
    return containerLayout_.policy();
}

void Container::setResizeWithParent(bool resizeWithParent){
    if(resizeWithParent_ == resizeWithParent){
        return;
    }
    resizeWithParent_ = resizeWithParent;
    // The flag feeds the parent's FlexLayout via the per-child spec, which
    // is read when the parent next arranges. Re-run this container's own
    // layout and ask the parent to re-arrange so the change takes effect
    // without waiting for an unrelated relayout trigger.
    relayout();
    if(parent != nullptr){
        parent->requestLayout();
    }
}

void Container::onThemeSet(Native::ThemeDesc & desc){
    (void)desc;
}

void Container::onMount(){
    relayout();
}

void Container::resize(Composition::Rect & newRect){
    (void)newRect;
    relayout();
}

void Container::wireChild(const WidgetPtr & child){
    if(child == nullptr){
        return;
    }
    child->parent = this;
    view->addSubView(child->view.get());
    children.push_back(child);
    child->setTreeHostRecurse(treeHost);
    child->notifyObservers(Widget::Attach,{});
}

void Container::unwireChild(const WidgetPtr & child){
    if(child == nullptr){
        return;
    }
    auto it = std::find(children.begin(),children.end(),child);
    if(it != children.end()){
        children.erase(it);
    }
    view->removeSubView(child->view.get());
    child->parent = nullptr;
    child->setTreeHostRecurse(nullptr);
    child->notifyObservers(Widget::Detach,{});
    // Phase 4.8: per-view `LayerTree` is gone — the
    // `notifyObserversOfWidgetDetach` callback existed only so
    // compositor backends could react to per-view tree teardown.
    // The window owns the single tree now and nothing tracks
    // per-widget detach at the layer-tree level.
}

OmegaCommon::ArrayRef<WidgetPtr> Container::childWidgets(){
    return children;
}

Composition::Rect Container::clampChildRect(const Widget & child,const GeometryProposal & proposal) const{
    // Phase 4.5: clamp math moved into `ContainerLayout::clampChild`.
    // This override stays so `Widget::commitGeometry` can still ask the
    // container "what would you clamp this proposal to?"
    auto clamped = containerLayout_.clampChild(proposal.requested,
                                                view != nullptr ? view->getRect()
                                                                : Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});

    if(Widget::geometryTraceLoggingEnabled()){
        auto syncCtx = geometryTraceContext();
        std::fprintf(stderr,
                     "[OmegaWTKGeometry] phase=container-clamp lane=%llu packet=%llu container=%p child=%p reason=%s requested={x:%.3f y:%.3f w:%.3f h:%.3f} clamped={x:%.3f y:%.3f w:%.3f h:%.3f}\n",
                     static_cast<unsigned long long>(syncCtx.syncLaneId),
                     static_cast<unsigned long long>(syncCtx.predictedPacketId),
                     static_cast<const void *>(this),
                     static_cast<const void *>(&child),
                     geometryReasonLabel(proposal.reason),
                     proposal.requested.pos.x,
                     proposal.requested.pos.y,
                     proposal.requested.w,
                     proposal.requested.h,
                     clamped.pos.x,
                     clamped.pos.y,
                     clamped.w,
                     clamped.h);
    }

    return clamped;
}

void Container::onChildRectCommitted(const Widget & child,
                                     const Composition::Rect & oldRect,
                                     const Composition::Rect & newRect,
                                     GeometryChangeReason reason){
    if(Widget::geometryTraceLoggingEnabled()){
        auto syncCtx = geometryTraceContext();
        std::fprintf(stderr,
                     "[OmegaWTKGeometry] phase=container-commit lane=%llu packet=%llu container=%p child=%p reason=%s old={x:%.3f y:%.3f w:%.3f h:%.3f} new={x:%.3f y:%.3f w:%.3f h:%.3f}\n",
                     static_cast<unsigned long long>(syncCtx.syncLaneId),
                     static_cast<unsigned long long>(syncCtx.predictedPacketId),
                     static_cast<const void *>(this),
                     static_cast<const void *>(&child),
                     geometryReasonLabel(reason),
                     oldRect.pos.x,oldRect.pos.y,oldRect.w,oldRect.h,
                     newRect.pos.x,newRect.pos.y,newRect.w,newRect.h);
    }
    if(reason == GeometryChangeReason::ParentLayout){
        return;
    }
    // Phase 4.5: defer the relayout into the next frame instead of
    // running it eagerly inside the child's commit. The dirty mark +
    // requestFrame coalesces multiple sibling commits into one
    // arrange pass.
    relayout();
}

std::size_t Container::childCount() const{
    return children.size();
}

Widget *Container::childAt(std::size_t idx) const{
    if(idx >= children.size()){
        return nullptr;
    }
    return children[idx].get();
}

WidgetPtr Container::addChild(const WidgetPtr & child){
    if(child == nullptr || child.get() == this){
        return nullptr;
    }
    if(std::find(children.begin(),children.end(),child) != children.end()){
        return child;
    }

    wireChild(child);
    relayout();
    return child;
}

bool Container::removeChild(const WidgetPtr & child){
    if(child == nullptr){
        return false;
    }
    auto it = std::find(children.begin(),children.end(),child);
    if(it == children.end()){
        return false;
    }
    unwireChild(child);
    relayout();
    return true;
}

void Container::relayout(){
    // Phase 4.5: drive the parent's `ContainerLayout::arrange` directly.
    // The pre-migration `layoutChildren()` (which manually walked
    // children + clamped each via `clampChildRect`) is gone — the
    // manager owns that walk. Stale-child cleanup happens here so
    // arrange sees a clean view of `subviews()`.
    layoutPending = true;
    for(auto it = children.begin(); it != children.end(); ){
        if(*it == nullptr){
            it = children.erase(it);
        }
        else {
            ++it;
        }
    }
    if(view != nullptr){
        containerLayout_.arrange(*view, view->getRect());
    }
    layoutPending = false;
}

Container::~Container(){
    for(auto & child : children){
        if(child != nullptr){
            child->parent = nullptr;
        }
    }
    children.clear();
}

// --- ScrollableContainer (ScrollableContainer-Implementation-Plan S1) ---

ScrollableContainer::Composite
ScrollableContainer::BuildComposite(const Composition::Rect & rect,
                                    const ScrollableContainerOptions & options){
    Composite composite;
    // The content view lives inside the ScrollView's local coordinate
    // space, so its origin is {0,0}; the ScrollView itself carries the
    // parent-relative position. S1 starts the content extent equal to
    // the viewport (no overflow until the caller sets a larger extent
    // via `setContentSize`).
    Composition::Rect contentRect;
    contentRect.pos = {0.f, 0.f};
    contentRect.w = rect.w;
    contentRect.h = rect.h;
    composite.contentView = View::Create(contentRect);

    composite.scrollView = std::make_shared<ScrollView>(
        rect, composite.contentView,
        options.verticalScroll, options.horizontalScroll);
    return composite;
}

ScrollableContainer::ScrollableContainer(Composition::Rect rect,
                                         const ScrollableContainerOptions & options):
    ScrollableContainer(BuildComposite(rect, options), options){
}

ScrollableContainer::ScrollableContainer(Composite composite,
                                         const ScrollableContainerOptions & options):
    Widget(composite.scrollView),
    options_(options),
    contentView_(composite.contentView),
    contentWidget_(std::make_shared<Container>(composite.contentView)){
    // `contentWidget_` reuses `contentView_` rather than introducing a
    // third View, so its child-clamp bounds are the content extent and
    // its child views nest directly under the ScrollView's scrolled
    // subtree. No relayout needed yet — there are no children.
}

void ScrollableContainer::onThemeSet(Native::ThemeDesc & desc){
    (void)desc;
}

WidgetPtr ScrollableContainer::addChild(const WidgetPtr & child){
    auto added = contentWidget_->addChild(child);
    // `contentWidget_` is a detached helper the framework's host walk
    // never reaches (it is not in `childWidgets()`), so `wireChild`
    // threaded a null `treeHost` into a child added after this widget
    // attached. Re-thread the real host now. (View-level wiring —
    // frontend / sync lane / focus host — is already correct via
    // `View::addSubView`; this fixes only the child Widget's pointer.)
    if(added != nullptr && treeHost != nullptr){
        added->setTreeHostRecurse(treeHost);
    }
    return added;
}

bool ScrollableContainer::removeChild(const WidgetPtr & child){
    return contentWidget_->removeChild(child);
}

OmegaCommon::ArrayRef<WidgetPtr> ScrollableContainer::childWidgets(){
    return contentWidget_->childWidgets();
}

void ScrollableContainer::setContentSize(float w, float h){
    if(contentView_ == nullptr){
        return;
    }
    auto & contentRect = contentView_->getRect();
    contentRect.w = w;
    contentRect.h = h;
    // Re-clamp existing children against the new extent so a content
    // size set after children were added still sees the right bounds.
    if(contentWidget_ != nullptr){
        contentWidget_->relayout();
    }
    // Clamp the current scroll offset to the new maximum — shrinking the
    // extent below the current offset would otherwise leave the viewport
    // scrolled past the end.
    auto & scrollView = viewAs<ScrollView>();
    const Composition::Rect & viewport = scrollView.getRect();
    Composition::Point2D offset = scrollView.getScrollOffset();
    const float maxX = std::max(0.f, w - viewport.w);
    const float maxY = std::max(0.f, h - viewport.h);
    offset.x = std::clamp(offset.x, 0.f, maxX);
    offset.y = std::clamp(offset.y, 0.f, maxY);
    scrollView.setScrollOffset(offset);
    invalidate();
}

Composition::Point2D ScrollableContainer::contentSize() const{
    if(contentView_ == nullptr){
        return {0.f, 0.f};
    }
    const Composition::Rect & contentRect = contentView_->getRect();
    return {contentRect.w, contentRect.h};
}

ScrollableContainer::~ScrollableContainer() = default;

}
