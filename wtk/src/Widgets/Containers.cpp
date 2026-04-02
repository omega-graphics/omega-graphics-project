#include "omegaWTK/Widgets/Containers.h"
#include "omegaWTK/UI/View.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OmegaWTK {

namespace {

#if defined(TARGET_MACOS)
constexpr float kMaxStackDimension = 8192.f;
#else
constexpr float kMaxStackDimension = 16384.f;
#endif

static inline float clampSize(float value,const Core::Optional<float> & minValue,const Core::Optional<float> & maxValue){
    if(minValue){
        value = std::max(value,*minValue);
    }
    if(maxValue){
        value = std::min(value,*maxValue);
    }
    return value;
}

static inline bool rectChanged(const Core::Rect &a,const Core::Rect &b){
    constexpr float kEpsilon = 0.001f;
    return std::fabs(a.pos.x - b.pos.x) > kEpsilon ||
           std::fabs(a.pos.y - b.pos.y) > kEpsilon ||
           std::fabs(a.w - b.w) > kEpsilon ||
           std::fabs(a.h - b.h) > kEpsilon;
}

static inline bool finiteRect(const Core::Rect & rect){
    return std::isfinite(rect.pos.x) &&
           std::isfinite(rect.pos.y) &&
           std::isfinite(rect.w) &&
           std::isfinite(rect.h);
}

static inline bool suspiciousFrame(const Core::Rect & rect){
    if(!finiteRect(rect)){
        return true;
    }
    if(rect.w <= 0.f || rect.h <= 0.f){
        return true;
    }
    const float maxDim = std::max(rect.w,rect.h);
    const float minDim = std::min(rect.w,rect.h);
    if(maxDim >= (kMaxStackDimension * 0.5f) && minDim <= 2.f){
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

static inline bool suspiciousSizePair(float mainSize,float crossSize){
    if(!std::isfinite(mainSize) || !std::isfinite(crossSize)){
        return true;
    }
    if(mainSize <= 0.f || crossSize <= 0.f){
        return true;
    }
    const float maxDim = std::max(mainSize,crossSize);
    const float minDim = std::min(mainSize,crossSize);
    if(maxDim >= (kMaxStackDimension * 0.5f) && minDim <= 2.f){
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

static inline bool tinyPlaceholderSize(float mainSize,float crossSize){
    return std::isfinite(mainSize) &&
           std::isfinite(crossSize) &&
           mainSize <= 1.5f &&
           crossSize <= 1.5f;
}

static inline Core::Rect sanitizeStackFrame(const Core::Rect & candidate){
    Core::Rect frame = candidate;
    if(!std::isfinite(frame.pos.x)){
        frame.pos.x = 0.f;
    }
    if(!std::isfinite(frame.pos.y)){
        frame.pos.y = 0.f;
    }
    if(!std::isfinite(frame.w)){
        frame.w = 1.f;
    }
    if(!std::isfinite(frame.h)){
        frame.h = 1.f;
    }
    frame.w = std::clamp(frame.w,1.f,kMaxStackDimension);
    frame.h = std::clamp(frame.h,1.f,kMaxStackDimension);
    return frame;
}

struct LayoutItem {
    Widget *widget = nullptr;
    StackSlot slot {};
    bool resizable = true;
    float currentMain = 0.f;
    float currentCross = 0.f;
    float resolvedMain = 0.f;
    float resolvedCross = 0.f;
    float minMain = 0.f;
    float marginMainBefore = 0.f;
    float marginMainAfter = 0.f;
    float marginCrossBefore = 0.f;
    float marginCrossAfter = 0.f;
};

}

StackWidget::StackWidget(StackAxis axis,Core::Rect rect,const StackOptions & options):
Container(rect),
axis(axis),
stackOptions(options){

}

StackWidget::StackWidget(StackAxis axis,ViewPtr view,const StackOptions & options):
Container(std::move(view)),
axis(axis),
stackOptions(options){

}

void StackWidget::onMount(){
    relayout();
}

void StackWidget::onPaint(PaintReason reason){
    Container::onPaint(reason);
    if(needsLayout){
        layoutChildren();
    }
}

void StackWidget::resize(Core::Rect & newRect){
    (void)newRect;
    relayout();
}

StackAxis StackWidget::getAxis() const{
    return axis;
}

const StackOptions & StackWidget::getOptions() const{
    return stackOptions;
}

void StackWidget::setOptions(const StackOptions & options){
    this->stackOptions = options;
    relayout();
}

WidgetPtr StackWidget::addChild(const WidgetPtr & child){
    return addChild(child,{});
}

WidgetPtr StackWidget::addChild(const WidgetPtr & child,const StackSlot & slot){
    if(child == nullptr || child.get() == this){
        return nullptr;
    }

    for(std::size_t i = 0; i < children.size(); ++i){
        if(children[i] == child.get()){
            if(i < childSlots.size()){
                childSlots[i] = slot;
            }
            relayout();
            return child;
        }
    }

    wireChild(child.get());
    auto childRect = child->rect();
    const float preferredMain = axis == StackAxis::Horizontal ? childRect.w : childRect.h;
    const float preferredCross = axis == StackAxis::Horizontal ? childRect.h : childRect.w;
    const bool hasPreferred =
            !suspiciousSizePair(preferredMain,preferredCross) &&
            !tinyPlaceholderSize(preferredMain,preferredCross);
    childSlots.push_back(slot);
    childSizeCache.push_back({preferredMain,preferredCross,hasPreferred});
    relayout();
    return child;
}

bool StackWidget::removeChild(const WidgetPtr & child){
    if(child == nullptr){
        return false;
    }
    for(std::size_t i = 0; i < children.size(); ++i){
        if(children[i] == child.get()){
            unwireChild(child.get());
            if(i < childSlots.size()){
                childSlots.erase(childSlots.begin() + static_cast<std::ptrdiff_t>(i));
            }
            if(i < childSizeCache.size()){
                childSizeCache.erase(childSizeCache.begin() + static_cast<std::ptrdiff_t>(i));
            }
            relayout();
            return true;
        }
    }
    return false;
}

bool StackWidget::setSlot(const WidgetPtr & child,const StackSlot & slot){
    if(child == nullptr){
        return false;
    }
    for(std::size_t i = 0; i < children.size(); ++i){
        if(children[i] == child.get()){
            if(i < childSlots.size()){
                childSlots[i] = slot;
            }
            relayout();
            return true;
        }
    }
    return false;
}

bool StackWidget::setSlot(std::size_t idx,const StackSlot & slot){
    if(idx >= childSlots.size()){
        return false;
    }
    childSlots[idx] = slot;
    relayout();
    return true;
}

Core::Optional<StackSlot> StackWidget::getSlot(const WidgetPtr & child) const{
    if(child == nullptr){
        return {};
    }
    for(std::size_t i = 0; i < children.size(); ++i){
        if(children[i] == child.get() && i < childSlots.size()){
            return childSlots[i];
        }
    }
    return {};
}

void StackWidget::relayout(){
    needsLayout = true;
    layoutChildren();
}

void StackWidget::layoutChildren(){
    if(inLayout){
        return;
    }

    inLayout = true;

    for(std::size_t i = 0; i < children.size(); ){
        if(children[i] == nullptr){
            children.erase(children.begin() + static_cast<std::ptrdiff_t>(i));
            if(i < childSlots.size()) childSlots.erase(childSlots.begin() + static_cast<std::ptrdiff_t>(i));
            if(i < childSizeCache.size()) childSizeCache.erase(childSizeCache.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else {
            ++i;
        }
    }

    const auto count = children.size();
    if(count == 0){
        needsLayout = false;
        inLayout = false;
        return;
    }

    auto frameCandidate = sanitizeStackFrame(rect());
    if(suspiciousFrame(frameCandidate)){
        if(!hasLastStableFrame){
            needsLayout = true;
            inLayout = false;
            return;
        }
        frameCandidate = lastStableFrame;
    }
    else {
        hasLastStableFrame = true;
        lastStableFrame = frameCandidate;
    }

    auto frame = frameCandidate;
    const float contentMain = std::max(
        0.f,
        (axis == StackAxis::Horizontal ? frame.w : frame.h) -
        (axis == StackAxis::Horizontal
            ? (stackOptions.padding.left + stackOptions.padding.right)
            : (stackOptions.padding.top + stackOptions.padding.bottom)));
    const float contentCross = std::max(
        0.f,
        (axis == StackAxis::Horizontal ? frame.h : frame.w) -
        (axis == StackAxis::Horizontal
            ? (stackOptions.padding.top + stackOptions.padding.bottom)
            : (stackOptions.padding.left + stackOptions.padding.right)));
    Core::Rect contentBoundsRect {
            Core::Position{
                    frame.pos.x + stackOptions.padding.left,
                    frame.pos.y + stackOptions.padding.top
            },
            std::max(0.f,frame.w - stackOptions.padding.left - stackOptions.padding.right),
            std::max(0.f,frame.h - stackOptions.padding.top - stackOptions.padding.bottom)
    };

    const float mainStart = (axis == StackAxis::Horizontal) ? stackOptions.padding.left : stackOptions.padding.top;
    const float crossStart = (axis == StackAxis::Horizontal) ? stackOptions.padding.top : stackOptions.padding.left;

    OmegaCommon::Vector<LayoutItem> items;
    items.reserve(count);

    const auto slotCount = childSlots.size();
    const auto cacheCount = childSizeCache.size();

    for(std::size_t idx = 0; idx < count; ++idx){
        auto *child = children[idx];
        if(child == nullptr){
            continue;
        }

        const auto & slot = idx < slotCount ? childSlots[idx] : StackSlot{};
        StackChildCache defaultCache {};
        auto & cache = idx < cacheCount ? childSizeCache[idx] : defaultCache;
        const bool hasCache = idx < cacheCount;

        auto childRect = child->rect();
        float currentMain = axis == StackAxis::Horizontal ? childRect.w : childRect.h;
        float currentCross = axis == StackAxis::Horizontal ? childRect.h : childRect.w;

        const bool currentSuspicious = suspiciousSizePair(currentMain,currentCross);
        const bool currentPlaceholder = tinyPlaceholderSize(currentMain,currentCross);
        const bool hasPreferred =
                hasCache && cache.hasPreferredSize &&
                !suspiciousSizePair(cache.preferredMainSize,cache.preferredCrossSize) &&
                !tinyPlaceholderSize(cache.preferredMainSize,cache.preferredCrossSize);

        if(!currentSuspicious && !currentPlaceholder && hasCache){
            cache.preferredMainSize = currentMain;
            cache.preferredCrossSize = currentCross;
            cache.hasPreferredSize = true;
        }
        else if(hasPreferred){
            currentMain = cache.preferredMainSize;
            currentCross = cache.preferredCrossSize;
        }

        currentMain = std::clamp(
                std::isfinite(currentMain) ? currentMain : 1.f,
                1.f,
                kMaxStackDimension);
        currentCross = std::clamp(
                std::isfinite(currentCross) ? currentCross : 1.f,
                1.f,
                kMaxStackDimension);

        LayoutItem item {};
        item.widget = child;
        item.slot = slot;
        item.resizable = child->isLayoutResizable();
        item.currentMain = currentMain;
        item.currentCross = currentCross;

        item.marginMainBefore = axis == StackAxis::Horizontal ? slot.margin.left : slot.margin.top;
        item.marginMainAfter = axis == StackAxis::Horizontal ? slot.margin.right : slot.margin.bottom;
        item.marginCrossBefore = axis == StackAxis::Horizontal ? slot.margin.top : slot.margin.left;
        item.marginCrossAfter = axis == StackAxis::Horizontal ? slot.margin.bottom : slot.margin.right;

        item.resolvedMain = item.currentMain;
        item.resolvedCross = item.currentCross;

        if(item.resizable){
            const bool lockMainToPreferred =
                    slot.flexGrow <= 0.f &&
                    slot.flexShrink <= 0.f &&
                    hasCache && cache.hasPreferredSize;

            if(slot.basis){
                item.resolvedMain = *slot.basis;
            }
            else if(lockMainToPreferred){
                item.resolvedMain = cache.preferredMainSize;
            }
            item.resolvedMain = clampSize(item.resolvedMain,slot.minMain,slot.maxMain);

            if((!std::isfinite(item.resolvedCross) || item.resolvedCross <= 1.f) && hasCache && cache.hasPreferredSize){
                item.resolvedCross = cache.preferredCrossSize;
            }
            item.resolvedCross = clampSize(item.resolvedCross,slot.minCross,slot.maxCross);
        }

        item.minMain = slot.minMain.value_or(0.f);
        items.push_back(item);
    }

    if(items.empty()){
        needsLayout = false;
        inLayout = false;
        return;
    }

    float usedMain = stackOptions.spacing * static_cast<float>(items.size() > 0 ? items.size() - 1 : 0);
    float totalGrow = 0.f;
    float totalShrinkWeight = 0.f;

    for(const auto & item : items){
        usedMain += item.marginMainBefore + item.resolvedMain + item.marginMainAfter;
        if(item.resizable){
            totalGrow += std::max(item.slot.flexGrow,0.f);
            totalShrinkWeight += std::max(item.slot.flexShrink,0.f) * std::max(item.resolvedMain,0.f);
        }
    }

    float freeMain = contentMain - usedMain;
    if(freeMain > 0.f && totalGrow > 0.f){
        for(auto & item : items){
            if(!item.resizable || item.slot.flexGrow <= 0.f){
                continue;
            }
            float delta = freeMain * (item.slot.flexGrow / totalGrow);
            item.resolvedMain += delta;
            item.resolvedMain = clampSize(item.resolvedMain,item.slot.minMain,item.slot.maxMain);
        }
    }
    else if(freeMain < 0.f && totalShrinkWeight > 0.f){
        float overflow = -freeMain;
        for(auto & item : items){
            if(!item.resizable || item.slot.flexShrink <= 0.f){
                continue;
            }
            float weight = std::max(item.slot.flexShrink,0.f) * std::max(item.resolvedMain,0.f);
            if(weight <= 0.f){
                continue;
            }
            float shrink = overflow * (weight / totalShrinkWeight);
            item.resolvedMain = std::max(item.minMain,item.resolvedMain - shrink);
            item.resolvedMain = clampSize(item.resolvedMain,item.slot.minMain,item.slot.maxMain);
        }
    }

    usedMain = stackOptions.spacing * static_cast<float>(items.size() > 0 ? items.size() - 1 : 0);
    for(const auto & item : items){
        usedMain += item.marginMainBefore + item.resolvedMain + item.marginMainAfter;
    }

    float extraMain = std::max(0.f,contentMain - usedMain);
    float layoutSpacing = stackOptions.spacing;
    float startOffset = 0.f;

    switch(stackOptions.mainAlign){
        case StackMainAlign::Start:
            break;
        case StackMainAlign::Center:
            startOffset = extraMain * 0.5f;
            break;
        case StackMainAlign::End:
            startOffset = extraMain;
            break;
        case StackMainAlign::SpaceBetween:
            if(items.size() > 1){
                layoutSpacing += extraMain / static_cast<float>(items.size() - 1);
            }
            break;
        case StackMainAlign::SpaceAround:
            if(!items.empty()){
                float gap = extraMain / static_cast<float>(items.size());
                layoutSpacing += gap;
                startOffset = gap * 0.5f;
            }
            break;
        case StackMainAlign::SpaceEvenly:
            if(!items.empty()){
                float gap = extraMain / static_cast<float>(items.size() + 1);
                layoutSpacing += gap;
                startOffset = gap;
            }
            break;
    }

    float cursor = mainStart + startOffset;
    OmegaCommon::Vector<Widget *> resizedWidgets {};
    resizedWidgets.reserve(items.size());

    for(auto & item : items){
        auto childRect = item.widget->rect();
        float mainSize = item.resizable ? item.resolvedMain : item.currentMain;

        cursor += item.marginMainBefore;

        auto align = item.slot.alignSelf.value_or(stackOptions.crossAlign);
        float crossSize = item.resizable ? item.resolvedCross : item.currentCross;

        float crossAvailable = std::max(0.f,contentCross - (item.marginCrossBefore + item.marginCrossAfter));
        if(item.resizable){
            crossSize = std::max(0.f,crossSize);
            crossSize = std::min(crossSize,crossAvailable);
        }

        float crossPos = crossStart + item.marginCrossBefore;
        switch(align){
            case StackCrossAlign::Start:
                break;
            case StackCrossAlign::Center:
                crossPos += std::max(0.f,(crossAvailable - crossSize) * 0.5f);
                break;
            case StackCrossAlign::End:
                crossPos += std::max(0.f,crossAvailable - crossSize);
                break;
            case StackCrossAlign::Stretch:
                if(item.resizable){
                    crossSize = crossAvailable;
                }
                break;
        }

        Core::Rect targetRect = childRect;
        if(axis == StackAxis::Horizontal){
            targetRect.pos.x = cursor;
            targetRect.pos.y = crossPos;
            targetRect.w = mainSize;
            targetRect.h = item.resizable ? crossSize : childRect.h;
        }
        else {
            targetRect.pos.x = crossPos;
            targetRect.pos.y = cursor;
            targetRect.w = item.resizable ? crossSize : childRect.w;
            targetRect.h = mainSize;
        }

        ChildResizeSpec resizeSpec {};
        resizeSpec.resizable = item.resizable;
        resizeSpec.policy = item.resizable ? ChildResizePolicy::FitContent : ChildResizePolicy::Fixed;
        if(axis == StackAxis::Horizontal){
            resizeSpec.clamp.minWidth = std::max(1.f,item.slot.minMain.value_or(1.f));
            resizeSpec.clamp.minHeight = std::max(1.f,item.slot.minCross.value_or(1.f));
            resizeSpec.clamp.maxWidth = item.slot.maxMain.value_or(std::numeric_limits<float>::infinity());
            resizeSpec.clamp.maxHeight = item.slot.maxCross.value_or(std::numeric_limits<float>::infinity());
        }
        else {
            resizeSpec.clamp.minWidth = std::max(1.f,item.slot.minCross.value_or(1.f));
            resizeSpec.clamp.minHeight = std::max(1.f,item.slot.minMain.value_or(1.f));
            resizeSpec.clamp.maxWidth = item.slot.maxCross.value_or(std::numeric_limits<float>::infinity());
            resizeSpec.clamp.maxHeight = item.slot.maxMain.value_or(std::numeric_limits<float>::infinity());
        }
        targetRect = ViewResizeCoordinator::clampRectToParent(targetRect,contentBoundsRect,resizeSpec);

        if(rectChanged(childRect,targetRect)){
            auto prevOpts = item.widget->paintOptions();
            const bool suppressResizeInvalidate = prevOpts.invalidateOnResize;
            if(suppressResizeInvalidate){
                auto suppressed = prevOpts;
                suppressed.invalidateOnResize = false;
                item.widget->setPaintOptions(suppressed);
            }

            item.widget->setRect(targetRect);

            if(suppressResizeInvalidate){
                item.widget->setPaintOptions(prevOpts);
                if(std::find(resizedWidgets.begin(),resizedWidgets.end(),item.widget) == resizedWidgets.end()){
                    resizedWidgets.push_back(item.widget);
                }
            }
        }

        cursor += mainSize + item.marginMainAfter + layoutSpacing;
    }

    for(auto * resized : resizedWidgets){
        if(resized != nullptr){
            resized->invalidate(PaintReason::Resize);
        }
    }

    needsLayout = false;
    inLayout = false;
}

StackWidget::~StackWidget(){
    childSlots.clear();
    childSizeCache.clear();
}

HStack::HStack(Core::Rect rect,const StackOptions & options):
StackWidget(StackAxis::Horizontal,rect,options){

}

HStack::HStack(ViewPtr view,const StackOptions & options):
StackWidget(StackAxis::Horizontal,std::move(view),options){

}

VStack::VStack(Core::Rect rect,const StackOptions & options):
StackWidget(StackAxis::Vertical,rect,options){

}

VStack::VStack(ViewPtr view,const StackOptions & options):
StackWidget(StackAxis::Vertical,std::move(view),options){

}

}
