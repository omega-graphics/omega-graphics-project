#include "omegaWTK/UI/UIView.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace OmegaWTK {

namespace {

struct ResolvedViewStyle {
    Core::Optional<Composition::Color> backgroundColor {};
    bool useBorder = false;
    Core::Optional<Composition::Color> borderColor {};
    float borderWidth = 0.f;
};

struct ResolvedTextStyle {
    SharedHandle<Composition::Font> font = nullptr;
    Composition::Color color = Composition::Color::create8Bit(Composition::Color::Black8);
    Composition::TextLayoutDescriptor layout {
        Composition::TextLayoutDescriptor::LeftUpper,
        Composition::TextLayoutDescriptor::None
    };
    unsigned lineLimit = 0;
};

struct ResolvedEffectTransition {
    bool transition = false;
    float duration = 0.f;
    SharedHandle<Composition::AnimationCurve> curve = nullptr;
};

struct ResolvedEffectStyle {
    Core::Optional<Composition::LayerEffect::DropShadowParams> dropShadow {};
    ResolvedEffectTransition dropShadowTransition {};
    Core::Optional<Composition::CanvasEffect::GaussianBlurParams> gaussianBlur {};
    ResolvedEffectTransition gaussianBlurTransition {};
    Core::Optional<Composition::CanvasEffect::DirectionalBlurParams> directionalBlur {};
    ResolvedEffectTransition directionalBlurTransition {};
};

constexpr const char *kUIViewRootEffectTag = "__UIViewRootEffectTarget__";

static bool matchesTag(const OmegaCommon::String & selector,const OmegaCommon::String & tag){
    return selector.empty() || selector == tag;
}

static inline bool colorsClose(const Composition::Color & lhs,const Composition::Color & rhs){
    constexpr float kEpsilon = 0.0005f;
    return std::fabs(lhs.r - rhs.r) <= kEpsilon &&
           std::fabs(lhs.g - rhs.g) <= kEpsilon &&
           std::fabs(lhs.b - rhs.b) <= kEpsilon &&
           std::fabs(lhs.a - rhs.a) <= kEpsilon;
}

static inline float clamp01(float value){
    return std::clamp(value,0.f,1.f);
}

static inline bool nearlyEqual(float lhs,float rhs,float epsilon = 0.0005f){
    return std::fabs(lhs - rhs) <= epsilon;
}

static Core::Optional<Composition::Color> colorFromBrush(const SharedHandle<Composition::Brush> & brush){
    if(brush == nullptr || !brush->isColor){
        return {};
    }
    return brush->color;
}

static ResolvedViewStyle resolveViewStyle(const StyleSheetPtr & style,const UIViewTag & viewTag){
    ResolvedViewStyle resolved {};
    if(style == nullptr){
        return resolved;
    }

    for(const auto & entry : style->entries){
        if(!matchesTag(entry.viewTag,viewTag)){
            continue;
        }
        switch(entry.kind){
            case StyleSheet::Entry::Kind::BackgroundColor:
                if(entry.color){
                    resolved.backgroundColor = entry.color;
                }
                break;
            case StyleSheet::Entry::Kind::BorderEnabled:
                if(entry.boolValue){
                    resolved.useBorder = *entry.boolValue;
                }
                break;
            case StyleSheet::Entry::Kind::BorderColor:
                if(entry.color){
                    resolved.borderColor = entry.color;
                }
                break;
            case StyleSheet::Entry::Kind::BorderWidth:
                if(entry.floatValue){
                    resolved.borderWidth = *entry.floatValue;
                }
                break;
            default:
                break;
        }
    }
    return resolved;
}

static SharedHandle<Composition::Brush> resolveElementBrush(const StyleSheetPtr & style,
                                                            const UIViewTag & viewTag,
                                                            const UIElementTag & elementTag){
    auto brush = Composition::ColorBrush(
            Composition::Color::create8Bit(Composition::Color::White8));

    if(style == nullptr){
        return brush;
    }

    for(const auto & entry : style->entries){
        if(entry.kind != StyleSheet::Entry::Kind::ElementBrush){
            continue;
        }
        if(!entry.viewTag.empty() && !matchesTag(entry.viewTag,viewTag)){
            continue;
        }
        if(!matchesTag(entry.elementTag,elementTag)){
            continue;
        }
        if(entry.brush != nullptr){
            brush = entry.brush;
        }
    }
    return brush;
}

static ResolvedTextStyle resolveTextStyle(const StyleSheetPtr & style,
                                          const UIViewTag & viewTag,
                                          const UIElementTag & elementTag){
    ResolvedTextStyle resolved {};
    if(style == nullptr){
        return resolved;
    }

    auto entrySpecificity = [&](const StyleSheet::Entry & entry) -> int {
        int specificity = 0;
        if(!entry.viewTag.empty()){
            if(!matchesTag(entry.viewTag,viewTag)){
                return -1;
            }
            specificity += 1;
        }
        if(!entry.elementTag.empty()){
            if(!matchesTag(entry.elementTag,elementTag)){
                return -1;
            }
            specificity += 2;
        }
        return specificity;
    };

    auto takeCandidate = [](int candidateSpecificity,
                            std::size_t candidateOrder,
                            int & currentSpecificity,
                            std::size_t & currentOrder) -> bool {
        if(candidateSpecificity < 0){
            return false;
        }
        if(candidateSpecificity > currentSpecificity){
            return true;
        }
        return candidateSpecificity == currentSpecificity && candidateOrder >= currentOrder;
    };

    int fontSpecificity = -1;
    int colorSpecificity = -1;
    int alignmentSpecificity = -1;
    int wrappingSpecificity = -1;
    int lineLimitSpecificity = -1;
    std::size_t fontOrder = 0;
    std::size_t colorOrder = 0;
    std::size_t alignmentOrder = 0;
    std::size_t wrappingOrder = 0;
    std::size_t lineLimitOrder = 0;

    for(std::size_t idx = 0; idx < style->entries.size(); ++idx){
        const auto & entry = style->entries[idx];
        const int specificity = entrySpecificity(entry);
        if(specificity < 0){
            continue;
        }

        switch(entry.kind){
            case StyleSheet::Entry::Kind::TextFont:
                if(entry.font != nullptr &&
                   takeCandidate(specificity,idx,fontSpecificity,fontOrder)){
                    resolved.font = entry.font;
                    fontSpecificity = specificity;
                    fontOrder = idx;
                }
                break;
            case StyleSheet::Entry::Kind::TextColor:
                if(entry.color &&
                   takeCandidate(specificity,idx,colorSpecificity,colorOrder)){
                    resolved.color = *entry.color;
                    colorSpecificity = specificity;
                    colorOrder = idx;
                }
                break;
            case StyleSheet::Entry::Kind::TextAlignment:
                if(entry.textAlignment &&
                   takeCandidate(specificity,idx,alignmentSpecificity,alignmentOrder)){
                    resolved.layout.alignment = *entry.textAlignment;
                    alignmentSpecificity = specificity;
                    alignmentOrder = idx;
                }
                break;
            case StyleSheet::Entry::Kind::TextWrapping:
                if(entry.textWrapping &&
                   takeCandidate(specificity,idx,wrappingSpecificity,wrappingOrder)){
                    resolved.layout.wrapping = *entry.textWrapping;
                    wrappingSpecificity = specificity;
                    wrappingOrder = idx;
                }
                break;
            case StyleSheet::Entry::Kind::TextLineLimit:
                if(entry.uintValue &&
                   takeCandidate(specificity,idx,lineLimitSpecificity,lineLimitOrder)){
                    resolved.lineLimit = *entry.uintValue;
                    lineLimitSpecificity = specificity;
                    lineLimitOrder = idx;
                }
                break;
            default:
                break;
        }
    }

    return resolved;
}

static inline Composition::LayerEffect::DropShadowParams makeDefaultShadowParams(){
    Composition::LayerEffect::DropShadowParams params {};
    params.x_offset = 0.f;
    params.y_offset = 0.f;
    params.radius = 0.f;
    params.blurAmount = 0.f;
    params.opacity = 0.f;
    params.color = Composition::Color::Transparent;
    return params;
}

static inline Composition::LayerEffect::DropShadowParams makeClearShadowParams(
        const Composition::LayerEffect::DropShadowParams & source){
    auto params = source;
    params.x_offset = 0.f;
    params.y_offset = 0.f;
    params.radius = 0.f;
    params.blurAmount = 0.f;
    params.opacity = 0.f;
    params.color.a = 0.f;
    return params;
}

static inline bool dropShadowClose(const Composition::LayerEffect::DropShadowParams & lhs,
                                   const Composition::LayerEffect::DropShadowParams & rhs){
    return nearlyEqual(lhs.x_offset,rhs.x_offset) &&
           nearlyEqual(lhs.y_offset,rhs.y_offset) &&
           nearlyEqual(lhs.radius,rhs.radius) &&
           nearlyEqual(lhs.blurAmount,rhs.blurAmount) &&
           nearlyEqual(lhs.opacity,rhs.opacity) &&
           colorsClose(lhs.color,rhs.color);
}

static inline bool gaussianBlurClose(const Composition::CanvasEffect::GaussianBlurParams & lhs,
                                     const Composition::CanvasEffect::GaussianBlurParams & rhs){
    return nearlyEqual(lhs.radius,rhs.radius);
}

static inline bool directionalBlurClose(const Composition::CanvasEffect::DirectionalBlurParams & lhs,
                                        const Composition::CanvasEffect::DirectionalBlurParams & rhs){
    return nearlyEqual(lhs.radius,rhs.radius) &&
           nearlyEqual(lhs.angle,rhs.angle);
}

static ResolvedEffectStyle resolveRootEffectStyle(const StyleSheetPtr & style,
                                                  const UIViewTag & viewTag){
    ResolvedEffectStyle resolved {};
    if(style == nullptr){
        return resolved;
    }

    for(const auto & entry : style->entries){
        if(!entry.viewTag.empty() && !matchesTag(entry.viewTag,viewTag)){
            continue;
        }
        if(!entry.elementTag.empty()){
            continue;
        }

        switch(entry.kind){
            case StyleSheet::Entry::Kind::DropShadowEffect:
                if(entry.dropShadowValue){
                    resolved.dropShadow = *entry.dropShadowValue;
                    resolved.dropShadowTransition.transition = entry.transition;
                    resolved.dropShadowTransition.duration = entry.duration;
                    resolved.dropShadowTransition.curve = entry.curve;
                }
                break;
            case StyleSheet::Entry::Kind::GaussianBlurEffect:
                if(entry.gaussianBlurValue){
                    resolved.gaussianBlur = *entry.gaussianBlurValue;
                    resolved.gaussianBlurTransition.transition = entry.transition;
                    resolved.gaussianBlurTransition.duration = entry.duration;
                    resolved.gaussianBlurTransition.curve = entry.curve;
                }
                break;
            case StyleSheet::Entry::Kind::DirectionalBlurEffect:
                if(entry.directionalBlurValue){
                    resolved.directionalBlur = *entry.directionalBlurValue;
                    resolved.directionalBlurTransition.transition = entry.transition;
                    resolved.directionalBlurTransition.duration = entry.duration;
                    resolved.directionalBlurTransition.curve = entry.curve;
                }
                break;
            default:
                break;
        }
    }
    return resolved;
}

static ResolvedEffectStyle resolveElementEffectStyle(const StyleSheetPtr & style,
                                                     const UIViewTag & viewTag,
                                                     const UIElementTag & elementTag){
    ResolvedEffectStyle resolved {};
    if(style == nullptr){
        return resolved;
    }

    for(const auto & entry : style->entries){
        if(!entry.viewTag.empty() && !matchesTag(entry.viewTag,viewTag)){
            continue;
        }
        if(entry.elementTag.empty() || !matchesTag(entry.elementTag,elementTag)){
            continue;
        }

        switch(entry.kind){
            case StyleSheet::Entry::Kind::DropShadowEffect:
                if(entry.dropShadowValue){
                    resolved.dropShadow = *entry.dropShadowValue;
                    resolved.dropShadowTransition.transition = entry.transition;
                    resolved.dropShadowTransition.duration = entry.duration;
                    resolved.dropShadowTransition.curve = entry.curve;
                }
                break;
            case StyleSheet::Entry::Kind::GaussianBlurEffect:
                if(entry.gaussianBlurValue){
                    resolved.gaussianBlur = *entry.gaussianBlurValue;
                    resolved.gaussianBlurTransition.transition = entry.transition;
                    resolved.gaussianBlurTransition.duration = entry.duration;
                    resolved.gaussianBlurTransition.curve = entry.curve;
                }
                break;
            case StyleSheet::Entry::Kind::DirectionalBlurEffect:
                if(entry.directionalBlurValue){
                    resolved.directionalBlur = *entry.directionalBlurValue;
                    resolved.directionalBlurTransition.transition = entry.transition;
                    resolved.directionalBlurTransition.duration = entry.duration;
                    resolved.directionalBlurTransition.curve = entry.curve;
                }
                break;
            default:
                break;
        }
    }
    return resolved;
}

static bool containsTag(const OmegaCommon::Vector<UIElementTag> & tags,const UIElementTag & tag){
    return std::find(tags.begin(),tags.end(),tag) != tags.end();
}

struct StyleScope {
    bool touchesRoot = false;
    bool touchesAllElements = false;
    OmegaCommon::Vector<UIElementTag> elementTags {};
};

static inline void addUniqueTag(OmegaCommon::Vector<UIElementTag> & tags,const UIElementTag & tag){
    if(tag.empty()){
        return;
    }
    if(!containsTag(tags,tag)){
        tags.push_back(tag);
    }
}

static StyleScope collectStyleScope(const StyleSheetPtr & style,const UIViewTag & viewTag){
    StyleScope scope {};
    if(style == nullptr){
        return scope;
    }

    for(const auto & entry : style->entries){
        if(!entry.viewTag.empty() && !matchesTag(entry.viewTag,viewTag)){
            continue;
        }

        switch(entry.kind){
            case StyleSheet::Entry::Kind::BackgroundColor:
            case StyleSheet::Entry::Kind::BorderEnabled:
            case StyleSheet::Entry::Kind::BorderColor:
            case StyleSheet::Entry::Kind::BorderWidth:
                scope.touchesRoot = true;
                break;
            case StyleSheet::Entry::Kind::DropShadowEffect:
            case StyleSheet::Entry::Kind::GaussianBlurEffect:
            case StyleSheet::Entry::Kind::DirectionalBlurEffect:
                if(entry.elementTag.empty()){
                    scope.touchesRoot = true;
                }
                else {
                    addUniqueTag(scope.elementTags,entry.elementTag);
                }
                break;
            case StyleSheet::Entry::Kind::ElementBrush:
            case StyleSheet::Entry::Kind::ElementBrushAnimation:
            case StyleSheet::Entry::Kind::ElementAnimation:
            case StyleSheet::Entry::Kind::ElementPathAnimation:
            case StyleSheet::Entry::Kind::TextFont:
            case StyleSheet::Entry::Kind::TextColor:
            case StyleSheet::Entry::Kind::TextAlignment:
            case StyleSheet::Entry::Kind::TextWrapping:
            case StyleSheet::Entry::Kind::TextLineLimit:
            case StyleSheet::Entry::Kind::LayoutWidth:
            case StyleSheet::Entry::Kind::LayoutHeight:
            case StyleSheet::Entry::Kind::LayoutMargin:
            case StyleSheet::Entry::Kind::LayoutPadding:
            case StyleSheet::Entry::Kind::LayoutClamp:
            case StyleSheet::Entry::Kind::LayoutTransition:
                if(entry.elementTag.empty()){
                    scope.touchesAllElements = true;
                }
                else {
                    addUniqueTag(scope.elementTags,entry.elementTag);
                }
                break;
            default:
                break;
        }
    }
    return scope;
}

static inline UIView::EffectState toEffectState(const ResolvedEffectStyle & style){
    UIView::EffectState state {};
    if(style.dropShadow){
        state.dropShadow = *style.dropShadow;
    }
    if(style.gaussianBlur){
        state.gaussianBlur = *style.gaussianBlur;
    }
    if(style.directionalBlur){
        state.directionalBlur = *style.directionalBlur;
    }
    return state;
}

static inline bool effectStateClose(const UIView::EffectState & lhs,const UIView::EffectState & rhs){
    if(lhs.dropShadow.has_value() != rhs.dropShadow.has_value()){
        return false;
    }
    if(lhs.dropShadow && rhs.dropShadow && !dropShadowClose(*lhs.dropShadow,*rhs.dropShadow)){
        return false;
    }
    if(lhs.gaussianBlur.has_value() != rhs.gaussianBlur.has_value()){
        return false;
    }
    if(lhs.gaussianBlur && rhs.gaussianBlur && !gaussianBlurClose(*lhs.gaussianBlur,*rhs.gaussianBlur)){
        return false;
    }
    if(lhs.directionalBlur.has_value() != rhs.directionalBlur.has_value()){
        return false;
    }
    if(lhs.directionalBlur && rhs.directionalBlur &&
       !directionalBlurClose(*lhs.directionalBlur,*rhs.directionalBlur)){
        return false;
    }
    return true;
}

static inline bool hasAnyEffect(const UIView::EffectState & state){
    return state.dropShadow.has_value() ||
           state.gaussianBlur.has_value() ||
           state.directionalBlur.has_value();
}

static OmegaCommon::Vector<OmegaGTE::GPoint2D> pathToPoints(const OmegaGTE::GVectorPath2D & path){
    OmegaCommon::Vector<OmegaGTE::GPoint2D> points {};
    auto copy = path;
    points.push_back(copy.firstPt());
    for(auto it = copy.begin(); it != copy.end(); ++it){
        auto seg = *it;
        if(seg.pt_B != nullptr){
            points.push_back(*(seg.pt_B));
        }
    }
    return points;
}

static Core::Optional<OmegaGTE::GVectorPath2D> pointsToPath(const OmegaCommon::Vector<OmegaGTE::GPoint2D> & points){
    if(points.empty()){
        return {};
    }
    OmegaGTE::GVectorPath2D path(points.front());
    for(std::size_t i = 1; i < points.size(); i++){
        path.append(points[i]);
    }
    return path;
}

static Core::Optional<float> shapeDimension(const Shape & shape,ElementAnimationKey key){
    switch(shape.type){
        case Shape::Type::Rect:
            return key == ElementAnimationKeyWidth ? shape.rect.w : shape.rect.h;
        case Shape::Type::RoundedRect:
            return key == ElementAnimationKeyWidth ? shape.roundedRect.w : shape.roundedRect.h;
        case Shape::Type::Ellipse:
            return key == ElementAnimationKeyWidth ? (shape.ellipse.rad_x * 2.f) : (shape.ellipse.rad_y * 2.f);
        default:
            return {};
    }
}

static bool applyShapeDimension(Shape & shape,ElementAnimationKey key,float value){
    const float safeValue = std::max(1.f,value);
    switch(shape.type){
        case Shape::Type::Rect:
            if(key == ElementAnimationKeyWidth){
                shape.rect.w = safeValue;
            }
            else {
                shape.rect.h = safeValue;
            }
            return true;
        case Shape::Type::RoundedRect:
            if(key == ElementAnimationKeyWidth){
                shape.roundedRect.w = safeValue;
                shape.roundedRect.rad_x = std::min(shape.roundedRect.rad_x,shape.roundedRect.w * 0.5f);
            }
            else {
                shape.roundedRect.h = safeValue;
                shape.roundedRect.rad_y = std::min(shape.roundedRect.rad_y,shape.roundedRect.h * 0.5f);
            }
            return true;
        case Shape::Type::Ellipse:
            if(key == ElementAnimationKeyWidth){
                shape.ellipse.rad_x = safeValue * 0.5f;
            }
            else {
                shape.ellipse.rad_y = safeValue * 0.5f;
            }
            return true;
        default:
            return false;
    }
}

static inline bool isValidDimension(float v){
    return std::isfinite(v) && v > 0.f;
}

static inline float clampDrawableDimension(float v){
#if defined(TARGET_MACOS)
    constexpr float kMaxDrawableDimension = 8192.f;
#else
    constexpr float kMaxDrawableDimension = 16384.f;
#endif
    if(!std::isfinite(v)){
        return 1.f;
    }
    return std::clamp(v,1.f,kMaxDrawableDimension);
}

static inline bool isSuspiciousDimensionPair(float w,float h){
    if(!std::isfinite(w) || !std::isfinite(h) || w <= 0.f || h <= 0.f){
        return true;
    }
    const float maxDim = std::max(w,h);
    const float minDim = std::min(w,h);
    if(maxDim >= 4096.f && minDim <= 1.5f){
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

static Core::Rect localBoundsFromView(UIView *view){
    constexpr Core::Rect kFallbackRect{
            Core::Position{0.f,0.f},
            1.f,
            1.f
    };
    if(view == nullptr){
        return kFallbackRect;
    }

    struct StableBoundsState {
        bool hasStable = false;
        Core::Rect bounds {Core::Position{0.f,0.f},1.f,1.f};
    };
    static std::unordered_map<UIView *,StableBoundsState> stableBoundsByView {};

    const auto & viewRect = view->getRect();
    float viewWidth = viewRect.w;
    float viewHeight = viewRect.h;
    float limbWidth = 0.f;
    float limbHeight = 0.f;

    auto *tree = view->getLayerTree();
    if(tree != nullptr && tree->getRootLayer() != nullptr){
        const auto & treeRect = tree->getRootLayer()->getLayerRect();
        limbWidth = treeRect.w;
        limbHeight = treeRect.h;
    }

    const bool viewValid = isValidDimension(viewWidth) &&
                           isValidDimension(viewHeight) &&
                           !isSuspiciousDimensionPair(viewWidth,viewHeight);
    const bool limbValid = isValidDimension(limbWidth) &&
                           isValidDimension(limbHeight) &&
                           !isSuspiciousDimensionPair(limbWidth,limbHeight);

    float width = 0.f;
    float height = 0.f;
    bool candidateValid = false;
    if(viewValid){
        width = viewWidth;
        height = viewHeight;
        candidateValid = true;
    }
    else if(limbValid){
        width = limbWidth;
        height = limbHeight;
        candidateValid = true;
    }

    auto & stable = stableBoundsByView[view];
    if(!candidateValid){
        if(stable.hasStable){
            return stable.bounds;
        }
        return kFallbackRect;
    }

    width = clampDrawableDimension(width);
    height = clampDrawableDimension(height);

    Core::Rect resolved {
            Core::Position{0.f,0.f},
            width,
            height
    };
    stable.hasStable = true;
    stable.bounds = resolved;
    return resolved;
}

}

Shape Shape::Scalar(int width,int height){
    Shape shape {};
    shape.type = Shape::Type::Rect;
    shape.rect = Core::Rect{{0.f,0.f},static_cast<float>(width),static_cast<float>(height)};
    return shape;
}

Shape Shape::Rect(const Core::Rect &rect){
    Shape shape {};
    shape.type = Shape::Type::Rect;
    shape.rect = rect;
    return shape;
}

Shape Shape::RoundedRect(const Core::RoundedRect &rect){
    Shape shape {};
    shape.type = Shape::Type::RoundedRect;
    shape.roundedRect = rect;
    return shape;
}

Shape Shape::Ellipse(const OmegaGTE::GEllipsoid &ellipse){
    Shape shape {};
    shape.type = Shape::Type::Ellipse;
    shape.ellipse = ellipse;
    return shape;
}

Shape Shape::Ellipse(const Core::Ellipse &ellipse){
    return Shape::Ellipse(OmegaGTE::GEllipsoid{
            ellipse.x,
            ellipse.y,
            0.f,
            ellipse.rad_x,
            ellipse.rad_y,
            0.f
    });
}

Shape Shape::Path(const OmegaGTE::GVectorPath2D &path,unsigned strokeWidth,bool closePath){
    Shape shape {};
    shape.type = Shape::Type::Path;
    shape.path = path;
    shape.pathStrokeWidth = std::max(1u,strokeWidth);
    shape.closePath = closePath;
    return shape;
}

void UIViewLayout::text(UIElementTag tag,OmegaCommon::UString content){
    auto taggedElementIt = std::find_if(_content.begin(),_content.end(),[&](const Element & element){
        return element.tag == tag;
    });

    if(taggedElementIt != _content.end()){
        if(taggedElementIt->type != Element::Type::Text){
            return;
        }
        taggedElementIt->str = content;
        taggedElementIt->textRect = {};
        taggedElementIt->textStyleTag = {};
        return;
    }

    Element element {};
    element.type = Element::Type::Text;
    element.tag = tag;
    element.str = content;
    _content.push_back(element);
}

void UIViewLayout::text(UIElementTag tag,OmegaCommon::UString content,const Core::Rect & rect){
    auto taggedElementIt = std::find_if(_content.begin(),_content.end(),[&](const Element & element){
        return element.tag == tag;
    });

    if(taggedElementIt != _content.end()){
        if(taggedElementIt->type != Element::Type::Text){
            return;
        }
        taggedElementIt->str = content;
        taggedElementIt->textRect = rect;
        return;
    }

    Element element {};
    element.type = Element::Type::Text;
    element.tag = tag;
    element.str = content;
    element.textRect = rect;
    _content.push_back(element);
}

void UIViewLayout::text(UIElementTag tag,OmegaCommon::UString content,const Core::Rect & rect,UIElementTag styleTag){
    auto taggedElementIt = std::find_if(_content.begin(),_content.end(),[&](const Element & element){
        return element.tag == tag;
    });

    if(taggedElementIt != _content.end()){
        if(taggedElementIt->type != Element::Type::Text){
            return;
        }
        taggedElementIt->str = content;
        taggedElementIt->textRect = rect;
        taggedElementIt->textStyleTag = styleTag;
        return;
    }

    Element element {};
    element.type = Element::Type::Text;
    element.tag = tag;
    element.str = content;
    element.textRect = rect;
    element.textStyleTag = styleTag;
    _content.push_back(element);
}

void UIViewLayout::shape(UIElementTag tag,const Shape &shape){
    auto taggedElementIt = std::find_if(_content.begin(),_content.end(),[&](const Element & element){
        return element.tag == tag;
    });

    if(taggedElementIt != _content.end()){
        if(taggedElementIt->type != Element::Type::Shape){
            return;
        }
        taggedElementIt->shape = shape;
        taggedElementIt->textRect = {};
        taggedElementIt->textStyleTag = {};
        return;
    }

    Element element {};
    element.type = Element::Type::Shape;
    element.tag = tag;
    element.shape = shape;
    _content.push_back(element);
}

bool UIViewLayout::remove(UIElementTag tag){
    auto taggedElementIt = std::find_if(_content.begin(),_content.end(),[&](const Element & element){
        return element.tag == tag;
    });
    if(taggedElementIt == _content.end()){
        return false;
    }
    _content.erase(taggedElementIt);
    return true;
}

void UIViewLayout::clear(){
    _content.clear();
}

const OmegaCommon::Vector<UIViewLayout::Element> & UIViewLayout::elements() const{
    return _content;
}

StyleSheetPtr StyleSheet::Create(){
    return make<StyleSheet>();
}

StyleSheetPtr StyleSheet::copy(){
    return StyleSheetPtr(new StyleSheet(*this));
}

StyleSheet::StyleSheet() = default;

StyleSheetPtr StyleSheet::backgroundColor(UIViewTag tag,
                                          const Composition::Color &color,
                                          bool transition,
                                          float duration){
    Entry entry {};
    entry.kind = Entry::Kind::BackgroundColor;
    entry.viewTag = tag;
    entry.color = color;
    entry.transition = transition;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::border(UIViewTag tag,bool use){
    Entry entry {};
    entry.kind = Entry::Kind::BorderEnabled;
    entry.viewTag = tag;
    entry.boolValue = use;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::borderColor(UIViewTag tag,
                                      const Composition::Color &color,
                                      bool transition,
                                      float duration){
    Entry entry {};
    entry.kind = Entry::Kind::BorderColor;
    entry.viewTag = tag;
    entry.color = color;
    entry.transition = transition;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::borderWidth(UIViewTag tag,
                                      float width,
                                      bool transition,
                                      float duration){
    Entry entry {};
    entry.kind = Entry::Kind::BorderWidth;
    entry.viewTag = tag;
    entry.floatValue = width;
    entry.transition = transition;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::dropShadow(UIViewTag tag,
                                     const Composition::LayerEffect::DropShadowParams & params,
                                     bool transition,
                                     float duration){
    Entry entry {};
    entry.kind = Entry::Kind::DropShadowEffect;
    entry.viewTag = tag;
    entry.dropShadowValue = params;
    entry.transition = transition;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::gaussianBlur(UIViewTag tag,
                                       float radius,
                                       bool transition,
                                       float duration){
    Entry entry {};
    entry.kind = Entry::Kind::GaussianBlurEffect;
    entry.viewTag = tag;
    entry.gaussianBlurValue = Composition::CanvasEffect::GaussianBlurParams{
            std::max(0.f,radius)
    };
    entry.transition = transition;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::directionalBlur(UIViewTag tag,
                                          float radius,
                                          float angle,
                                          bool transition,
                                          float duration){
    Entry entry {};
    entry.kind = Entry::Kind::DirectionalBlurEffect;
    entry.viewTag = tag;
    entry.directionalBlurValue = Composition::CanvasEffect::DirectionalBlurParams{
            std::max(0.f,radius),
            angle
    };
    entry.transition = transition;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::elementDropShadow(UIElementTag elementTag,
                                            const Composition::LayerEffect::DropShadowParams & params,
                                            bool transition,
                                            float duration){
    Entry entry {};
    entry.kind = Entry::Kind::DropShadowEffect;
    entry.elementTag = elementTag;
    entry.dropShadowValue = params;
    entry.transition = transition;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::elementGaussianBlur(UIElementTag elementTag,
                                              float radius,
                                              bool transition,
                                              float duration){
    Entry entry {};
    entry.kind = Entry::Kind::GaussianBlurEffect;
    entry.elementTag = elementTag;
    entry.gaussianBlurValue = Composition::CanvasEffect::GaussianBlurParams{
            std::max(0.f,radius)
    };
    entry.transition = transition;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::elementDirectionalBlur(UIElementTag elementTag,
                                                 float radius,
                                                 float angle,
                                                 bool transition,
                                                 float duration){
    Entry entry {};
    entry.kind = Entry::Kind::DirectionalBlurEffect;
    entry.elementTag = elementTag;
    entry.directionalBlurValue = Composition::CanvasEffect::DirectionalBlurParams{
            std::max(0.f,radius),
            angle
    };
    entry.transition = transition;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::elementBrush(UIElementTag elementTag,
                                       SharedHandle<Composition::Brush> brush,
                                       bool transition,
                                       float duration){
    Entry entry {};
    entry.kind = Entry::Kind::ElementBrush;
    entry.elementTag = elementTag;
    entry.brush = brush;
    entry.transition = transition;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::elementBrushAnimation(SharedHandle<Composition::Brush> brush,
                                                ElementAnimationKey key,
                                                SharedHandle<Composition::AnimationCurve> curve,
                                                float duration){
    Entry entry {};
    entry.kind = Entry::Kind::ElementBrushAnimation;
    entry.brush = brush;
    entry.animationKey = key;
    entry.curve = curve;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::elementAnimation(UIElementTag elementTag,
                                           ElementAnimationKey key,
                                           SharedHandle<Composition::AnimationCurve> curve,
                                           float duration){
    Entry entry {};
    entry.kind = Entry::Kind::ElementAnimation;
    entry.elementTag = elementTag;
    entry.animationKey = key;
    entry.curve = curve;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::elementPathAnimation(UIElementTag elementTag,
                                               SharedHandle<Composition::AnimationCurve> curve,
                                               int nodeIndex,
                                               float duration){
    Entry entry {};
    entry.kind = Entry::Kind::ElementPathAnimation;
    entry.elementTag = elementTag;
    entry.curve = curve;
    entry.nodeIndex = nodeIndex;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::textFont(UIElementTag elementTag,
                                   SharedHandle<Composition::Font> font){
    Entry entry {};
    entry.kind = Entry::Kind::TextFont;
    entry.elementTag = elementTag;
    entry.font = font;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::textColor(UIElementTag elementTag,
                                    const Composition::Color & color,
                                    bool transition,
                                    float duration){
    Entry entry {};
    entry.kind = Entry::Kind::TextColor;
    entry.elementTag = elementTag;
    entry.color = color;
    entry.transition = transition;
    entry.duration = duration;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::textAlignment(UIElementTag elementTag,
                                        Composition::TextLayoutDescriptor::Alignment alignment){
    Entry entry {};
    entry.kind = Entry::Kind::TextAlignment;
    entry.elementTag = elementTag;
    entry.textAlignment = alignment;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::textWrapping(UIElementTag elementTag,
                                       Composition::TextLayoutDescriptor::Wrapping wrapping){
    Entry entry {};
    entry.kind = Entry::Kind::TextWrapping;
    entry.elementTag = elementTag;
    entry.textWrapping = wrapping;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::textLineLimit(UIElementTag elementTag,unsigned lineLimit){
    Entry entry {};
    entry.kind = Entry::Kind::TextLineLimit;
    entry.elementTag = elementTag;
    entry.uintValue = lineLimit;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::layoutWidth(UIElementTag elementTag,LayoutLength width){
    Entry entry {};
    entry.kind = Entry::Kind::LayoutWidth;
    entry.elementTag = elementTag;
    entry.layoutLengthValue = width;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::layoutHeight(UIElementTag elementTag,LayoutLength height){
    Entry entry {};
    entry.kind = Entry::Kind::LayoutHeight;
    entry.elementTag = elementTag;
    entry.layoutLengthValue = height;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::layoutSize(UIElementTag elementTag,LayoutLength width,LayoutLength height){
    {
        Entry entry {};
        entry.kind = Entry::Kind::LayoutWidth;
        entry.elementTag = elementTag;
        entry.layoutLengthValue = width;
        entries.push_back(entry);
    }
    {
        Entry entry {};
        entry.kind = Entry::Kind::LayoutHeight;
        entry.elementTag = elementTag;
        entry.layoutLengthValue = height;
        entries.push_back(entry);
    }
    return copy();
}

StyleSheetPtr StyleSheet::layoutMargin(UIElementTag elementTag,LayoutEdges margin){
    Entry entry {};
    entry.kind = Entry::Kind::LayoutMargin;
    entry.elementTag = elementTag;
    entry.layoutEdgesValue = margin;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::layoutPadding(UIElementTag elementTag,LayoutEdges padding){
    Entry entry {};
    entry.kind = Entry::Kind::LayoutPadding;
    entry.elementTag = elementTag;
    entry.layoutEdgesValue = padding;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::layoutClamp(UIElementTag elementTag,LayoutClamp clamp){
    Entry entry {};
    entry.kind = Entry::Kind::LayoutClamp;
    entry.elementTag = elementTag;
    entry.layoutClampValue = clamp;
    entries.push_back(entry);
    return copy();
}

StyleSheetPtr StyleSheet::layoutTransition(UIElementTag elementTag,LayoutTransitionSpec spec){
    Entry entry {};
    entry.kind = Entry::Kind::LayoutTransition;
    entry.elementTag = elementTag;
    entry.layoutTransitionValue = spec;
    entries.push_back(entry);
    return copy();
}

// ---------------------------------------------------------------------------
// UIViewLayoutV2
// ---------------------------------------------------------------------------

UIViewLayoutV2 & UIViewLayoutV2::element(const UIElementLayoutSpec & spec){
    auto it = std::find_if(elements_.begin(),elements_.end(),[&](const UIElementLayoutSpec & e){
        return e.tag == spec.tag;
    });
    if(it != elements_.end()){
        *it = spec;
    }
    else {
        elements_.push_back(spec);
    }
    return *this;
}

bool UIViewLayoutV2::remove(UIElementTag tag){
    auto it = std::find_if(elements_.begin(),elements_.end(),[&](const UIElementLayoutSpec & e){
        return e.tag == tag;
    });
    if(it == elements_.end()){
        return false;
    }
    elements_.erase(it);
    return true;
}

void UIViewLayoutV2::clear(){
    elements_.clear();
}

const OmegaCommon::Vector<UIElementLayoutSpec> & UIViewLayoutV2::elements() const{
    return elements_;
}

bool UIViewLayoutV2::hasElement(UIElementTag tag) const{
    return std::find_if(elements_.begin(),elements_.end(),[&](const UIElementLayoutSpec & e){
        return e.tag == tag;
    }) != elements_.end();
}

UIView::UIView(const Core::Rect &rect,ViewPtr parent,UIViewTag tag):
View(rect,parent),
tag(tag){
    rootCanvas = makeCanvas(getLayerTree()->getRootLayer());
    enable();
}

UIViewLayout & UIView::layout(){
    layoutDirty = true;
    firstFrameCoherentSubmit = true;
    return currentLayout;
}

void UIView::setLayout(const UIViewLayout &layout){
    currentLayout = layout;
    convertLegacyLayoutToV2();
}

void UIView::convertLegacyLayoutToV2(){
    UIViewLayoutV2 v2 {};
    for(const auto & element : currentLayout.elements()){
        UIElementLayoutSpec spec {};
        spec.tag = element.tag;
        spec.style.width = LayoutLength::Auto();
        spec.style.height = LayoutLength::Auto();
        if(element.shape){
            spec.shape = element.shape;
        }
        if(element.str){
            spec.text = element.str;
        }
        if(element.textRect){
            spec.textRect = element.textRect;
        }
        if(element.textStyleTag){
            spec.textStyleTag = element.textStyleTag;
        }
        v2.element(spec);
    }
    currentLayoutV2_ = std::move(v2);
    layoutDirty = true;
    markAllElementsDirty();
    firstFrameCoherentSubmit = true;
}

void UIView::setStyleSheet(const StyleSheetPtr &style){
    const auto previousScope = collectStyleScope(currentStyle,tag);
    const auto nextScope = collectStyleScope(style,tag);

    currentStyle = style;
    styleDirty = true;
    styleDirtyGlobal = previousScope.touchesAllElements || nextScope.touchesAllElements;
    styleChangeRequiresCoherentFrame = styleDirtyGlobal;

    if(styleDirtyGlobal){
        markAllElementsDirty();
        firstFrameCoherentSubmit = true;
        return;
    }

    OmegaCommon::Vector<UIElementTag> affectedTags = previousScope.elementTags;
    for(const auto & nextTag : nextScope.elementTags){
        addUniqueTag(affectedTags,nextTag);
    }
    OmegaCommon::Vector<UIElementTag> expandedTags = affectedTags;
    for(const auto & spec : currentLayoutV2_.elements()){
        if(!spec.text || !spec.textStyleTag){
            continue;
        }
        if(containsTag(affectedTags,*spec.textStyleTag)){
            addUniqueTag(expandedTags,spec.tag);
        }
    }
    for(const auto & affectedTag : expandedTags){
        markElementDirty(affectedTag,false,true,true,false,false);
    }
}

StyleSheetPtr UIView::getStyleSheet() const{
    return currentStyle;
}

const UIView::UpdateDiagnostics & UIView::getLastUpdateDiagnostics() const{
    return lastUpdateDiagnostics;
}

const UIView::AnimationDiagnostics & UIView::getLastAnimationDiagnostics() const{
    return lastAnimationDiagnostics;
}

UIViewLayoutV2 & UIView::layoutV2(){
    layoutDirty = true;
    firstFrameCoherentSubmit = true;
    return currentLayoutV2_;
}

void UIView::setLayoutV2(const UIViewLayoutV2 & layout){
    currentLayoutV2_ = layout;
    layoutDirty = true;
    markAllElementsDirty();
    firstFrameCoherentSubmit = true;
}

void UIView::setDiagnosticSink(LayoutDiagnosticSink * sink){
    diagnosticSink_ = sink;
}

void UIView::applyLayoutDelta(const UIElementTag & elementTag,
                              const LayoutDelta & delta,
                              const LayoutTransitionSpec & spec){
    if(!spec.enabled || spec.durationSec <= 0.f || delta.changedProperties.empty()){
        return;
    }
    auto layerAnimator = ensureAnimationLayerAnimator(elementTag);
    if(layerAnimator == nullptr){
        return;
    }

    int dx = static_cast<int>(delta.toRectPx.pos.x - delta.fromRectPx.pos.x);
    int dy = static_cast<int>(delta.toRectPx.pos.y - delta.fromRectPx.pos.y);
    int dw = static_cast<int>(delta.toRectPx.w - delta.fromRectPx.w);
    int dh = static_cast<int>(delta.toRectPx.h - delta.fromRectPx.h);

    if(dx == 0 && dy == 0 && dw == 0 && dh == 0){
        return;
    }

    unsigned durationMs = static_cast<unsigned>(spec.durationSec * 1000.f);
    if(durationMs == 0){
        return;
    }

    auto curve = spec.curve;
    if(curve == nullptr){
        curve = Composition::AnimationCurve::Linear();
    }
    layerAnimator->resizeTransition(dx,dy,dw,dh,durationMs,curve);
}

void UIView::markAllElementsDirty(){
    for(auto & entry : elementDirtyState){
        entry.second.layoutDirty = true;
        entry.second.styleDirty = true;
        entry.second.contentDirty = true;
        entry.second.orderDirty = true;
        entry.second.visibilityDirty = true;
    }
}

void UIView::markElementDirty(const UIElementTag &tag,
                              bool layout,
                              bool style,
                              bool content,
                              bool order,
                              bool visibility){
    auto & dirty = elementDirtyState[tag];
    dirty.layoutDirty = dirty.layoutDirty || layout;
    dirty.styleDirty = dirty.styleDirty || style;
    dirty.contentDirty = dirty.contentDirty || content;
    dirty.orderDirty = dirty.orderDirty || order;
    dirty.visibilityDirty = dirty.visibilityDirty || visibility;
}

bool UIView::isElementDirty(const UIElementTag &tag) const{
    auto it = elementDirtyState.find(tag);
    if(it == elementDirtyState.end()){
        return true;
    }
    const auto & dirty = it->second;
    return dirty.layoutDirty ||
           dirty.styleDirty ||
           dirty.contentDirty ||
           dirty.orderDirty ||
           dirty.visibilityDirty;
}

void UIView::clearElementDirty(const UIElementTag &tag){
    auto it = elementDirtyState.find(tag);
    if(it == elementDirtyState.end()){
        return;
    }
    it->second.layoutDirty = false;
    it->second.styleDirty = false;
    it->second.contentDirty = false;
    it->second.orderDirty = false;
    it->second.visibilityDirty = false;
}

SharedHandle<Composition::ViewAnimator> UIView::ensureAnimationViewAnimator(){
    if(animationViewAnimator != nullptr){
        return animationViewAnimator;
    }
    animationViewAnimator = SharedHandle<Composition::ViewAnimator>(
            new Composition::ViewAnimator(compositorProxy()));
    return animationViewAnimator;
}

SharedHandle<Composition::LayerAnimator> UIView::ensureAnimationLayerAnimator(const UIElementTag & tag){
    auto existing = animationLayerAnimators.find(tag);
    if(existing != animationLayerAnimators.end() && existing->second != nullptr){
        return existing->second;
    }

    auto viewAnimator = ensureAnimationViewAnimator();
    if(viewAnimator == nullptr){
        return nullptr;
    }

    SharedHandle<Composition::Layer> layer = nullptr;
    if(getLayerTree() != nullptr){
        layer = getLayerTree()->getRootLayer();
    }
    if(layer == nullptr){
        return nullptr;
    }

    auto layerAnimator = viewAnimator->layerAnimator(*layer);
    animationLayerAnimators[tag] = layerAnimator;
    return layerAnimator;
}

Composition::AnimationHandle UIView::beginCompositionClock(const UIElementTag & tag,
                                                           float durationSec,
                                                           SharedHandle<Composition::AnimationCurve> curve){
    if(durationSec <= 0.f){
        return {};
    }

    auto layerAnimator = ensureAnimationLayerAnimator(tag);
    if(layerAnimator == nullptr){
        return {};
    }

    Composition::LayerEffect::TransformationParams identity {};
    identity.translate = {0.f,0.f,0.f};
    identity.rotate = {0.f,0.f,0.f};
    identity.scale = {1.f,1.f,1.f};

    Composition::KeyframeValue<Composition::LayerEffect::TransformationParams> startKey {};
    startKey.offset = 0.f;
    startKey.value = identity;
    startKey.easingToNext = curve != nullptr ? curve : Composition::AnimationCurve::Linear();
    Composition::KeyframeValue<Composition::LayerEffect::TransformationParams> endKey {};
    endKey.offset = 1.f;
    endKey.value = identity;

    Composition::LayerClip clip {};
    clip.transform = Composition::KeyframeTrack<Composition::LayerEffect::TransformationParams>::From({
            startKey,
            endKey
    });

    Composition::TimingOptions timing {};
    timing.durationMs = static_cast<std::uint32_t>(std::max(1.f,durationSec * 1000.f));
    timing.frameRateHint = static_cast<std::uint16_t>(std::max(1,framesPerSec));
    timing.clockMode = Composition::ClockMode::Hybrid;
    timing.preferResizeSafeBudget = true;
    timing.maxCatchupSteps = 1;

    const auto laneId = compositorProxy().getSyncLaneId();
    if(laneId != 0){
        return layerAnimator->animateOnLane(clip,timing,laneId);
    }
    return layerAnimator->animate(clip,timing);
}

void UIView::startOrUpdateAnimation(const UIElementTag &tag,
                                    int key,
                                    float from,
                                    float to,
                                    float durationSec,
                                    SharedHandle<Composition::AnimationCurve> curve){
    if(!std::isfinite(from) || !std::isfinite(to)){
        return;
    }

    auto tagIt = elementAnimations.find(tag);
    if(durationSec <= 0.f || std::fabs(to - from) <= 0.0001f){
        if(tagIt != elementAnimations.end()){
            auto existing = tagIt->second.find(key);
            if(existing != tagIt->second.end() && existing->second.compositionHandle.valid()){
                existing->second.compositionHandle.cancel();
            }
            tagIt->second.erase(key);
            if(tagIt->second.empty()){
                elementAnimations.erase(tagIt);
            }
        }
        return;
    }

    auto & propertyMap = elementAnimations[tag];
    auto & state = propertyMap[key];
    if(state.active && std::fabs(state.to - to) <= 0.0001f){
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float startValue = state.active ? state.value : from;
    state.active = true;
    state.from = startValue;
    state.to = to;
    state.value = startValue;
    state.lastProgress = 0.f;
    state.durationSec = std::max(0.001f,durationSec);
    state.startTime = now;
    state.curve = curve != nullptr ? curve : Composition::AnimationCurve::Linear();
    if(state.compositionHandle.valid()){
        state.compositionHandle.cancel();
    }
    state.compositionHandle = beginCompositionClock(tag,state.durationSec,state.curve);
    state.compositionClock = state.compositionHandle.valid();
    if(tag != kUIViewRootEffectTag){
        markElementDirty(tag,false,false,true,false,false);
    }
    styleDirty = true;
}

bool UIView::advanceAnimations(){
    auto now = std::chrono::steady_clock::now();
    bool changed = false;
    OmegaCommon::Vector<UIElementTag> removePropertyTags {};

    const auto laneDiagnostics = compositorProxy().getSyncLaneDiagnostics();
    const bool hasLaneDiagnostics = laneDiagnostics.syncLaneId != 0;
    bool staleSkipMode = false;
    if(hasLaneDiagnostics){
        const bool droppedCountIncreased = hasObservedLaneDiagnostics &&
                                           laneDiagnostics.droppedPacketCount > lastObservedDroppedPacketCount;
                        laneDiagnostics.inFlight > 0 ||
                        droppedCountIncreased;
        lastObservedDroppedPacketCount = laneDiagnostics.droppedPacketCount;
        hasObservedLaneDiagnostics = true;
    }
    else {
        hasObservedLaneDiagnostics = false;
        lastObservedDroppedPacketCount = 0;
    }

    std::uint64_t staleStepsSkippedThisTick = 0;
    std::uint64_t monotonicProgressClampsThisTick = 0;
    std::uint64_t activeTrackCount = 0;
    std::uint64_t completedTrackCountThisTick = 0;
    std::uint64_t cancelledTrackCountThisTick = 0;
    std::uint64_t failedTrackCountThisTick = 0;

    auto resolveProgress = [&](PropertyAnimationState & state) -> float {
        float elapsedSec = std::chrono::duration<float>(now - state.startTime).count();
        if(!std::isfinite(elapsedSec) || elapsedSec < 0.f){
            elapsedSec = 0.f;
        }
        float wallClockT = state.durationSec <= 0.f ? 1.f : clamp01(elapsedSec / state.durationSec);

        if(!state.compositionClock || !state.compositionHandle.valid()){
            float monotonicT = clamp01(wallClockT);
            if(monotonicT + 0.0001f < state.lastProgress){
                monotonicT = state.lastProgress;
                monotonicProgressClampsThisTick += 1;
            }
            state.lastProgress = monotonicT;
            return monotonicT;
        }

        auto handleState = state.compositionHandle.state();
        if(handleState == Composition::AnimationState::Cancelled ||
           handleState == Composition::AnimationState::Failed){
            state.compositionClock = false;
            if(handleState == Composition::AnimationState::Cancelled){
                cancelledTrackCountThisTick += 1;
            }
            else {
                failedTrackCountThisTick += 1;
            }
            state.compositionHandle = {};
            float monotonicT = clamp01(wallClockT);
            if(monotonicT + 0.0001f < state.lastProgress){
                monotonicT = state.lastProgress;
                monotonicProgressClampsThisTick += 1;
            }
            state.lastProgress = monotonicT;
            return monotonicT;
        }

        float compositionT = clamp01(state.compositionHandle.progress());
        if(handleState == Composition::AnimationState::Completed){
            compositionT = 1.f;
        }

        float resolvedT = wallClockT;
        if(staleSkipMode &&
           handleState != Composition::AnimationState::Completed){
            if(wallClockT > compositionT + 0.0001f){
                staleStepsSkippedThisTick += 1;
            }
            resolvedT = compositionT;
        }
        else {
            // Keep motion monotonic even when compositor telemetry is briefly conservative.
            resolvedT = std::max(wallClockT,compositionT);
        }

        resolvedT = clamp01(resolvedT);
        if(resolvedT + 0.0001f < state.lastProgress){
            resolvedT = state.lastProgress;
            monotonicProgressClampsThisTick += 1;
        }
        state.lastProgress = resolvedT;
        return resolvedT;
    };

    for(auto & tagEntry : elementAnimations){
        OmegaCommon::Vector<int> removeKeys {};
        for(auto & propertyEntry : tagEntry.second){
            auto & state = propertyEntry.second;
            if(!state.active){
                removeKeys.push_back(propertyEntry.first);
                continue;
            }
            activeTrackCount += 1;

            float t = resolveProgress(state);
            float sampled = state.curve != nullptr ? clamp01(state.curve->sample(t)) : t;
            float nextValue = state.from + ((state.to - state.from) * sampled);
            if(!std::isfinite(nextValue)){
                nextValue = state.to;
            }

            if(std::fabs(nextValue - state.value) > 0.0001f){
                state.value = nextValue;
                if(tagEntry.first != kUIViewRootEffectTag){
                    markElementDirty(tagEntry.first,false,false,true,false,false);
                }
                styleDirty = true;
                changed = true;
            }

            if(t >= 1.f){
                state.value = state.to;
                state.active = false;
                state.compositionClock = false;
                state.lastProgress = 1.f;
                state.compositionHandle = {};
                completedTrackCountThisTick += 1;
                if(tagEntry.first != kUIViewRootEffectTag){
                    markElementDirty(tagEntry.first,false,false,true,false,false);
                }
                styleDirty = true;
                changed = true;
                removeKeys.push_back(propertyEntry.first);
            }
        }

        for(const auto key : removeKeys){
            tagEntry.second.erase(key);
        }
        if(tagEntry.second.empty()){
            removePropertyTags.push_back(tagEntry.first);
        }
    }

    for(const auto & tagToRemove : removePropertyTags){
        elementAnimations.erase(tagToRemove);
    }

    OmegaCommon::Vector<UIElementTag> removePathTags {};
    for(auto & tagEntry : pathNodeAnimations){
        auto & nodeAnimations = tagEntry.second;
        OmegaCommon::Vector<PathNodeAnimationState> nextNodeAnimations {};
        nextNodeAnimations.reserve(nodeAnimations.size());
        bool tagChanged = false;

        for(auto nodeAnimation : nodeAnimations){
            auto advanceProperty = [&](PropertyAnimationState & propertyState) -> bool {
                if(!propertyState.active){
                    return false;
                }
                activeTrackCount += 1;

                float t = resolveProgress(propertyState);
                float sampled = propertyState.curve != nullptr ? clamp01(propertyState.curve->sample(t)) : t;
                float nextValue = propertyState.from + ((propertyState.to - propertyState.from) * sampled);
                if(!std::isfinite(nextValue)){
                    nextValue = propertyState.to;
                }
                bool propertyChanged = false;
                if(std::fabs(nextValue - propertyState.value) > 0.0001f){
                    propertyState.value = nextValue;
                    propertyChanged = true;
                }
                if(t >= 1.f){
                    propertyState.value = propertyState.to;
                    propertyState.active = false;
                    propertyState.compositionClock = false;
                    propertyState.lastProgress = 1.f;
                    propertyState.compositionHandle = {};
                    completedTrackCountThisTick += 1;
                    propertyChanged = true;
                }
                return propertyChanged;
            };

            bool xChanged = advanceProperty(nodeAnimation.x);
            bool yChanged = advanceProperty(nodeAnimation.y);
            bool nodeActive = nodeAnimation.x.active || nodeAnimation.y.active;
            if(nodeActive){
                nextNodeAnimations.push_back(nodeAnimation);
            }
            if(xChanged || yChanged){
                tagChanged = true;
            }
        }

        nodeAnimations = std::move(nextNodeAnimations);
        if(nodeAnimations.empty()){
            removePathTags.push_back(tagEntry.first);
        }
        if(tagChanged){
            markElementDirty(tagEntry.first,false,false,true,false,false);
            changed = true;
        }
    }

    for(const auto & tagToRemove : removePathTags){
        pathNodeAnimations.erase(tagToRemove);
    }

    lastAnimationDiagnostics.syncLaneId = laneDiagnostics.syncLaneId;
    lastAnimationDiagnostics.tickCount += 1;
    lastAnimationDiagnostics.staleStepsSkipped += staleStepsSkippedThisTick;
    lastAnimationDiagnostics.monotonicProgressClamps += monotonicProgressClampsThisTick;
    lastAnimationDiagnostics.activeTrackCount = activeTrackCount;
    lastAnimationDiagnostics.completedTrackCount += completedTrackCountThisTick;
    lastAnimationDiagnostics.cancelledTrackCount += cancelledTrackCountThisTick;
    lastAnimationDiagnostics.failedTrackCount += failedTrackCountThisTick;
    lastAnimationDiagnostics.queuedPacketCount = laneDiagnostics.queuedPacketCount;
    lastAnimationDiagnostics.submittedPacketCount = laneDiagnostics.submittedPacketCount;
    lastAnimationDiagnostics.presentedPacketCount = laneDiagnostics.presentedPacketCount;
    lastAnimationDiagnostics.droppedPacketCount = laneDiagnostics.droppedPacketCount;
    lastAnimationDiagnostics.failedPacketCount = laneDiagnostics.failedPacketCount;
    lastAnimationDiagnostics.lastSubmittedPacketId = laneDiagnostics.lastSubmittedPacketId;
    lastAnimationDiagnostics.lastPresentedPacketId = laneDiagnostics.lastPresentedPacketId;
    lastAnimationDiagnostics.inFlight = laneDiagnostics.inFlight;
    lastAnimationDiagnostics.staleSkipMode = staleSkipMode;

    return changed;
}

Core::Optional<float> UIView::animatedValue(const UIElementTag &tag,int key) const{
    auto tagIt = elementAnimations.find(tag);
    if(tagIt == elementAnimations.end()){
        return {};
    }
    auto propertyIt = tagIt->second.find(key);
    if(propertyIt == tagIt->second.end()){
        return {};
    }
    return propertyIt->second.value;
}

Composition::Color UIView::applyAnimatedColor(const UIElementTag &tag,const Composition::Color &baseColor) const{
    Composition::Color output = baseColor;
    if(auto value = animatedValue(tag,ElementAnimationKeyColorRed); value){
        output.r = clamp01(*value);
    }
    if(auto value = animatedValue(tag,ElementAnimationKeyColorGreen); value){
        output.g = clamp01(*value);
    }
    if(auto value = animatedValue(tag,ElementAnimationKeyColorBlue); value){
        output.b = clamp01(*value);
    }
    if(auto value = animatedValue(tag,ElementAnimationKeyColorAlpha); value){
        output.a = clamp01(*value);
    }
    return output;
}

Shape UIView::applyAnimatedShape(const UIElementTag &tag,const Shape &inputShape) const{
    Shape output = inputShape;
    if(auto value = animatedValue(tag,ElementAnimationKeyWidth); value){
        (void)applyShapeDimension(output,ElementAnimationKeyWidth,*value);
    }
    if(auto value = animatedValue(tag,ElementAnimationKeyHeight); value){
        (void)applyShapeDimension(output,ElementAnimationKeyHeight,*value);
    }

    auto pathAnimIt = pathNodeAnimations.find(tag);
    if(pathAnimIt != pathNodeAnimations.end() &&
       output.type == Shape::Type::Path &&
       output.path){
        auto points = pathToPoints(*output.path);
        for(const auto & nodeAnimation : pathAnimIt->second){
            if(nodeAnimation.nodeIndex < 0){
                continue;
            }
            auto nodeIndex = static_cast<std::size_t>(nodeAnimation.nodeIndex);
            if(nodeIndex >= points.size()){
                continue;
            }
            points[nodeIndex].x = nodeAnimation.x.value;
            points[nodeIndex].y = nodeAnimation.y.value;
        }
        auto rebuiltPath = pointsToPath(points);
        if(rebuiltPath){
            output.path = *rebuiltPath;
        }
    }
    return output;
}

SharedHandle<Composition::Font> UIView::resolveFallbackTextFont(){
    if(fallbackTextFont != nullptr){
        return fallbackTextFont;
    }

    auto fontEngine = Composition::FontEngine::inst();
    if(fontEngine == nullptr){
        return nullptr;
    }

    Composition::FontDescriptor descriptor("Helvetica",18,Composition::FontDescriptor::Regular);
    fallbackTextFont = fontEngine->CreateFont(descriptor);
    if(fallbackTextFont == nullptr){
        Composition::FontDescriptor secondaryDescriptor("Arial",18,Composition::FontDescriptor::Regular);
        fallbackTextFont = fontEngine->CreateFont(secondaryDescriptor);
    }
    return fallbackTextFont;
}

void UIView::update(){
    if(rootCanvas == nullptr){
        rootCanvas = makeCanvas(getLayerTree()->getRootLayer());
        firstFrameCoherentSubmit = true;
    }

    const auto & v2Elements = currentLayoutV2_.elements();
    if(v2Elements.empty()){
        layoutDirty = false;
        styleDirty = false;
        styleDirtyGlobal = false;
        styleChangeRequiresCoherentFrame = false;
        firstFrameCoherentSubmit = false;
        ++lastUpdateDiagnostics.revision;
        return;
    }

    const auto localBounds = localBoundsFromView(this);
    const float dpiScale = 1.f;
    LayoutContext ctx {};
    ctx.availableRectPx = localBounds;
    ctx.dpiScale = dpiScale;
    const auto availDp = ctx.availableRectDp();

    OmegaCommon::Vector<StyleRule> layoutRules {};
    if(currentStyle != nullptr){
        layoutRules = convertEntriesToRules(*currentStyle,tag);
    }

    struct V2Resolved {
        UIElementTag tag;
        const UIElementLayoutSpec * spec;
        Core::Rect resolvedRectDp {};
        Core::Rect resolvedRectPx {};
        int zIndex = 0;
        std::size_t insertionOrder = 0;
    };
    OmegaCommon::Vector<V2Resolved> resolved {};
    resolved.reserve(v2Elements.size());

    for(std::size_t i = 0; i < v2Elements.size(); ++i){
        const auto & spec = v2Elements[i];
        LayoutStyle effectiveStyle = spec.style;
        mergeLayoutRulesIntoStyle(effectiveStyle,layoutRules,spec.tag);

        Core::Rect rectDp = resolveClampedRect(effectiveStyle,availDp,dpiScale);
        Core::Rect rectPx {
            Core::Position{rectDp.pos.x * dpiScale,rectDp.pos.y * dpiScale},
            rectDp.w * dpiScale,
            rectDp.h * dpiScale
        };

        if(diagnosticSink_ != nullptr){
            diagnosticSink_->record(LayoutDiagnosticEntry{
                spec.tag,rectDp,rectPx,LayoutDiagnosticEntry::Pass::Arrange});
        }

        V2Resolved entry {};
        entry.tag = spec.tag;
        entry.spec = &spec;
        entry.resolvedRectDp = rectDp;
        entry.resolvedRectPx = rectPx;
        entry.zIndex = spec.zIndex;
        entry.insertionOrder = i;
        resolved.push_back(entry);
    }

    std::stable_sort(resolved.begin(),resolved.end(),
        [](const V2Resolved & a,const V2Resolved & b){
            if(a.zIndex != b.zIndex){
                return a.zIndex < b.zIndex;
            }
            return a.insertionOrder < b.insertionOrder;
        });

    OmegaCommon::Vector<UIElementTag> nextOrder {};
    nextOrder.reserve(resolved.size());
    for(const auto & r : resolved){
        nextOrder.push_back(r.tag);
    }

    OmegaCommon::Vector<UIElementTag> previousOrder = activeTagOrder;
    const bool orderChanged = previousOrder.size() != nextOrder.size() ||
                              !std::equal(previousOrder.begin(),previousOrder.end(),
                                          nextOrder.begin(),nextOrder.end());
    activeTagOrder = nextOrder;

    startCompositionSession();

    auto viewStyle = resolveViewStyle(currentStyle,tag);
    auto backgroundColor = viewStyle.backgroundColor.value_or(Composition::Color::Transparent);

    auto & rootBackground = rootCanvas->getCurrentFrame()->background;
    rootBackground.r = backgroundColor.r;
    rootBackground.g = backgroundColor.g;
    rootBackground.b = backgroundColor.b;
    rootBackground.a = backgroundColor.a;

    // Draw background fill.
    auto rootBgBrush = Composition::ColorBrush(backgroundColor);
    auto rootBgRect = localBounds;
    rootCanvas->drawRect(rootBgRect, rootBgBrush);

    ChildResizeSpec layoutClamp {};
    layoutClamp.resizable = true;
    layoutClamp.policy = ChildResizePolicy::FitContent;

    // All elements draw to rootCanvas in z-order.
    for(const auto & entry : resolved){
        const auto & spec = *entry.spec;

        auto previousRectIt = lastResolvedV2Rects_.find(entry.tag);
        Core::Rect previousRectPx = (previousRectIt != lastResolvedV2Rects_.end())
                                        ? previousRectIt->second
                                        : entry.resolvedRectPx;
        LayoutDelta delta = computeLayoutDelta(previousRectPx,entry.resolvedRectPx);
        lastResolvedV2Rects_[entry.tag] = entry.resolvedRectPx;

        if(!delta.changedProperties.empty()){
            auto transSpec = resolveLayoutTransition(layoutRules,entry.tag);
            if(transSpec && transSpec->enabled){
                applyLayoutDelta(entry.tag,delta,*transSpec);
            }
        }

        if(diagnosticSink_ != nullptr){
            diagnosticSink_->record(LayoutDiagnosticEntry{
                entry.tag,entry.resolvedRectDp,entry.resolvedRectPx,
                LayoutDiagnosticEntry::Pass::Commit});
        }

        // Resolve per-element effects (shadow).
        auto effectStyle = resolveElementEffectStyle(currentStyle,tag,entry.tag);

        if(spec.shape){
            auto shapeToDraw = *spec.shape;
            auto brush = resolveElementBrush(currentStyle,tag,entry.tag);

            // Draw shadow before the element (if any).
            if(effectStyle.dropShadow){
                auto shadowParams = *effectStyle.dropShadow;
                // Apply animated shadow properties if active.
                if(auto v = animatedValue(entry.tag,EffectAnimationKeyShadowOffsetX); v)
                    shadowParams.x_offset = *v;
                if(auto v = animatedValue(entry.tag,EffectAnimationKeyShadowOffsetY); v)
                    shadowParams.y_offset = *v;
                if(auto v = animatedValue(entry.tag,EffectAnimationKeyShadowRadius); v)
                    shadowParams.radius = *v;
                if(auto v = animatedValue(entry.tag,EffectAnimationKeyShadowBlur); v)
                    shadowParams.blurAmount = *v;
                if(auto v = animatedValue(entry.tag,EffectAnimationKeyShadowOpacity); v)
                    shadowParams.opacity = *v;

                switch(shapeToDraw.type){
                    case Shape::Type::Rect: {
                        auto rect = shapeToDraw.rect;
                        rect = ViewResizeCoordinator::clampRectToParent(rect,localBounds,layoutClamp);
                        rootCanvas->drawShadow(rect,shadowParams);
                        break;
                    }
                    case Shape::Type::RoundedRect: {
                        auto rr = shapeToDraw.roundedRect;
                        Core::Rect clampedRect {rr.pos,rr.w,rr.h};
                        clampedRect = ViewResizeCoordinator::clampRectToParent(clampedRect,localBounds,layoutClamp);
                        rr.pos = clampedRect.pos;
                        rr.w = clampedRect.w;
                        rr.h = clampedRect.h;
                        rr.rad_x = std::min(rr.rad_x,rr.w * 0.5f);
                        rr.rad_y = std::min(rr.rad_y,rr.h * 0.5f);
                        rootCanvas->drawShadow(rr,shadowParams);
                        break;
                    }
                    case Shape::Type::Ellipse: {
                        const auto & srcEllipse = shapeToDraw.ellipse;
                        Core::Rect ellipseRect {
                            Core::Position{srcEllipse.x - srcEllipse.rad_x,srcEllipse.y - srcEllipse.rad_y},
                            std::max(1.f,srcEllipse.rad_x * 2.f),
                            std::max(1.f,srcEllipse.rad_y * 2.f)
                        };
                        ellipseRect = ViewResizeCoordinator::clampRectToParent(ellipseRect,localBounds,layoutClamp);
                        Core::Ellipse ellipse {
                            ellipseRect.pos.x + (ellipseRect.w * 0.5f),
                            ellipseRect.pos.y + (ellipseRect.h * 0.5f),
                            ellipseRect.w * 0.5f,
                            ellipseRect.h * 0.5f
                        };
                        rootCanvas->drawShadow(ellipse,shadowParams);
                        break;
                    }
                    default:
                        break;
                }
            }

            switch(shapeToDraw.type){
                case Shape::Type::Rect: {
                    auto rect = shapeToDraw.rect;
                    rect = ViewResizeCoordinator::clampRectToParent(rect,localBounds,layoutClamp);
                    rootCanvas->drawRect(rect,brush);
                    break;
                }
                case Shape::Type::RoundedRect: {
                    auto rect = shapeToDraw.roundedRect;
                    Core::Rect clampedRect {rect.pos,rect.w,rect.h};
                    clampedRect = ViewResizeCoordinator::clampRectToParent(clampedRect,localBounds,layoutClamp);
                    rect.pos = clampedRect.pos;
                    rect.w = clampedRect.w;
                    rect.h = clampedRect.h;
                    rect.rad_x = std::min(rect.rad_x,rect.w * 0.5f);
                    rect.rad_y = std::min(rect.rad_y,rect.h * 0.5f);
                    rootCanvas->drawRoundedRect(rect,brush);
                    break;
                }
                case Shape::Type::Ellipse: {
                    const auto & srcEllipse = shapeToDraw.ellipse;
                    Core::Rect ellipseRect {
                        Core::Position{
                            srcEllipse.x - srcEllipse.rad_x,
                            srcEllipse.y - srcEllipse.rad_y
                        },
                        std::max(1.f,srcEllipse.rad_x * 2.f),
                        std::max(1.f,srcEllipse.rad_y * 2.f)
                    };
                    ellipseRect = ViewResizeCoordinator::clampRectToParent(ellipseRect,localBounds,layoutClamp);
                    Core::Ellipse ellipse {
                        ellipseRect.pos.x + (ellipseRect.w * 0.5f),
                        ellipseRect.pos.y + (ellipseRect.h * 0.5f),
                        ellipseRect.w * 0.5f,
                        ellipseRect.h * 0.5f
                    };
                    rootCanvas->drawEllipse(ellipse,brush);
                    break;
                }
                case Shape::Type::Path: {
                    if(shapeToDraw.path){
                        auto vectorPath = *shapeToDraw.path;
                        auto path = Composition::Path(vectorPath,shapeToDraw.pathStrokeWidth);
                        if(shapeToDraw.closePath){
                            path.close();
                        }
                        path.setPathBrush(brush);
                        rootCanvas->drawPath(path);
                    }
                    break;
                }
                default:
                    break;
            }
        }
        else if(spec.text){
            UIElementTag textStyleTag = spec.textStyleTag.value_or(entry.tag);
            auto textStyle = resolveTextStyle(currentStyle,tag,textStyleTag);
            auto font = textStyle.font != nullptr ? textStyle.font : resolveFallbackTextFont();
            if(font != nullptr){
                auto textRect = spec.textRect.value_or(localBounds);
                textRect = ViewResizeCoordinator::clampRectToParent(textRect,localBounds,layoutClamp);
                auto unicodeText = UniString::fromUTF32(
                    reinterpret_cast<const UChar32 *>(spec.text->data()),
                    static_cast<int32_t>(spec.text->size()));
                auto textLayout = textStyle.layout;
                textLayout.lineLimit = textStyle.lineLimit;
                rootCanvas->drawText(unicodeText,font,textRect,textStyle.color,textLayout);
            }
        }
    }

    // Single sendFrame for all elements.
    rootCanvas->sendFrame();

    endCompositionSession();
    layoutDirty = false;
    styleDirty = false;
    styleDirtyGlobal = false;
    styleChangeRequiresCoherentFrame = false;
    firstFrameCoherentSubmit = false;
    ++lastUpdateDiagnostics.revision;
}


}
