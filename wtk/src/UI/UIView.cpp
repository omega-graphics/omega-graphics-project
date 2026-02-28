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

    auto limb = view->getLayerTreeLimb();
    if(limb != nullptr && limb->getRootLayer() != nullptr){
        const auto & limbRect = limb->getRootLayer()->getLayerRect();
        limbWidth = limbRect.w;
        limbHeight = limbRect.h;
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

UIRenderer::UIRenderer(UIView *view):
view(view){

}

UIRenderer::RenderTargetBundle & UIRenderer::buildLayerRenderTarget(UIElementTag tag){
    auto entry = renderTargetStore.find(tag);
    if(entry != renderTargetStore.end()){
        if(entry->second.layer != nullptr){
            auto localRect = localBoundsFromView(view);
            auto currentRect = entry->second.layer->getLayerRect();
            if(currentRect.pos.x != localRect.pos.x ||
               currentRect.pos.y != localRect.pos.y ||
               currentRect.w != localRect.w ||
               currentRect.h != localRect.h){
                entry->second.layer->resize(localRect);
            }
        }
        return entry->second;
    }

    auto layerRect = localBoundsFromView(view);
    auto layer = view->makeLayer(layerRect);
    auto canvas = view->makeCanvas(layer);
    renderTargetStore[tag] = {layer,canvas};
    return renderTargetStore[tag];
}

void UIRenderer::handleElement(UIElementTag tag){
    auto & target = buildLayerRenderTarget(tag);
    if(target.canvas != nullptr){
        target.canvas->sendFrame();
    }
}

void UIRenderer::handleTransition(UIElementTag tag,ElementAnimationKey k,float duration){
    (void)tag;
    (void)k;
    (void)duration;
}

void UIRenderer::handleAnimation(UIElementTag tag,
                                 ElementAnimationKey k,
                                 float duration,
                                 SharedHandle<Composition::AnimationCurve> &curve){
    (void)tag;
    (void)k;
    (void)duration;
    (void)curve;
}

UIView::UIView(const Core::Rect &rect,Composition::LayerTree *layerTree,ViewPtr parent,UIViewTag tag):
CanvasView(rect,layerTree,parent),
UIRenderer(this),
tag(tag){
    rootCanvas = makeCanvas(getLayerTreeLimb()->getRootLayer());
    // UIView is a pure View (not a Widget), so ensure it is visible when attached.
    enable();
}

UIViewLayout & UIView::layout(){
    layoutDirty = true;
    markRootDirty();
    firstFrameCoherentSubmit = true;
    return currentLayout;
}

void UIView::setLayout(const UIViewLayout &layout){
    currentLayout = layout;
    layoutDirty = true;
    markRootDirty();
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

    if(previousScope.touchesRoot || nextScope.touchesRoot){
        rootStyleDirty = true;
        rootContentDirty = true;
    }

    if(styleDirtyGlobal){
        markAllElementsDirty();
        firstFrameCoherentSubmit = true;
        return;
    }

    OmegaCommon::Vector<UIElementTag> affectedTags = previousScope.elementTags;
    for(const auto & nextTag : nextScope.elementTags){
        addUniqueTag(affectedTags,nextTag);
    }
    // Text style selectors may target a shared style tag instead of the concrete element tag.
    OmegaCommon::Vector<UIElementTag> expandedTags = affectedTags;
    for(const auto & element : currentLayout.elements()){
        if(element.type != UIViewLayout::Element::Type::Text){
            continue;
        }
        if(!element.textStyleTag){
            continue;
        }
        if(containsTag(affectedTags,*element.textStyleTag)){
            addUniqueTag(expandedTags,element.tag);
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

void UIView::markRootDirty(){
    rootLayoutDirty = true;
    rootStyleDirty = true;
    rootContentDirty = true;
    rootOrderDirty = true;
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
    if(tag == kUIViewRootEffectTag){
        if(getLayerTreeLimb() != nullptr){
            layer = getLayerTreeLimb()->getRootLayer();
        }
    }
    else {
        auto & target = buildLayerRenderTarget(tag);
        layer = target.layer;
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

void UIView::syncElementDirtyState(const OmegaCommon::Vector<UIViewLayout::Element> &elements,
                                   bool layoutChanged,
                                   bool styleChanged,
                                   bool orderChanged){
    OmegaCommon::Vector<UIElementTag> activeTags {};
    activeTags.reserve(elements.size());
    for(const auto & element : elements){
        activeTags.push_back(element.tag);
        auto it = elementDirtyState.find(element.tag);
        const bool isNewTag = (it == elementDirtyState.end());
        if(isNewTag){
            elementDirtyState[element.tag] = ElementDirtyState{};
        }

        markElementDirty(
                element.tag,
                layoutChanged || isNewTag,
                styleChanged || isNewTag,
                layoutChanged || isNewTag,
                orderChanged || isNewTag,
                layoutChanged || styleChanged || isNewTag);
    }

    for(auto & entry : elementDirtyState){
        if(!containsTag(activeTags,entry.first)){
            entry.second.visibilityDirty = true;
        }
    }

    activeTagOrder = std::move(activeTags);
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
    if(tag == kUIViewRootEffectTag){
        rootStyleDirty = true;
        rootContentDirty = true;
    }
    else {
        markElementDirty(tag,false,false,true,false,false);
    }
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
        staleSkipMode = laneDiagnostics.resizeBudgetActive ||
                        laneDiagnostics.underPressure ||
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
                if(tagEntry.first == kUIViewRootEffectTag){
                    rootStyleDirty = true;
                    rootContentDirty = true;
                }
                else {
                    markElementDirty(tagEntry.first,false,false,true,false,false);
                }
                changed = true;
            }

            if(t >= 1.f){
                state.value = state.to;
                state.active = false;
                state.compositionClock = false;
                state.lastProgress = 1.f;
                state.compositionHandle = {};
                completedTrackCountThisTick += 1;
                if(tagEntry.first == kUIViewRootEffectTag){
                    rootStyleDirty = true;
                    rootContentDirty = true;
                }
                else {
                    markElementDirty(tagEntry.first,false,false,true,false,false);
                }
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
    lastAnimationDiagnostics.laneUnderPressure = laneDiagnostics.underPressure;
    lastAnimationDiagnostics.resizeBudgetActive = laneDiagnostics.resizeBudgetActive;

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

void UIView::prepareElementAnimations(const OmegaCommon::Vector<UIViewLayout::Element> &elements,
                                      bool layoutChanged,
                                      bool styleChanged){
    OmegaCommon::Map<UIElementTag,Shape> currentShapes {};
    OmegaCommon::Map<UIElementTag,Composition::Color> targetColors {};
    OmegaCommon::Map<UIElementTag,UIElementTag> textStyleSelectorByElement {};

    for(const auto & element : elements){
        if(element.type == UIViewLayout::Element::Type::Shape && element.shape){
            currentShapes[element.tag] = *element.shape;
            auto brush = resolveElementBrush(currentStyle,tag,element.tag);
            auto color = colorFromBrush(brush);
            if(color){
                targetColors[element.tag] = *color;
            }
        }
        else if(element.type == UIViewLayout::Element::Type::Text && element.str){
            UIElementTag styleTag = element.textStyleTag.value_or(element.tag);
            textStyleSelectorByElement[element.tag] = styleTag;
            auto textStyle = resolveTextStyle(currentStyle,tag,styleTag);
            targetColors[element.tag] = textStyle.color;
        }
    }

    if(currentStyle != nullptr && (layoutChanged || styleChanged)){
        const auto defaultCurve = Composition::AnimationCurve::Linear();
        const auto now = std::chrono::steady_clock::now();
        for(const auto & entry : currentStyle->entries){
            if(!entry.viewTag.empty() && !matchesTag(entry.viewTag,tag)){
                continue;
            }

            auto applyColorAnimation = [&](const UIElementTag & elementTag,
                                           ElementAnimationKey animationKey,
                                           float to,
                                           float durationSec,
                                           const SharedHandle<Composition::AnimationCurve> & curve){
                auto fromColorIt = lastResolvedElementColor.find(elementTag);
                auto targetColorIt = targetColors.find(elementTag);
                const float from = [&]() -> float {
                    if(fromColorIt != lastResolvedElementColor.end()){
                        switch(animationKey){
                            case ElementAnimationKeyColorRed: return fromColorIt->second.r;
                            case ElementAnimationKeyColorGreen: return fromColorIt->second.g;
                            case ElementAnimationKeyColorBlue: return fromColorIt->second.b;
                            case ElementAnimationKeyColorAlpha: return fromColorIt->second.a;
                            default: return to;
                        }
                    }
                    if(targetColorIt != targetColors.end()){
                        switch(animationKey){
                            case ElementAnimationKeyColorRed: return targetColorIt->second.r;
                            case ElementAnimationKeyColorGreen: return targetColorIt->second.g;
                            case ElementAnimationKeyColorBlue: return targetColorIt->second.b;
                            case ElementAnimationKeyColorAlpha: return targetColorIt->second.a;
                            default: return to;
                        }
                    }
                    return to;
                }();
                startOrUpdateAnimation(elementTag,
                                       animationKey,
                                       from,
                                       to,
                                       durationSec,
                                       curve != nullptr ? curve : defaultCurve);
            };

            auto applyDimensionAnimation = [&](const UIElementTag & elementTag,
                                               ElementAnimationKey animationKey,
                                               float durationSec,
                                               const SharedHandle<Composition::AnimationCurve> & curve){
                auto currentShapeIt = currentShapes.find(elementTag);
                auto prevShapeIt = previousShapeByTag.find(elementTag);
                if(currentShapeIt == currentShapes.end() || prevShapeIt == previousShapeByTag.end()){
                    return;
                }
                auto toValue = shapeDimension(currentShapeIt->second,animationKey);
                auto fromValue = shapeDimension(prevShapeIt->second,animationKey);
                if(!toValue || !fromValue){
                    return;
                }
                startOrUpdateAnimation(elementTag,
                                       animationKey,
                                       *fromValue,
                                       *toValue,
                                       durationSec,
                                       curve != nullptr ? curve : defaultCurve);
            };

            auto applyPathNodeAnimation = [&](const UIElementTag & elementTag,
                                              int nodeIndex,
                                              float durationSec,
                                              const SharedHandle<Composition::AnimationCurve> & curve){
                auto currentShapeIt = currentShapes.find(elementTag);
                auto prevShapeIt = previousShapeByTag.find(elementTag);
                if(currentShapeIt == currentShapes.end() || prevShapeIt == previousShapeByTag.end()){
                    return;
                }
                if(currentShapeIt->second.type != Shape::Type::Path ||
                   prevShapeIt->second.type != Shape::Type::Path ||
                   !currentShapeIt->second.path ||
                   !prevShapeIt->second.path){
                    return;
                }

                auto currentPoints = pathToPoints(*currentShapeIt->second.path);
                auto previousPoints = pathToPoints(*prevShapeIt->second.path);
                const auto maxIndex = std::min(currentPoints.size(),previousPoints.size());
                if(maxIndex == 0){
                    return;
                }

                const auto idx = static_cast<std::size_t>(nodeIndex);
                if(nodeIndex < 0 || idx >= maxIndex){
                    return;
                }

                auto & nodeAnimations = pathNodeAnimations[elementTag];
                auto existingNodeIt = std::find_if(nodeAnimations.begin(),
                                                   nodeAnimations.end(),
                                                   [&](const PathNodeAnimationState & state){
                                                       return state.nodeIndex == nodeIndex;
                                                   });
                if(existingNodeIt == nodeAnimations.end()){
                    nodeAnimations.push_back(PathNodeAnimationState{});
                    existingNodeIt = std::prev(nodeAnimations.end());
                    existingNodeIt->nodeIndex = nodeIndex;
                }

                auto startPathAnimationProperty =
                        [&](PropertyAnimationState & propertyState,float from,float to){
                    if(!std::isfinite(from) || !std::isfinite(to)){
                        if(propertyState.compositionHandle.valid()){
                            propertyState.compositionHandle.cancel();
                        }
                        propertyState.compositionClock = false;
                        propertyState.active = false;
                        propertyState.value = to;
                        propertyState.lastProgress = 1.f;
                        return;
                    }
                    if(durationSec <= 0.f || std::fabs(to - from) <= 0.0001f){
                        if(propertyState.compositionHandle.valid()){
                            propertyState.compositionHandle.cancel();
                        }
                        propertyState.compositionClock = false;
                        propertyState.active = false;
                        propertyState.value = to;
                        propertyState.lastProgress = 1.f;
                        return;
                    }
                    if(propertyState.active && std::fabs(propertyState.to - to) <= 0.0001f){
                        return;
                    }
                    const float startValue = propertyState.active ? propertyState.value : from;
                    propertyState.active = true;
                    propertyState.from = startValue;
                    propertyState.to = to;
                    propertyState.value = propertyState.from;
                    propertyState.lastProgress = 0.f;
                    propertyState.durationSec = std::max(0.001f,durationSec);
                    propertyState.startTime = now;
                    propertyState.curve = curve != nullptr ? curve : defaultCurve;
                    if(propertyState.compositionHandle.valid()){
                        propertyState.compositionHandle.cancel();
                    }
                    propertyState.compositionHandle = beginCompositionClock(
                            elementTag,
                            propertyState.durationSec,
                            propertyState.curve);
                    propertyState.compositionClock = propertyState.compositionHandle.valid();
                    markElementDirty(elementTag,false,false,true,false,false);
                };

                startPathAnimationProperty(existingNodeIt->x,
                                           previousPoints[idx].x,
                                           currentPoints[idx].x);
                startPathAnimationProperty(existingNodeIt->y,
                                           previousPoints[idx].y,
                                           currentPoints[idx].y);
            };

            if(entry.kind == StyleSheet::Entry::Kind::ElementBrush){
                if(!entry.transition || entry.duration <= 0.f){
                    continue;
                }
                for(const auto & colorEntry : targetColors){
                    if(!entry.elementTag.empty() && !matchesTag(entry.elementTag,colorEntry.first)){
                        continue;
                    }
                    auto previousColorIt = lastResolvedElementColor.find(colorEntry.first);
                    if(previousColorIt == lastResolvedElementColor.end() ||
                       colorsClose(previousColorIt->second,colorEntry.second)){
                        continue;
                    }

                    startOrUpdateAnimation(
                            colorEntry.first,
                            ElementAnimationKeyColorRed,
                            previousColorIt->second.r,
                            colorEntry.second.r,
                            entry.duration,
                            entry.curve != nullptr ? entry.curve : defaultCurve);
                    startOrUpdateAnimation(
                            colorEntry.first,
                            ElementAnimationKeyColorGreen,
                            previousColorIt->second.g,
                            colorEntry.second.g,
                            entry.duration,
                            entry.curve != nullptr ? entry.curve : defaultCurve);
                    startOrUpdateAnimation(
                            colorEntry.first,
                            ElementAnimationKeyColorBlue,
                            previousColorIt->second.b,
                            colorEntry.second.b,
                            entry.duration,
                            entry.curve != nullptr ? entry.curve : defaultCurve);
                    startOrUpdateAnimation(
                            colorEntry.first,
                            ElementAnimationKeyColorAlpha,
                            previousColorIt->second.a,
                            colorEntry.second.a,
                            entry.duration,
                            entry.curve != nullptr ? entry.curve : defaultCurve);
                }
                continue;
            }

            if(entry.kind == StyleSheet::Entry::Kind::TextColor){
                if(!entry.transition || entry.duration <= 0.f){
                    continue;
                }
                for(const auto & styleSelectorEntry : textStyleSelectorByElement){
                    const auto & elementTag = styleSelectorEntry.first;
                    const auto & styleSelectorTag = styleSelectorEntry.second;
                    if(!entry.elementTag.empty() &&
                       !matchesTag(entry.elementTag,styleSelectorTag) &&
                       !matchesTag(entry.elementTag,elementTag)){
                        continue;
                    }
                    auto targetColorIt = targetColors.find(elementTag);
                    auto previousColorIt = lastResolvedElementColor.find(elementTag);
                    if(targetColorIt == targetColors.end() ||
                       previousColorIt == lastResolvedElementColor.end() ||
                       colorsClose(previousColorIt->second,targetColorIt->second)){
                        continue;
                    }

                    startOrUpdateAnimation(
                            elementTag,
                            ElementAnimationKeyColorRed,
                            previousColorIt->second.r,
                            targetColorIt->second.r,
                            entry.duration,
                            entry.curve != nullptr ? entry.curve : defaultCurve);
                    startOrUpdateAnimation(
                            elementTag,
                            ElementAnimationKeyColorGreen,
                            previousColorIt->second.g,
                            targetColorIt->second.g,
                            entry.duration,
                            entry.curve != nullptr ? entry.curve : defaultCurve);
                    startOrUpdateAnimation(
                            elementTag,
                            ElementAnimationKeyColorBlue,
                            previousColorIt->second.b,
                            targetColorIt->second.b,
                            entry.duration,
                            entry.curve != nullptr ? entry.curve : defaultCurve);
                    startOrUpdateAnimation(
                            elementTag,
                            ElementAnimationKeyColorAlpha,
                            previousColorIt->second.a,
                            targetColorIt->second.a,
                            entry.duration,
                            entry.curve != nullptr ? entry.curve : defaultCurve);
                }
                continue;
            }

            if(entry.kind == StyleSheet::Entry::Kind::ElementAnimation){
                if(entry.elementTag.empty()){
                    continue;
                }
                switch(entry.animationKey){
                    case ElementAnimationKeyColorRed:
                    case ElementAnimationKeyColorGreen:
                    case ElementAnimationKeyColorBlue:
                    case ElementAnimationKeyColorAlpha: {
                        auto colorIt = targetColors.find(entry.elementTag);
                        if(colorIt == targetColors.end()){
                            break;
                        }
                        float to = colorIt->second.a;
                        if(entry.animationKey == ElementAnimationKeyColorRed){
                            to = colorIt->second.r;
                        }
                        else if(entry.animationKey == ElementAnimationKeyColorGreen){
                            to = colorIt->second.g;
                        }
                        else if(entry.animationKey == ElementAnimationKeyColorBlue){
                            to = colorIt->second.b;
                        }
                        applyColorAnimation(entry.elementTag,
                                            entry.animationKey,
                                            to,
                                            entry.duration,
                                            entry.curve);
                        break;
                    }
                    case ElementAnimationKeyWidth:
                    case ElementAnimationKeyHeight:
                        applyDimensionAnimation(entry.elementTag,
                                               entry.animationKey,
                                               entry.duration,
                                               entry.curve);
                        break;
                    default:
                        break;
                }
                continue;
            }

            if(entry.kind == StyleSheet::Entry::Kind::ElementPathAnimation){
                if(entry.elementTag.empty()){
                    continue;
                }
                applyPathNodeAnimation(entry.elementTag,entry.nodeIndex,entry.duration,entry.curve);
                continue;
            }

            if(entry.kind == StyleSheet::Entry::Kind::ElementBrushAnimation){
                auto animatedColor = colorFromBrush(entry.brush);
                if(!animatedColor){
                    continue;
                }
                for(const auto & colorEntry : targetColors){
                    if(!entry.elementTag.empty() && !matchesTag(entry.elementTag,colorEntry.first)){
                        continue;
                    }
                    float targetComponent = animatedColor->a;
                    switch(entry.animationKey){
                        case ElementAnimationKeyColorRed:
                            targetComponent = animatedColor->r;
                            break;
                        case ElementAnimationKeyColorGreen:
                            targetComponent = animatedColor->g;
                            break;
                        case ElementAnimationKeyColorBlue:
                            targetComponent = animatedColor->b;
                            break;
                        case ElementAnimationKeyColorAlpha:
                            targetComponent = animatedColor->a;
                            break;
                        default:
                            continue;
                    }
                    applyColorAnimation(colorEntry.first,
                                        entry.animationKey,
                                        targetComponent,
                                        entry.duration,
                                        entry.curve);
                }
            }
        }
    }

    for(const auto & shapeEntry : currentShapes){
        previousShapeByTag[shapeEntry.first] = shapeEntry.second;
    }
    for(const auto & colorEntry : targetColors){
        lastResolvedElementColor[colorEntry.first] = colorEntry.second;
    }

    OmegaCommon::Vector<UIElementTag> removeShapeTags {};
    for(const auto & previousShapeEntry : previousShapeByTag){
        if(currentShapes.find(previousShapeEntry.first) == currentShapes.end()){
            removeShapeTags.push_back(previousShapeEntry.first);
        }
    }
    for(const auto & tagToRemove : removeShapeTags){
        previousShapeByTag.erase(tagToRemove);
    }

    OmegaCommon::Vector<UIElementTag> removeColorTags {};
    for(const auto & colorEntry : lastResolvedElementColor){
        if(targetColors.find(colorEntry.first) == targetColors.end()){
            removeColorTags.push_back(colorEntry.first);
        }
    }
    for(const auto & tagToRemove : removeColorTags){
        lastResolvedElementColor.erase(tagToRemove);
    }
}

void UIView::prepareEffectAnimations(const OmegaCommon::Vector<UIViewLayout::Element> &elements,
                                     bool layoutChanged,
                                     bool styleChanged){
    OmegaCommon::Map<UIElementTag,ResolvedEffectStyle> resolvedByTag {};
    resolvedByTag[kUIViewRootEffectTag] = resolveRootEffectStyle(currentStyle,tag);
    for(const auto & element : elements){
        resolvedByTag[element.tag] = resolveElementEffectStyle(currentStyle,tag,element.tag);
    }

    if(layoutChanged || styleChanged){
        const auto defaultCurve = Composition::AnimationCurve::Linear();
        for(const auto & effectEntry : resolvedByTag){
            const auto & targetTag = effectEntry.first;
            const auto & resolved = effectEntry.second;
            auto prevIt = lastResolvedEffects.find(targetTag);
            EffectState previous {};
            if(prevIt != lastResolvedEffects.end()){
                previous = prevIt->second;
            }
            auto next = toEffectState(resolved);

            auto markTargetDirty = [&](){
                if(targetTag == kUIViewRootEffectTag){
                    rootStyleDirty = true;
                    rootContentDirty = true;
                }
                else {
                    markElementDirty(targetTag,false,true,true,false,false);
                }
            };

            if(!effectStateClose(previous,next)){
                markTargetDirty();
            }

            auto startTransition = [&](int animationKey,
                                       float from,
                                       float to,
                                       const ResolvedEffectTransition & transitionSpec){
                startOrUpdateAnimation(
                        targetTag,
                        animationKey,
                        from,
                        to,
                        transitionSpec.transition ? transitionSpec.duration : 0.f,
                        transitionSpec.curve != nullptr ? transitionSpec.curve : defaultCurve);
            };

            auto fromShadow = previous.dropShadow.value_or(makeDefaultShadowParams());
            auto toShadow = resolved.dropShadow.value_or(makeClearShadowParams(fromShadow));
            const bool useShadowTransition = resolved.dropShadowTransition.transition &&
                                             resolved.dropShadowTransition.duration > 0.f;
            startTransition(EffectAnimationKeyShadowOffsetX,
                            fromShadow.x_offset,
                            toShadow.x_offset,
                            useShadowTransition ? resolved.dropShadowTransition : ResolvedEffectTransition{});
            startTransition(EffectAnimationKeyShadowOffsetY,
                            fromShadow.y_offset,
                            toShadow.y_offset,
                            useShadowTransition ? resolved.dropShadowTransition : ResolvedEffectTransition{});
            startTransition(EffectAnimationKeyShadowRadius,
                            fromShadow.radius,
                            toShadow.radius,
                            useShadowTransition ? resolved.dropShadowTransition : ResolvedEffectTransition{});
            startTransition(EffectAnimationKeyShadowBlur,
                            fromShadow.blurAmount,
                            toShadow.blurAmount,
                            useShadowTransition ? resolved.dropShadowTransition : ResolvedEffectTransition{});
            startTransition(EffectAnimationKeyShadowOpacity,
                            fromShadow.opacity,
                            toShadow.opacity,
                            useShadowTransition ? resolved.dropShadowTransition : ResolvedEffectTransition{});
            startTransition(EffectAnimationKeyShadowColorR,
                            fromShadow.color.r,
                            toShadow.color.r,
                            useShadowTransition ? resolved.dropShadowTransition : ResolvedEffectTransition{});
            startTransition(EffectAnimationKeyShadowColorG,
                            fromShadow.color.g,
                            toShadow.color.g,
                            useShadowTransition ? resolved.dropShadowTransition : ResolvedEffectTransition{});
            startTransition(EffectAnimationKeyShadowColorB,
                            fromShadow.color.b,
                            toShadow.color.b,
                            useShadowTransition ? resolved.dropShadowTransition : ResolvedEffectTransition{});
            startTransition(EffectAnimationKeyShadowColorA,
                            fromShadow.color.a,
                            toShadow.color.a,
                            useShadowTransition ? resolved.dropShadowTransition : ResolvedEffectTransition{});

            const float fromGaussian = previous.gaussianBlur ? previous.gaussianBlur->radius : 0.f;
            const float toGaussian = resolved.gaussianBlur ? resolved.gaussianBlur->radius : 0.f;
            const bool useGaussianTransition = resolved.gaussianBlurTransition.transition &&
                                               resolved.gaussianBlurTransition.duration > 0.f;
            startTransition(EffectAnimationKeyGaussianRadius,
                            fromGaussian,
                            toGaussian,
                            useGaussianTransition ? resolved.gaussianBlurTransition : ResolvedEffectTransition{});

            const auto fromDirectional = previous.directionalBlur.value_or(
                    Composition::CanvasEffect::DirectionalBlurParams{0.f,0.f});
            const auto toDirectional = resolved.directionalBlur.value_or(
                    Composition::CanvasEffect::DirectionalBlurParams{0.f,fromDirectional.angle});
            const bool useDirectionalTransition = resolved.directionalBlurTransition.transition &&
                                                  resolved.directionalBlurTransition.duration > 0.f;
            startTransition(EffectAnimationKeyDirectionalRadius,
                            fromDirectional.radius,
                            toDirectional.radius,
                            useDirectionalTransition ? resolved.directionalBlurTransition
                                                     : ResolvedEffectTransition{});
            startTransition(EffectAnimationKeyDirectionalAngle,
                            fromDirectional.angle,
                            toDirectional.angle,
                            useDirectionalTransition ? resolved.directionalBlurTransition
                                                     : ResolvedEffectTransition{});

            if(hasAnyEffect(next)){
                lastResolvedEffects[targetTag] = next;
            }
            else {
                lastResolvedEffects.erase(targetTag);
            }
        }
    }
    else {
        for(const auto & effectEntry : resolvedByTag){
            auto next = toEffectState(effectEntry.second);
            if(hasAnyEffect(next)){
                lastResolvedEffects[effectEntry.first] = next;
            }
            else {
                lastResolvedEffects.erase(effectEntry.first);
            }
        }
    }

    OmegaCommon::Vector<UIElementTag> staleTags {};
    for(const auto & last : lastResolvedEffects){
        if(resolvedByTag.find(last.first) == resolvedByTag.end()){
            staleTags.push_back(last.first);
        }
    }
    for(const auto & staleTag : staleTags){
        lastResolvedEffects.erase(staleTag);
    }
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
        rootCanvas = makeCanvas(getLayerTreeLimb()->getRootLayer());
        markRootDirty();
        firstFrameCoherentSubmit = true;
    }

    OmegaCommon::Vector<UIElementTag> previousOrder = activeTagOrder;
    OmegaCommon::Vector<UIElementTag> nextOrder {};
    nextOrder.reserve(currentLayout.elements().size());
    for(const auto & element : currentLayout.elements()){
        nextOrder.push_back(element.tag);
    }

    const bool orderChanged = previousOrder.size() != nextOrder.size() ||
                              !std::equal(previousOrder.begin(),
                                          previousOrder.end(),
                                          nextOrder.begin(),
                                          nextOrder.end());

    if(orderChanged){
        rootOrderDirty = true;
    }

    const bool layoutChanged = layoutDirty;
    const bool styleChanged = styleDirty;
    const bool styleChangedGlobal = styleDirtyGlobal;
    const bool forceCoherentFrame = firstFrameCoherentSubmit ||
                                    layoutChanged ||
                                    styleChangeRequiresCoherentFrame;

    if(layoutChanged){
        rootLayoutDirty = true;
        rootContentDirty = true;
    }
    if(styleChanged && styleChangedGlobal){
        rootStyleDirty = true;
    }

    syncElementDirtyState(currentLayout.elements(),layoutChanged,styleChangedGlobal,orderChanged);

    const auto previousEffectsSnapshot = lastResolvedEffects;

    bool styleUsesAnimation = false;
    if(currentStyle != nullptr){
        for(const auto & entry : currentStyle->entries){
            if(!entry.viewTag.empty() && !matchesTag(entry.viewTag,tag)){
                continue;
            }
            if(entry.transition ||
               entry.kind == StyleSheet::Entry::Kind::ElementAnimation ||
               entry.kind == StyleSheet::Entry::Kind::ElementPathAnimation ||
               entry.kind == StyleSheet::Entry::Kind::ElementBrushAnimation){
                styleUsesAnimation = true;
                break;
            }
        }
    }
    prepareEffectAnimations(currentLayout.elements(),layoutChanged,styleChanged);

    const bool hasActiveRuntimeAnimations = !elementAnimations.empty() ||
                                            !pathNodeAnimations.empty();
    if(styleUsesAnimation || hasActiveRuntimeAnimations){
        prepareElementAnimations(currentLayout.elements(),layoutChanged,styleChanged);
        (void)advanceAnimations();
    }

    const bool submitRoot = forceCoherentFrame ||
                            rootLayoutDirty ||
                            rootStyleDirty ||
                            rootContentDirty ||
                            rootOrderDirty;

    OmegaCommon::Vector<UIElementTag> dirtyActiveTags {};
    dirtyActiveTags.reserve(activeTagOrder.size());
    for(const auto & tagToCheck : activeTagOrder){
        if(forceCoherentFrame || isElementDirty(tagToCheck)){
            dirtyActiveTags.push_back(tagToCheck);
        }
    }
    lastUpdateDiagnostics.activeTagCount = activeTagOrder.size();
    lastUpdateDiagnostics.dirtyTagCount = dirtyActiveTags.size();
    lastUpdateDiagnostics.submittedTagCount = 0;

    bool hasInactiveVisibilityChanges = false;
    for(const auto & entry : elementDirtyState){
        if(containsTag(activeTagOrder,entry.first)){
            continue;
        }
        if(entry.second.visibilityDirty){
            hasInactiveVisibilityChanges = true;
            break;
        }
    }

    const bool hasDirtyWork = submitRoot ||
                              !dirtyActiveTags.empty() ||
                              hasInactiveVisibilityChanges;
    if(!hasDirtyWork){
        layoutDirty = false;
        styleDirty = false;
        styleDirtyGlobal = false;
        styleChangeRequiresCoherentFrame = false;
        firstFrameCoherentSubmit = false;
        ++lastUpdateDiagnostics.revision;
        return;
    }

    startCompositionSession();

    auto effectStateForTag = [&](const UIElementTag & targetTag) -> EffectState {
        if(targetTag == kUIViewRootEffectTag){
            return toEffectState(resolveRootEffectStyle(currentStyle,tag));
        }
        return toEffectState(resolveElementEffectStyle(currentStyle,tag,targetTag));
    };

    auto previousEffectStateForTag = [&](const UIElementTag & targetTag) -> EffectState {
        auto prevIt = previousEffectsSnapshot.find(targetTag);
        if(prevIt != previousEffectsSnapshot.end()){
            return prevIt->second;
        }
        return {};
    };

    auto applyEffects = [&](const UIElementTag & targetTag,
                            const EffectState & resolved,
                            const EffectState & previous,
                            const SharedHandle<Composition::Canvas> & targetCanvas){
        if(targetCanvas == nullptr){
            return;
        }

        const bool hasShadowAnimation = animatedValue(targetTag,EffectAnimationKeyShadowOffsetX).has_value() ||
                                        animatedValue(targetTag,EffectAnimationKeyShadowOffsetY).has_value() ||
                                        animatedValue(targetTag,EffectAnimationKeyShadowRadius).has_value() ||
                                        animatedValue(targetTag,EffectAnimationKeyShadowBlur).has_value() ||
                                        animatedValue(targetTag,EffectAnimationKeyShadowOpacity).has_value() ||
                                        animatedValue(targetTag,EffectAnimationKeyShadowColorR).has_value() ||
                                        animatedValue(targetTag,EffectAnimationKeyShadowColorG).has_value() ||
                                        animatedValue(targetTag,EffectAnimationKeyShadowColorB).has_value() ||
                                        animatedValue(targetTag,EffectAnimationKeyShadowColorA).has_value();

        auto shadowParams = resolved.dropShadow.value_or(
                previous.dropShadow.value_or(makeDefaultShadowParams()));
        if(auto v = animatedValue(targetTag,EffectAnimationKeyShadowOffsetX); v){
            shadowParams.x_offset = *v;
        }
        if(auto v = animatedValue(targetTag,EffectAnimationKeyShadowOffsetY); v){
            shadowParams.y_offset = *v;
        }
        if(auto v = animatedValue(targetTag,EffectAnimationKeyShadowRadius); v){
            shadowParams.radius = std::max(0.f,*v);
        }
        if(auto v = animatedValue(targetTag,EffectAnimationKeyShadowBlur); v){
            shadowParams.blurAmount = std::max(0.f,*v);
        }
        if(auto v = animatedValue(targetTag,EffectAnimationKeyShadowOpacity); v){
            shadowParams.opacity = clamp01(*v);
        }
        if(auto v = animatedValue(targetTag,EffectAnimationKeyShadowColorR); v){
            shadowParams.color.r = clamp01(*v);
        }
        if(auto v = animatedValue(targetTag,EffectAnimationKeyShadowColorG); v){
            shadowParams.color.g = clamp01(*v);
        }
        if(auto v = animatedValue(targetTag,EffectAnimationKeyShadowColorB); v){
            shadowParams.color.b = clamp01(*v);
        }
        if(auto v = animatedValue(targetTag,EffectAnimationKeyShadowColorA); v){
            shadowParams.color.a = clamp01(*v);
        }

        if(resolved.dropShadow || previous.dropShadow || hasShadowAnimation){
            auto layerEffect = std::make_shared<Composition::LayerEffect>(
                    Composition::LayerEffect{Composition::LayerEffect::DropShadow});
            layerEffect->dropShadow = shadowParams;
            targetCanvas->applyLayerEffect(layerEffect);
        }

        float gaussianRadius = resolved.gaussianBlur ? resolved.gaussianBlur->radius : 0.f;
        if(auto v = animatedValue(targetTag,EffectAnimationKeyGaussianRadius); v){
            gaussianRadius = std::max(0.f,*v);
        }
        if(gaussianRadius > 0.f){
            auto blurEffect = std::make_shared<Composition::CanvasEffect>();
            blurEffect->type = Composition::CanvasEffect::GaussianBlur;
            blurEffect->gaussianBlur.radius = gaussianRadius;
            targetCanvas->applyEffect(blurEffect);
        }

        Composition::CanvasEffect::DirectionalBlurParams directionalParams {};
        bool hasDirectional = false;
        if(resolved.directionalBlur){
            directionalParams = *resolved.directionalBlur;
            hasDirectional = true;
        }
        else if(previous.directionalBlur){
            directionalParams = *previous.directionalBlur;
        }
        if(auto v = animatedValue(targetTag,EffectAnimationKeyDirectionalRadius); v){
            directionalParams.radius = std::max(0.f,*v);
            hasDirectional = true;
        }
        if(auto v = animatedValue(targetTag,EffectAnimationKeyDirectionalAngle); v){
            directionalParams.angle = *v;
            hasDirectional = true;
        }
        if(hasDirectional && directionalParams.radius > 0.f){
            auto directionalEffect = std::make_shared<Composition::CanvasEffect>();
            directionalEffect->type = Composition::CanvasEffect::DirectionalBlur;
            directionalEffect->directionalBlur = directionalParams;
            targetCanvas->applyEffect(directionalEffect);
        }
    };

    if(submitRoot){
        auto viewStyle = resolveViewStyle(currentStyle,tag);
        auto & rootBackground = rootCanvas->getCurrentFrame()->background;
        auto backgroundColor = viewStyle.backgroundColor.value_or(Composition::Color::Transparent);
        rootBackground.r = backgroundColor.r;
        rootBackground.g = backgroundColor.g;
        rootBackground.b = backgroundColor.b;
        rootBackground.a = backgroundColor.a;

        if(viewStyle.useBorder && viewStyle.borderColor && viewStyle.borderWidth > 0.f){
            auto borderBrush = Composition::ColorBrush(*viewStyle.borderColor);
            auto outer = localBoundsFromView(this);
            rootCanvas->drawRect(outer,borderBrush);

            float borderWidth = std::max(0.f,viewStyle.borderWidth);
            Core::Rect inner {
                    {outer.pos.x + borderWidth,outer.pos.y + borderWidth},
                    std::max(0.f,outer.w - (borderWidth * 2.f)),
                    std::max(0.f,outer.h - (borderWidth * 2.f))
            };
            if(inner.w > 0.f && inner.h > 0.f){
                auto fillBrush = Composition::ColorBrush(backgroundColor);
                rootCanvas->drawRect(inner,fillBrush);
            }
        }
        applyEffects(kUIViewRootEffectTag,
                     effectStateForTag(kUIViewRootEffectTag),
                     previousEffectStateForTag(kUIViewRootEffectTag),
                     rootCanvas);
        rootCanvas->sendFrame();
    }

    for(const auto & dirtyTag : dirtyActiveTags){
        auto layoutIt = std::find_if(currentLayout.elements().begin(),
                                     currentLayout.elements().end(),
                                     [&](const UIViewLayout::Element & element){
                                         return element.tag == dirtyTag;
                                     });
        if(layoutIt == currentLayout.elements().end()){
            continue;
        }

        auto & target = buildLayerRenderTarget(dirtyTag);
        if(target.layer != nullptr){
            target.layer->setEnabled(true);
        }
        if(target.canvas == nullptr){
            clearElementDirty(dirtyTag);
            continue;
        }

        auto & elementBg = target.canvas->getCurrentFrame()->background;
        elementBg.r = 0.f;
        elementBg.g = 0.f;
        elementBg.b = 0.f;
        elementBg.a = 0.f;

        bool emittedVisual = false;
        const auto localBounds = localBoundsFromView(this);
        ChildResizeSpec layoutClamp {};
        layoutClamp.resizable = true;
        layoutClamp.policy = ChildResizePolicy::FitContent;
        if(layoutIt->type == UIViewLayout::Element::Type::Shape && layoutIt->shape){
            auto shapeToDraw = styleUsesAnimation ?
                               applyAnimatedShape(dirtyTag,*layoutIt->shape) :
                               *layoutIt->shape;
            auto brush = resolveElementBrush(currentStyle,tag,dirtyTag);
            auto animatedBrushColor = styleUsesAnimation ? colorFromBrush(brush) : Core::Optional<Composition::Color>{};
            if(animatedBrushColor && styleUsesAnimation){
                auto animatedColor = applyAnimatedColor(dirtyTag,*animatedBrushColor);
                brush = Composition::ColorBrush(animatedColor);
            }

            switch(shapeToDraw.type){
                case Shape::Type::Rect: {
                    auto rect = shapeToDraw.rect;
                    rect = ViewResizeCoordinator::clampRectToParent(rect,localBounds,layoutClamp);
                    target.canvas->drawRect(rect,brush);
                    emittedVisual = true;
                    break;
                }
                case Shape::Type::RoundedRect: {
                    auto rect = shapeToDraw.roundedRect;
                    Core::Rect clampedRect {
                        rect.pos,
                        rect.w,
                        rect.h
                    };
                    clampedRect = ViewResizeCoordinator::clampRectToParent(clampedRect,localBounds,layoutClamp);
                    rect.pos = clampedRect.pos;
                    rect.w = clampedRect.w;
                    rect.h = clampedRect.h;
                    rect.rad_x = std::min(rect.rad_x,rect.w * 0.5f);
                    rect.rad_y = std::min(rect.rad_y,rect.h * 0.5f);
                    target.canvas->drawRoundedRect(rect,brush);
                    emittedVisual = true;
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
                    target.canvas->drawEllipse(ellipse,brush);
                    emittedVisual = true;
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
                        target.canvas->drawPath(path);
                        emittedVisual = true;
                    }
                    break;
                }
                default:
                    break;
            }
        }
        else if(layoutIt->type == UIViewLayout::Element::Type::Text && layoutIt->str){
            UIElementTag textStyleTag = layoutIt->textStyleTag.value_or(dirtyTag);
            auto textStyle = resolveTextStyle(currentStyle,tag,textStyleTag);
            auto font = textStyle.font != nullptr ? textStyle.font : resolveFallbackTextFont();
            if(font != nullptr){
                auto textRect = layoutIt->textRect.value_or(localBounds);
                textRect = ViewResizeCoordinator::clampRectToParent(textRect,localBounds,layoutClamp);
                auto textColor = styleUsesAnimation ? applyAnimatedColor(dirtyTag,textStyle.color) : textStyle.color;
                auto unicodeText = UniString::fromUTF32(
                        reinterpret_cast<const UChar32 *>(layoutIt->str->data()),
                        static_cast<int32_t>(layoutIt->str->size()));
                auto textLayout = textStyle.layout;
                textLayout.lineLimit = textStyle.lineLimit;
                target.canvas->drawText(unicodeText,font,textRect,textColor,textLayout);
                emittedVisual = true;
            }
        }

        if(!emittedVisual){
            auto clearBrush = Composition::ColorBrush(Composition::Color::Transparent);
            auto clearRect = localBounds;
            target.canvas->drawRect(clearRect,clearBrush);
        }

        applyEffects(dirtyTag,
                     effectStateForTag(dirtyTag),
                     previousEffectStateForTag(dirtyTag),
                     target.canvas);

        target.canvas->sendFrame();
        ++lastUpdateDiagnostics.submittedTagCount;
        clearElementDirty(dirtyTag);
    }

    for(auto & entry : elementDirtyState){
        if(containsTag(activeTagOrder,entry.first)){
            continue;
        }
        if(!entry.second.visibilityDirty){
            continue;
        }

        auto targetIt = renderTargetStore.find(entry.first);
        if(targetIt != renderTargetStore.end()){
            auto & target = targetIt->second;
            if(target.canvas != nullptr){
                auto & elementBg = target.canvas->getCurrentFrame()->background;
                elementBg.r = 0.f;
                elementBg.g = 0.f;
                elementBg.b = 0.f;
                elementBg.a = 0.f;
                auto clearBrush = Composition::ColorBrush(Composition::Color::Transparent);
                auto clearRect = localBoundsFromView(this);
                target.canvas->drawRect(clearRect,clearBrush);
                target.canvas->sendFrame();
                ++lastUpdateDiagnostics.submittedTagCount;
            }
            if(target.layer != nullptr){
                target.layer->setEnabled(false);
            }
        }

        clearElementDirty(entry.first);
    }

    rootLayoutDirty = false;
    rootStyleDirty = false;
    rootContentDirty = false;
    rootOrderDirty = false;

    endCompositionSession();
    layoutDirty = false;
    styleDirty = false;
    styleDirtyGlobal = false;
    styleChangeRequiresCoherentFrame = false;
    firstFrameCoherentSubmit = false;
    ++lastUpdateDiagnostics.revision;
}

}
