#include "omegaWTK/Widgets/Containers.h"

#include <algorithm>
#include <cmath>

namespace OmegaWTK {

namespace {

static inline float clampSize(float value,const Core::Optional<float> & minValue,const Core::Optional<float> & maxValue){
    if(minValue){
        value = std::max(value,*minValue);
    }
    if(maxValue){
        value = std::min(value,*maxValue);
    }
    return value;
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

StackWidget::StackWidget(StackAxis axis,const Core::Rect & rect,WidgetPtr parent,const StackOptions & options):
Widget(rect,parent),
axis(axis),
options(options){

}

void StackWidget::onMount(){
    relayout();
}

void StackWidget::onPaint(PaintContext & context,PaintReason reason){
    (void)context;
    (void)reason;
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
    return options;
}

void StackWidget::setOptions(const StackOptions & options){
    this->options = options;
    relayout();
}

std::size_t StackWidget::childCount() const{
    return stackChildren.size();
}

WidgetPtr StackWidget::childAt(std::size_t idx) const{
    if(idx >= stackChildren.size()){
        return nullptr;
    }
    return stackChildren[idx].widget;
}

WidgetPtr StackWidget::addChild(const WidgetPtr & child,const StackSlot & slot){
    if(child == nullptr || child.get() == this){
        return nullptr;
    }

    for(auto & entry : stackChildren){
        if(entry.widget == child){
            entry.slot = slot;
            relayout();
            return child;
        }
    }

    child->setParentWidget(this);
    stackChildren.push_back({child,slot});
    relayout();
    return child;
}

bool StackWidget::removeChild(const WidgetPtr & child){
    if(child == nullptr){
        return false;
    }
    for(auto it = stackChildren.begin(); it != stackChildren.end(); ++it){
        if(it->widget == child){
            child->detachFromParent();
            stackChildren.erase(it);
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
    for(auto & entry : stackChildren){
        if(entry.widget == child){
            entry.slot = slot;
            relayout();
            return true;
        }
    }
    return false;
}

bool StackWidget::setSlot(std::size_t idx,const StackSlot & slot){
    if(idx >= stackChildren.size()){
        return false;
    }
    stackChildren[idx].slot = slot;
    relayout();
    return true;
}

Core::Optional<StackSlot> StackWidget::getSlot(const WidgetPtr & child) const{
    if(child == nullptr){
        return {};
    }
    for(const auto & entry : stackChildren){
        if(entry.widget == child){
            return entry.slot;
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

    for(auto it = stackChildren.begin(); it != stackChildren.end(); ){
        if(it->widget == nullptr){
            it = stackChildren.erase(it);
        }
        else {
            ++it;
        }
    }

    const auto count = stackChildren.size();
    if(count == 0){
        needsLayout = false;
        inLayout = false;
        return;
    }

    auto frame = rect();
    const float contentMain = std::max(
        0.f,
        (axis == StackAxis::Horizontal ? frame.w : frame.h) -
        (axis == StackAxis::Horizontal
            ? (options.padding.left + options.padding.right)
            : (options.padding.top + options.padding.bottom)));
    const float contentCross = std::max(
        0.f,
        (axis == StackAxis::Horizontal ? frame.h : frame.w) -
        (axis == StackAxis::Horizontal
            ? (options.padding.top + options.padding.bottom)
            : (options.padding.left + options.padding.right)));

    const float mainStart = (axis == StackAxis::Horizontal) ? options.padding.left : options.padding.top;
    const float crossStart = (axis == StackAxis::Horizontal) ? options.padding.top : options.padding.left;

    OmegaCommon::Vector<LayoutItem> items;
    items.reserve(count);

    for(auto & entry : stackChildren){
        auto *child = entry.widget.get();
        if(child == nullptr){
            continue;
        }

        auto childRect = child->rect();
        LayoutItem item {};
        item.widget = child;
        item.slot = entry.slot;
        item.resizable = child->isLayoutResizable();
        item.currentMain = axis == StackAxis::Horizontal ? childRect.w : childRect.h;
        item.currentCross = axis == StackAxis::Horizontal ? childRect.h : childRect.w;

        item.marginMainBefore = axis == StackAxis::Horizontal ? entry.slot.margin.left : entry.slot.margin.top;
        item.marginMainAfter = axis == StackAxis::Horizontal ? entry.slot.margin.right : entry.slot.margin.bottom;
        item.marginCrossBefore = axis == StackAxis::Horizontal ? entry.slot.margin.top : entry.slot.margin.left;
        item.marginCrossAfter = axis == StackAxis::Horizontal ? entry.slot.margin.bottom : entry.slot.margin.right;

        item.resolvedMain = item.currentMain;
        item.resolvedCross = item.currentCross;

        if(item.resizable){
            if(entry.slot.basis){
                item.resolvedMain = *entry.slot.basis;
            }
            item.resolvedMain = clampSize(item.resolvedMain,entry.slot.minMain,entry.slot.maxMain);
            item.resolvedCross = clampSize(item.resolvedCross,entry.slot.minCross,entry.slot.maxCross);
        }

        item.minMain = entry.slot.minMain.value_or(0.f);
        items.push_back(item);
    }

    if(items.empty()){
        needsLayout = false;
        inLayout = false;
        return;
    }

    float usedMain = options.spacing * static_cast<float>(items.size() > 0 ? items.size() - 1 : 0);
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

    usedMain = options.spacing * static_cast<float>(items.size() > 0 ? items.size() - 1 : 0);
    for(const auto & item : items){
        usedMain += item.marginMainBefore + item.resolvedMain + item.marginMainAfter;
    }

    float extraMain = std::max(0.f,contentMain - usedMain);
    float layoutSpacing = options.spacing;
    float startOffset = 0.f;

    switch(options.mainAlign){
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

    for(auto & item : items){
        auto childRect = item.widget->rect();
        float mainSize = item.resizable ? item.resolvedMain : item.currentMain;

        cursor += item.marginMainBefore;

        auto align = item.slot.alignSelf.value_or(options.crossAlign);
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

        item.widget->setRect(targetRect);

        cursor += mainSize + item.marginMainAfter + layoutSpacing;
    }

    needsLayout = false;
    inLayout = false;
}

StackWidget::~StackWidget(){
    for(auto & entry : stackChildren){
        if(entry.widget != nullptr){
            entry.widget->detachFromParent();
        }
    }
    stackChildren.clear();
}

HStack::HStack(const Core::Rect & rect,WidgetPtr parent,const StackOptions & options):
StackWidget(StackAxis::Horizontal,rect,parent,options){

}

VStack::VStack(const Core::Rect & rect,WidgetPtr parent,const StackOptions & options):
StackWidget(StackAxis::Vertical,rect,parent,options){

}

}
