#include "omegaWTK/Widgets/BasicWidgets.h"
#include "omegaWTK/UI/View.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace OmegaWTK {

namespace {

#if defined(TARGET_MACOS)
constexpr float kMaxContainerDimension = 8192.f;
#else
constexpr float kMaxContainerDimension = 16384.f;
#endif

static inline bool rectChanged(const Core::Rect &a,const Core::Rect &b){
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

static inline bool suspiciousBounds(const Core::Rect & rect){
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

static inline Core::Rect sanitizeHostBounds(const Core::Rect & candidate){
    Core::Rect out = candidate;
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

static inline Core::Rect contentBoundsFromHost(const Core::Rect &host,const ContainerClampPolicy &policy){
    auto insets = sanitizeInsets(policy.contentInsets);
    Core::Rect content {
        Core::Position{host.pos.x + insets.left,host.pos.y + insets.top},
        host.w - insets.left - insets.right,
        host.h - insets.top - insets.bottom
    };
    content.pos.x = safeFloat(content.pos.x,host.pos.x);
    content.pos.y = safeFloat(content.pos.y,host.pos.y);
    const float minW = policy.enforceMinSize ? std::max(1.f,safeFloat(policy.minWidth,1.f)) : 0.f;
    const float minH = policy.enforceMinSize ? std::max(1.f,safeFloat(policy.minHeight,1.f)) : 0.f;
    content.w = std::clamp(safeFloat(content.w,minW),minW,kMaxContainerDimension);
    content.h = std::clamp(safeFloat(content.h,minH),minH,kMaxContainerDimension);
    return content;
}

static inline float clampAxisPosition(float pos,float minPos,float maxPos){
    if(maxPos < minPos){
        maxPos = minPos;
    }
    return std::clamp(pos,minPos,maxPos);
}

}

SharedHandle<Widget> Container::Create(const Core::Rect & rect,WidgetPtr parent,...){
    return WIDGET_CREATE<Container>(rect,parent);
}

Container::Container(const Core::Rect & rect,WidgetPtr parent):
Widget(rect,parent){

}

void Container::setClampPolicy(const ContainerClampPolicy & policy){
    clampPolicy = policy;
    if(clampPolicy.enforceMinSize){
        clampPolicy.minWidth = std::max(1.f,safeFloat(clampPolicy.minWidth,1.f));
        clampPolicy.minHeight = std::max(1.f,safeFloat(clampPolicy.minHeight,1.f));
    }
    relayout();
}

const ContainerClampPolicy & Container::getClampPolicy() const{
    return clampPolicy;
}

void Container::onThemeSet(Native::ThemeDesc & desc){
    (void)desc;
}

void Container::onMount(){
    relayout();
}

void Container::onPaint(PaintContext & context,PaintReason reason){
    (void)context;
    (void)reason;
    if(layoutPending){
        layoutChildren();
    }
}

void Container::resize(Core::Rect & newRect){
    (void)newRect;
    relayout();
}

bool Container::acceptsChildWidget(const Widget *child) const{
    if(!Widget::acceptsChildWidget(child)){
        return false;
    }
    for(auto * existing : containerChildren){
        if(existing == child){
            return false;
        }
    }
    return true;
}

void Container::onChildAttached(Widget *child){
    if(child == nullptr){
        return;
    }
    if(std::find(containerChildren.begin(),containerChildren.end(),child) == containerChildren.end()){
        containerChildren.push_back(child);
    }
    relayout();
}

void Container::onChildDetached(Widget *child){
    if(child == nullptr){
        return;
    }
    auto it = std::find(containerChildren.begin(),containerChildren.end(),child);
    if(it != containerChildren.end()){
        containerChildren.erase(it);
    }
    relayout();
}

Core::Rect Container::clampChildRect(const Widget & child,const GeometryProposal & proposal) const{
    (void)child;
    auto clamped = proposal.requested;
    clamped.pos.x = safeFloat(clamped.pos.x,0.f);
    clamped.pos.y = safeFloat(clamped.pos.y,0.f);
    clamped.w = safeFloat(clamped.w,clampPolicy.enforceMinSize ? std::max(1.f,clampPolicy.minWidth) : 0.f);
    clamped.h = safeFloat(clamped.h,clampPolicy.enforceMinSize ? std::max(1.f,clampPolicy.minHeight) : 0.f);

    const float minW = clampPolicy.enforceMinSize ? std::max(1.f,safeFloat(clampPolicy.minWidth,1.f)) : 0.f;
    const float minH = clampPolicy.enforceMinSize ? std::max(1.f,safeFloat(clampPolicy.minHeight,1.f)) : 0.f;

    clamped.w = std::clamp(clamped.w,minW,kMaxContainerDimension);
    clamped.h = std::clamp(clamped.h,minH,kMaxContainerDimension);

    auto hostBounds = sanitizeHostBounds(rootView->getRect());
    auto contentBounds = contentBoundsFromHost(hostBounds,clampPolicy);

    if(!suspiciousBounds(contentBounds)){
        lastStableContentBounds = contentBounds;
        hasLastStableContentBounds = true;
    }
    else if(clampPolicy.keepLastStableBoundsOnInvalidResize && hasLastStableContentBounds){
        contentBounds = lastStableContentBounds;
    }

    if(clampPolicy.clampSizeToBounds){
        if(clampPolicy.horizontalOverflow == ContainerOverflowMode::Clamp){
            clamped.w = std::min(clamped.w,contentBounds.w);
        }
        if(clampPolicy.verticalOverflow == ContainerOverflowMode::Clamp){
            clamped.h = std::min(clamped.h,contentBounds.h);
        }
    }

    if(clampPolicy.clampPositionToBounds){
        if(clampPolicy.horizontalOverflow == ContainerOverflowMode::Clamp){
            const float minX = contentBounds.pos.x;
            const float maxX = contentBounds.pos.x + contentBounds.w - clamped.w;
            clamped.pos.x = clampAxisPosition(clamped.pos.x,minX,maxX);
        }
        if(clampPolicy.verticalOverflow == ContainerOverflowMode::Clamp){
            const float minY = contentBounds.pos.y;
            const float maxY = contentBounds.pos.y + contentBounds.h - clamped.h;
            clamped.pos.y = clampAxisPosition(clamped.pos.y,minY,maxY);
        }
    }

    return clamped;
}

void Container::onChildRectCommitted(const Widget & child,
                                     const Core::Rect & oldRect,
                                     const Core::Rect & newRect,
                                     GeometryChangeReason reason){
    (void)child;
    (void)oldRect;
    (void)newRect;
    if(reason == GeometryChangeReason::ParentLayout || inLayout){
        return;
    }
    relayout();
}

std::size_t Container::childCount() const{
    return containerChildren.size();
}

Widget *Container::childAt(std::size_t idx) const{
    if(idx >= containerChildren.size()){
        return nullptr;
    }
    return containerChildren[idx];
}

WidgetPtr Container::addChild(const WidgetPtr & child){
    if(child == nullptr || child.get() == this){
        return nullptr;
    }
    if(std::find(containerChildren.begin(),containerChildren.end(),child.get()) != containerChildren.end()){
        return child;
    }

    child->setParentWidget(this);
    if(std::find(containerChildren.begin(),containerChildren.end(),child.get()) != containerChildren.end()){
        return child;
    }
    return nullptr;
}

bool Container::removeChild(const WidgetPtr & child){
    if(child == nullptr){
        return false;
    }
    auto it = std::find(containerChildren.begin(),containerChildren.end(),child.get());
    if(it == containerChildren.end()){
        return false;
    }
    child->detachFromParent();
    return true;
}

void Container::relayout(){
    layoutPending = true;
    layoutChildren();
}

void Container::layoutChildren(){
    if(inLayout){
        return;
    }
    inLayout = true;

    for(auto it = containerChildren.begin();it != containerChildren.end();){
        if(*it == nullptr){
            it = containerChildren.erase(it);
        }
        else {
            ++it;
        }
    }

    for(auto * child : containerChildren){
        if(child == nullptr){
            continue;
        }
        auto currentRect = child->rect();
        GeometryProposal proposal {};
        proposal.requested = currentRect;
        proposal.reason = GeometryChangeReason::ParentLayout;
        auto clampedRect = clampChildRect(*child,proposal);
        if(rectChanged(currentRect,clampedRect)){
            child->setRect(clampedRect);
        }
    }

    layoutPending = false;
    inLayout = false;
}

Container::~Container(){
    containerChildren.clear();
}

}
