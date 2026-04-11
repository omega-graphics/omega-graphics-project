#include "omegaWTK/UI/Layout.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/UIView.h"
#include "LayoutBehaviors.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OmegaWTK {

// ---------------------------------------------------------------------------
// LayoutLength
// ---------------------------------------------------------------------------

LayoutLength LayoutLength::Auto(){
    return {LayoutUnit::Auto,0.f};
}

LayoutLength LayoutLength::Px(float v){
    return {LayoutUnit::Px,v};
}

LayoutLength LayoutLength::Dp(float v){
    return {LayoutUnit::Dp,v};
}

LayoutLength LayoutLength::Percent(float v){
    return {LayoutUnit::Percent,v};
}

LayoutLength LayoutLength::Fr(float v){
    return {LayoutUnit::Fr,v};
}

LayoutLength LayoutLength::Intrinsic(){
    return {LayoutUnit::Intrinsic,0.f};
}

bool LayoutLength::isAuto() const{
    return unit == LayoutUnit::Auto;
}

bool LayoutLength::isIntrinsic() const{
    return unit == LayoutUnit::Intrinsic;
}

bool LayoutLength::isFixed() const{
    return unit == LayoutUnit::Px || unit == LayoutUnit::Dp;
}

// ---------------------------------------------------------------------------
// LayoutEdges
// ---------------------------------------------------------------------------

LayoutEdges LayoutEdges::Zero(){
    return {
        LayoutLength::Dp(0.f),
        LayoutLength::Dp(0.f),
        LayoutLength::Dp(0.f),
        LayoutLength::Dp(0.f)
    };
}

LayoutEdges LayoutEdges::All(LayoutLength value){
    return {value,value,value,value};
}

LayoutEdges LayoutEdges::Symmetric(LayoutLength horizontal,LayoutLength vertical){
    return {horizontal,vertical,horizontal,vertical};
}

// ---------------------------------------------------------------------------
// LayoutContext
// ---------------------------------------------------------------------------

float LayoutContext::dpToPx(float dp) const{
    return dp * dpiScale;
}

Composition::Rect LayoutContext::availableRectDp() const{
    if(dpiScale <= 0.f){
        return availableRectPx;
    }
    const float inv = 1.f / dpiScale;
    return Composition::Rect{
        Composition::Point2D{availableRectPx.pos.x * inv,availableRectPx.pos.y * inv},
        availableRectPx.w * inv,
        availableRectPx.h * inv
    };
}

// ---------------------------------------------------------------------------
// Resolver free functions
// ---------------------------------------------------------------------------

float resolveLength(const LayoutLength & len,float availableDp,float dpiScale){
    switch(len.unit){
        case LayoutUnit::Auto:
            return availableDp;
        case LayoutUnit::Px:
            return (dpiScale > 0.f) ? (len.value / dpiScale) : len.value;
        case LayoutUnit::Dp:
            return len.value;
        case LayoutUnit::Percent:
            return len.value * availableDp;
        case LayoutUnit::Fr:
            return len.value;
        case LayoutUnit::Intrinsic:
            return availableDp;
    }
    return availableDp;
}

float clampValue(float value,
                 const LayoutLength & min,
                 const LayoutLength & max,
                 float availableDp,
                 float dpiScale){
    float minResolved = min.isAuto() ? 0.f : resolveLength(min,availableDp,dpiScale);
    float maxResolved = max.isAuto() ? std::numeric_limits<float>::infinity()
                                     : resolveLength(max,availableDp,dpiScale);
    if(maxResolved < minResolved){
        maxResolved = minResolved;
    }
    return std::clamp(value,minResolved,maxResolved);
}

Composition::Rect resolveClampedRect(const LayoutStyle & style,
                              const Composition::Rect & availableDp,
                              float dpiScale){
    float w = resolveLength(style.width,availableDp.w,dpiScale);
    float h = resolveLength(style.height,availableDp.h,dpiScale);
    w = clampValue(w,style.clamp.minWidth,style.clamp.maxWidth,availableDp.w,dpiScale);
    h = clampValue(h,style.clamp.minHeight,style.clamp.maxHeight,availableDp.h,dpiScale);

    if(style.aspectRatio){
        float ar = *style.aspectRatio;
        if(ar > 0.f){
            if(!style.width.isAuto() && style.height.isAuto()){
                h = w / ar;
            }
            else if(style.width.isAuto() && !style.height.isAuto()){
                w = h * ar;
            }
        }
    }

    float marginLeft = resolveLength(style.margin.left,availableDp.w,dpiScale);
    float marginTop  = resolveLength(style.margin.top,availableDp.h,dpiScale);

    float x = availableDp.pos.x + marginLeft;
    float y = availableDp.pos.y + marginTop;

    if(style.position == LayoutPositionMode::Absolute){
        if(!style.insetLeft.isAuto()){
            x = availableDp.pos.x + resolveLength(style.insetLeft,availableDp.w,dpiScale);
        }
        if(!style.insetTop.isAuto()){
            y = availableDp.pos.y + resolveLength(style.insetTop,availableDp.h,dpiScale);
        }
    }

    return Composition::Rect{Composition::Point2D{x,y},std::max(1.f,w),std::max(1.f,h)};
}

// ---------------------------------------------------------------------------
// LegacyResizeCoordinatorBehavior (B2)
// ---------------------------------------------------------------------------

LegacyResizeCoordinatorBehavior::LegacyResizeCoordinatorBehavior(ViewResizeCoordinator & coordinator)
    : coordinator_(coordinator){}

MeasureResult LegacyResizeCoordinatorBehavior::measure(LayoutNode & /*node*/, const LayoutContext & ctx){
    auto dpRect = ctx.availableRectDp();
    return {dpRect.w, dpRect.h};
}

void LegacyResizeCoordinatorBehavior::arrange(LayoutNode & /*node*/, const LayoutContext & ctx){
    coordinator_.resolve(ctx.availableRectPx);
}

// ---------------------------------------------------------------------------
// StackLayoutBehavior (B3)
// ---------------------------------------------------------------------------

MeasureResult StackLayoutBehavior::measure(LayoutNode & /*node*/, const LayoutContext & ctx){
    auto dpRect = ctx.availableRectDp();
    return {dpRect.w, dpRect.h};
}

void StackLayoutBehavior::arrange(LayoutNode & /*node*/, const LayoutContext & /*ctx*/){
}

// ---------------------------------------------------------------------------
// runWidgetLayout (B3)
// ---------------------------------------------------------------------------

void runWidgetLayout(Widget & root, const LayoutContext & ctx){
    if(!root.hasExplicitLayoutStyle()){
        auto & coordinator = root.viewRef().getResizeCoordinator();
        coordinator.resolve(ctx.availableRectPx);
        return;
    }

    auto dpRect = ctx.availableRectDp();
    auto behavior = root.layoutBehavior();
    if(behavior != nullptr){
        return;
    }

    auto resolved = resolveClampedRect(root.layoutStyle(), dpRect, ctx.dpiScale);

    Composition::Rect finalPx {
        Composition::Point2D{ctx.dpToPx(resolved.pos.x), ctx.dpToPx(resolved.pos.y)},
        ctx.dpToPx(resolved.w),
        ctx.dpToPx(resolved.h)
    };

    root.onLayoutResolved(finalPx);
}

// ---------------------------------------------------------------------------
// LayoutDelta computation (D1)
// ---------------------------------------------------------------------------

LayoutDelta computeLayoutDelta(const Composition::Rect & fromPx,const Composition::Rect & toPx){
    LayoutDelta delta {};
    delta.fromRectPx = fromPx;
    delta.toRectPx = toPx;

    constexpr float kEpsilon = 0.5f;
    if(std::fabs(fromPx.pos.x - toPx.pos.x) > kEpsilon){
        delta.changedProperties.push_back(LayoutTransitionProperty::X);
    }
    if(std::fabs(fromPx.pos.y - toPx.pos.y) > kEpsilon){
        delta.changedProperties.push_back(LayoutTransitionProperty::Y);
    }
    if(std::fabs(fromPx.w - toPx.w) > kEpsilon){
        delta.changedProperties.push_back(LayoutTransitionProperty::Width);
    }
    if(std::fabs(fromPx.h - toPx.h) > kEpsilon){
        delta.changedProperties.push_back(LayoutTransitionProperty::Height);
    }
    return delta;
}

// ---------------------------------------------------------------------------
// VectorDiagnosticSink (F1)
// ---------------------------------------------------------------------------

void VectorDiagnosticSink::record(const LayoutDiagnosticEntry & entry){
    entries_.push_back(entry);
}

const OmegaCommon::Vector<LayoutDiagnosticEntry> & VectorDiagnosticSink::entries() const{
    return entries_;
}

void VectorDiagnosticSink::clear(){
    entries_.clear();
}

// ---------------------------------------------------------------------------
// StyleRule (E2)
// ---------------------------------------------------------------------------

bool StyleRule::beats(const StyleRule & other) const{
    if(specificity != other.specificity){
        return specificity > other.specificity;
    }
    return sourceOrder >= other.sourceOrder;
}

static int computeSpecificity(const OmegaCommon::String & selectorTag,
                              const OmegaCommon::String & viewTag){
    int spec = 0;
    (void)viewTag;
    if(!selectorTag.empty()){
        spec += 2;
    }
    return spec;
}

OmegaCommon::Vector<StyleRule> convertEntriesToRules(
    const StyleSheet & sheet,
    const OmegaCommon::String & viewTag){

    const auto & entries = sheet.entries;
    OmegaCommon::Vector<StyleRule> rules {};
    rules.reserve(entries.size());

    for(std::size_t idx = 0; idx < entries.size(); ++idx){
        const auto & e = entries[idx];
        auto makeRule = [&](StyleRule::Property prop) -> StyleRule {
            StyleRule r {};
            r.selectorTag = e.elementTag;
            r.specificity = computeSpecificity(e.elementTag,viewTag);
            r.sourceOrder = idx;
            r.property = prop;
            return r;
        };

        switch(e.kind){
            case StyleSheet::Entry::Kind::LayoutWidth: {
                auto r = makeRule(StyleRule::Property::LayoutWidth);
                r.lengthValue = e.layoutLengthValue;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::LayoutHeight: {
                auto r = makeRule(StyleRule::Property::LayoutHeight);
                r.lengthValue = e.layoutLengthValue;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::LayoutMargin: {
                if(e.layoutEdgesValue){
                    const auto & edges = *e.layoutEdgesValue;
                    auto emitEdgeRule = [&](StyleRule::Property prop,LayoutLength val){
                        auto r = makeRule(prop);
                        r.lengthValue = val;
                        rules.push_back(r);
                    };
                    emitEdgeRule(StyleRule::Property::LayoutMarginLeft,edges.left);
                    emitEdgeRule(StyleRule::Property::LayoutMarginTop,edges.top);
                    emitEdgeRule(StyleRule::Property::LayoutMarginRight,edges.right);
                    emitEdgeRule(StyleRule::Property::LayoutMarginBottom,edges.bottom);
                }
                break;
            }
            case StyleSheet::Entry::Kind::LayoutPadding: {
                if(e.layoutEdgesValue){
                    const auto & edges = *e.layoutEdgesValue;
                    auto emitEdgeRule = [&](StyleRule::Property prop,LayoutLength val){
                        auto r = makeRule(prop);
                        r.lengthValue = val;
                        rules.push_back(r);
                    };
                    emitEdgeRule(StyleRule::Property::LayoutPaddingLeft,edges.left);
                    emitEdgeRule(StyleRule::Property::LayoutPaddingTop,edges.top);
                    emitEdgeRule(StyleRule::Property::LayoutPaddingRight,edges.right);
                    emitEdgeRule(StyleRule::Property::LayoutPaddingBottom,edges.bottom);
                }
                break;
            }
            case StyleSheet::Entry::Kind::LayoutClamp: {
                if(e.layoutClampValue){
                    auto r = makeRule(StyleRule::Property::LayoutClampMinWidth);
                    r.lengthValue = e.layoutClampValue->minWidth;
                    rules.push_back(r);
                    r = makeRule(StyleRule::Property::LayoutClampMinHeight);
                    r.lengthValue = e.layoutClampValue->minHeight;
                    rules.push_back(r);
                    r = makeRule(StyleRule::Property::LayoutClampMaxWidth);
                    r.lengthValue = e.layoutClampValue->maxWidth;
                    rules.push_back(r);
                    r = makeRule(StyleRule::Property::LayoutClampMaxHeight);
                    r.lengthValue = e.layoutClampValue->maxHeight;
                    rules.push_back(r);
                }
                break;
            }
            case StyleSheet::Entry::Kind::LayoutTransition: {
                auto r = makeRule(StyleRule::Property::LayoutTransition);
                r.transitionValue = e.layoutTransitionValue;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::BackgroundColor: {
                auto r = makeRule(StyleRule::Property::BackgroundColor);
                r.colorValue = e.color;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::BorderEnabled: {
                auto r = makeRule(StyleRule::Property::BorderEnabled);
                r.boolValue = e.boolValue;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::BorderColor: {
                auto r = makeRule(StyleRule::Property::BorderColor);
                r.colorValue = e.color;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::BorderWidth: {
                auto r = makeRule(StyleRule::Property::BorderWidth);
                r.floatValue = e.floatValue;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::DropShadowEffect: {
                auto r = makeRule(StyleRule::Property::DropShadow);
                r.dropShadowValue = e.dropShadowValue;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::GaussianBlurEffect: {
                auto r = makeRule(StyleRule::Property::GaussianBlur);
                r.gaussianBlurValue = e.gaussianBlurValue;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::DirectionalBlurEffect: {
                auto r = makeRule(StyleRule::Property::DirectionalBlur);
                r.directionalBlurValue = e.directionalBlurValue;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::ElementBrush: {
                auto r = makeRule(StyleRule::Property::ElementBrush);
                r.brushValue = e.brush;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::TextFont: {
                auto r = makeRule(StyleRule::Property::TextFont);
                r.fontValue = e.font;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::TextColor: {
                auto r = makeRule(StyleRule::Property::TextColor);
                r.colorValue = e.color;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::TextAlignment: {
                auto r = makeRule(StyleRule::Property::TextAlignment);
                r.textAlignmentValue = e.textAlignment;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::TextWrapping: {
                auto r = makeRule(StyleRule::Property::TextWrapping);
                r.textWrappingValue = e.textWrapping;
                rules.push_back(r);
                break;
            }
            case StyleSheet::Entry::Kind::TextLineLimit: {
                auto r = makeRule(StyleRule::Property::TextLineLimit);
                r.uintValue = e.uintValue;
                rules.push_back(r);
                break;
            }
            default:
                break;
        }
    }

    return rules;
}

static bool matchTag(const OmegaCommon::String & selector,const OmegaCommon::String & tag){
    return selector.empty() || selector == tag;
}

void mergeLayoutRulesIntoStyle(LayoutStyle & style,
                               const OmegaCommon::Vector<StyleRule> & rules,
                               const OmegaCommon::String & elementTag){
    struct Best {
        int specificity = -1;
        std::size_t order = 0;
    };
    OmegaCommon::Map<StyleRule::Property,Best> winners {};

    auto tryApply = [&](const StyleRule & r,StyleRule::Property prop) -> bool {
        if(!matchTag(r.selectorTag,elementTag)){
            return false;
        }
        auto & b = winners[prop];
        if(r.specificity > b.specificity ||
           (r.specificity == b.specificity && r.sourceOrder >= b.order)){
            b.specificity = r.specificity;
            b.order = r.sourceOrder;
            return true;
        }
        return false;
    };

    for(const auto & r : rules){
        switch(r.property){
            case StyleRule::Property::LayoutWidth:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.width = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutHeight:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.height = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutMarginLeft:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.margin.left = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutMarginTop:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.margin.top = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutMarginRight:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.margin.right = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutMarginBottom:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.margin.bottom = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutPaddingLeft:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.padding.left = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutPaddingTop:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.padding.top = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutPaddingRight:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.padding.right = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutPaddingBottom:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.padding.bottom = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutClampMinWidth:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.clamp.minWidth = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutClampMinHeight:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.clamp.minHeight = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutClampMaxWidth:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.clamp.maxWidth = *r.lengthValue;
                }
                break;
            case StyleRule::Property::LayoutClampMaxHeight:
                if(tryApply(r,r.property) && r.lengthValue){
                    style.clamp.maxHeight = *r.lengthValue;
                }
                break;
            default:
                break;
        }
    }
}

Core::Optional<LayoutTransitionSpec> resolveLayoutTransition(
    const OmegaCommon::Vector<StyleRule> & rules,
    const OmegaCommon::String & elementTag){

    Core::Optional<LayoutTransitionSpec> best {};
    int bestSpec = -1;
    std::size_t bestOrder = 0;

    for(const auto & r : rules){
        if(r.property != StyleRule::Property::LayoutTransition){
            continue;
        }
        if(!matchTag(r.selectorTag,elementTag)){
            continue;
        }
        if(r.specificity > bestSpec ||
           (r.specificity == bestSpec && r.sourceOrder >= bestOrder)){
            bestSpec = r.specificity;
            bestOrder = r.sourceOrder;
            if(r.transitionValue){
                best = *r.transitionValue;
            }
        }
    }
    return best;
}

} // namespace OmegaWTK
