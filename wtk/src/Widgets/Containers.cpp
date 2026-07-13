#include "omegaWTK/Widgets/Containers.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/UI/LayoutManager.h"

namespace OmegaWTK {

namespace {

// Phase 4.6: the public StackOptions / StackSlot enums and structs map
// one-for-one onto the FlexLayout-family equivalents. The helpers
// below adapt the Stack* surface (kept on `StackWidget` so existing
// call sites — tests, BasicAppTest, ImageRenderTest — compile
// unchanged) into the FlexOptions / FlexChildSpec values that
// `flexLayout_` actually consumes.

inline FlexMainAlign toFlexMainAlign(StackMainAlign align){
    switch(align){
        case StackMainAlign::Start:        return FlexMainAlign::Start;
        case StackMainAlign::Center:       return FlexMainAlign::Center;
        case StackMainAlign::End:          return FlexMainAlign::End;
        case StackMainAlign::SpaceBetween: return FlexMainAlign::SpaceBetween;
        case StackMainAlign::SpaceAround:  return FlexMainAlign::SpaceAround;
        case StackMainAlign::SpaceEvenly:  return FlexMainAlign::SpaceEvenly;
    }
    return FlexMainAlign::Start;
}

inline FlexCrossAlign toFlexCrossAlign(StackCrossAlign align){
    switch(align){
        case StackCrossAlign::Start:   return FlexCrossAlign::Start;
        case StackCrossAlign::Center:  return FlexCrossAlign::Center;
        case StackCrossAlign::End:     return FlexCrossAlign::End;
        case StackCrossAlign::Stretch: return FlexCrossAlign::Stretch;
    }
    return FlexCrossAlign::Start;
}

inline FlexInsets toFlexInsets(const StackInsets & insets){
    return FlexInsets{insets.left, insets.top, insets.right, insets.bottom};
}

inline FlexOptions toFlexOptions(StackAxis axis, const StackOptions & opts){
    FlexOptions out {};
    out.axis       = (axis == StackAxis::Horizontal)
        ? LayoutAxis::Horizontal : LayoutAxis::Vertical;
    out.spacing    = opts.spacing;
    out.padding    = toFlexInsets(opts.padding);
    out.mainAlign  = toFlexMainAlign(opts.mainAlign);
    out.crossAlign = toFlexCrossAlign(opts.crossAlign);
    return out;
}

// Grid: the public GridOptions / GridSlot surface maps one-for-one onto
// the GridLayout-family equivalents, the same way Stack* maps onto Flex*.
inline GridCellAlign toGridCellAlign(StackCrossAlign align){
    switch(align){
        case StackCrossAlign::Start:   return GridCellAlign::Start;
        case StackCrossAlign::Center:  return GridCellAlign::Center;
        case StackCrossAlign::End:     return GridCellAlign::End;
        case StackCrossAlign::Stretch: return GridCellAlign::Stretch;
    }
    return GridCellAlign::Start;
}

inline GridLayoutOptions toGridLayoutOptions(const GridOptions & opts){
    GridLayoutOptions out {};
    out.columns       = opts.columns;
    out.rowSpacing    = opts.rowSpacing;
    out.columnSpacing = opts.columnSpacing;
    out.cellAlign     = toGridCellAlign(opts.cellAlign);
    return out;
}

inline GridChildSpec toGridChildSpec(const GridSlot & slot){
    GridChildSpec out {};
    out.columnSpan = slot.columnSpan;
    out.rowSpan    = slot.rowSpan;
    return out;
}

inline FlexChildSpec toFlexChildSpec(const StackSlot & slot, bool resizable,
                                    bool honorCrossStretch){
    FlexChildSpec out {};
    out.resizable  = resizable;
    out.honorCrossStretch = honorCrossStretch;
    out.flexGrow   = slot.flexGrow;
    out.flexShrink = slot.flexShrink;
    out.basis      = slot.basis;
    out.minMain    = slot.minMain;
    out.maxMain    = slot.maxMain;
    out.minCross   = slot.minCross;
    out.maxCross   = slot.maxCross;
    out.margin     = toFlexInsets(slot.margin);
    if(slot.alignSelf){
        out.alignSelf = toFlexCrossAlign(*slot.alignSelf);
    }
    return out;
}

} // namespace

StackWidget::StackWidget(StackAxis axis,Composition::Rect rect,const StackOptions & options):
Container(rect),
axis(axis),
stackOptions(options),
flexLayout_(toFlexOptions(axis, options)){
    if(view != nullptr){
        view->setLayoutManager(&flexLayout_);
    }
}

StackWidget::StackWidget(StackAxis axis,ViewPtr view,const StackOptions & options):
Container(std::move(view)),
axis(axis),
stackOptions(options),
flexLayout_(toFlexOptions(axis, options)){
    if(this->view != nullptr){
        this->view->setLayoutManager(&flexLayout_);
    }
}

void StackWidget::onMount(){
    relayout();
}

void StackWidget::resize(Composition::Rect & newRect){
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
    stackOptions = options;
    flexLayout_.setOptions(toFlexOptions(axis, options));
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
        if(children[i] == child){
            if(i < childSlots.size()){
                childSlots[i] = slot;
            }
            flexLayout_.setChildSpec(&child->viewRef(),
                                     toFlexChildSpec(slot, child->isLayoutResizable(),
                                                         child->layoutCrossStretchAllowed()));
            relayout();
            return child;
        }
    }

    wireChild(child);
    childSlots.push_back(slot);
    flexLayout_.setChildSpec(&child->viewRef(),
                             toFlexChildSpec(slot, child->isLayoutResizable(),
                                                         child->layoutCrossStretchAllowed()));
    relayout();
    return child;
}

bool StackWidget::removeChild(const WidgetPtr & child){
    if(child == nullptr){
        return false;
    }
    for(std::size_t i = 0; i < children.size(); ++i){
        if(children[i] == child){
            flexLayout_.removeChildSpec(&child->viewRef());
            unwireChild(child);
            if(i < childSlots.size()){
                childSlots.erase(childSlots.begin() + static_cast<std::ptrdiff_t>(i));
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
        if(children[i] == child){
            if(i < childSlots.size()){
                childSlots[i] = slot;
            }
            flexLayout_.setChildSpec(&child->viewRef(),
                                     toFlexChildSpec(slot, child->isLayoutResizable(),
                                                         child->layoutCrossStretchAllowed()));
            relayout();
            return true;
        }
    }
    return false;
}

bool StackWidget::setSlot(std::size_t idx,const StackSlot & slot){
    if(idx >= childSlots.size() || idx >= children.size()){
        return false;
    }
    childSlots[idx] = slot;
    auto & child = children[idx];
    if(child != nullptr){
        flexLayout_.setChildSpec(&child->viewRef(),
                                 toFlexChildSpec(slot, child->isLayoutResizable(),
                                                         child->layoutCrossStretchAllowed()));
    }
    relayout();
    return true;
}

Core::Optional<StackSlot> StackWidget::getSlot(const WidgetPtr & child) const{
    if(child == nullptr){
        return {};
    }
    for(std::size_t i = 0; i < children.size(); ++i){
        if(children[i] == child && i < childSlots.size()){
            return childSlots[i];
        }
    }
    return {};
}

void StackWidget::relayout(){
    // Stale-child cleanup mirrors `Container::relayout` and the
    // pre-4.6 `StackWidget::layoutChildren` head — flexLayout_ keeps
    // per-child entries keyed by View*, so a nulled-out slot must be
    // removed before arrange iterates `view->subviews()`.
    for(std::size_t i = 0; i < children.size(); ){
        if(children[i] == nullptr){
            children.erase(children.begin() + static_cast<std::ptrdiff_t>(i));
            if(i < childSlots.size()){
                childSlots.erase(childSlots.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        else {
            ++i;
        }
    }
    if(view != nullptr){
        flexLayout_.arrange(*view, view->getRect());
    }
}

StackWidget::~StackWidget(){
    childSlots.clear();
}

HStack::HStack(Composition::Rect rect,const StackOptions & options):
StackWidget(StackAxis::Horizontal,rect,options){

}

HStack::HStack(ViewPtr view,const StackOptions & options):
StackWidget(StackAxis::Horizontal,std::move(view),options){

}

VStack::VStack(Composition::Rect rect,const StackOptions & options):
StackWidget(StackAxis::Vertical,rect,options){

}

VStack::VStack(ViewPtr view,const StackOptions & options):
StackWidget(StackAxis::Vertical,std::move(view),options){

}

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

Grid::Grid(Composition::Rect rect,const GridOptions & options):
Container(rect),
options_(options),
gridLayout_(toGridLayoutOptions(options)){
    if(view != nullptr){
        view->setLayoutManager(&gridLayout_);
    }
}

Grid::Grid(ViewPtr view,const GridOptions & options):
Container(std::move(view)),
options_(options),
gridLayout_(toGridLayoutOptions(options)){
    if(this->view != nullptr){
        this->view->setLayoutManager(&gridLayout_);
    }
}

void Grid::onMount(){
    relayout();
}

void Grid::resize(Composition::Rect & newRect){
    (void)newRect;
    relayout();
}

const GridOptions & Grid::getOptions() const{
    return options_;
}

void Grid::setOptions(const GridOptions & options){
    options_ = options;
    gridLayout_.setOptions(toGridLayoutOptions(options));
    relayout();
}

WidgetPtr Grid::addChild(const WidgetPtr & child){
    return addChild(child,{});
}

WidgetPtr Grid::addChild(const WidgetPtr & child,const GridSlot & slot){
    if(child == nullptr || child.get() == this){
        return nullptr;
    }

    for(std::size_t i = 0; i < children.size(); ++i){
        if(children[i] == child){
            if(i < childSlots_.size()){
                childSlots_[i] = slot;
            }
            gridLayout_.setChildSpec(&child->viewRef(), toGridChildSpec(slot));
            relayout();
            return child;
        }
    }

    wireChild(child);
    childSlots_.push_back(slot);
    gridLayout_.setChildSpec(&child->viewRef(), toGridChildSpec(slot));
    relayout();
    return child;
}

bool Grid::removeChild(const WidgetPtr & child){
    if(child == nullptr){
        return false;
    }
    for(std::size_t i = 0; i < children.size(); ++i){
        if(children[i] == child){
            gridLayout_.removeChildSpec(&child->viewRef());
            unwireChild(child);
            if(i < childSlots_.size()){
                childSlots_.erase(childSlots_.begin() + static_cast<std::ptrdiff_t>(i));
            }
            relayout();
            return true;
        }
    }
    return false;
}

void Grid::relayout(){
    // Stale-child cleanup mirrors StackWidget::relayout — gridLayout_ keeps
    // per-child span entries keyed by View*, so a nulled-out slot must be
    // dropped before arrange iterates view->subviews().
    for(std::size_t i = 0; i < children.size(); ){
        if(children[i] == nullptr){
            children.erase(children.begin() + static_cast<std::ptrdiff_t>(i));
            if(i < childSlots_.size()){
                childSlots_.erase(childSlots_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        else {
            ++i;
        }
    }
    if(view != nullptr){
        gridLayout_.arrange(*view, view->getRect());
    }
}

Grid::~Grid(){
    childSlots_.clear();
}

}
