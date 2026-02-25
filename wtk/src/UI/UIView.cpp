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

    for(const auto & entry : style->entries){
        if(!entry.viewTag.empty() && !matchesTag(entry.viewTag,viewTag)){
            continue;
        }
        if(!entry.elementTag.empty() && !matchesTag(entry.elementTag,elementTag)){
            continue;
        }
        switch(entry.kind){
            case StyleSheet::Entry::Kind::TextFont:
                if(entry.font != nullptr){
                    resolved.font = entry.font;
                }
                break;
            case StyleSheet::Entry::Kind::TextColor:
                if(entry.color){
                    resolved.color = *entry.color;
                }
                break;
            case StyleSheet::Entry::Kind::TextAlignment:
                if(entry.textAlignment){
                    resolved.layout.alignment = *entry.textAlignment;
                }
                break;
            case StyleSheet::Entry::Kind::TextWrapping:
                if(entry.textWrapping){
                    resolved.layout.wrapping = *entry.textWrapping;
                }
                break;
            case StyleSheet::Entry::Kind::TextLineLimit:
                if(entry.uintValue){
                    resolved.lineLimit = *entry.uintValue;
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
    currentStyle = style;
    styleDirty = true;
    rootStyleDirty = true;
    markAllElementsDirty();
    firstFrameCoherentSubmit = true;
}

StyleSheetPtr UIView::getStyleSheet() const{
    return currentStyle;
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
                                    ElementAnimationKey key,
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
            tagIt->second.erase(static_cast<int>(key));
            if(tagIt->second.empty()){
                elementAnimations.erase(tagIt);
            }
        }
        return;
    }

    auto & propertyMap = elementAnimations[tag];
    auto & state = propertyMap[static_cast<int>(key)];
    if(state.active && std::fabs(state.to - to) <= 0.0001f){
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float startValue = state.active ? state.value : from;
    state.active = true;
    state.from = startValue;
    state.to = to;
    state.value = startValue;
    state.durationSec = std::max(0.001f,durationSec);
    state.startTime = now;
    state.curve = curve != nullptr ? curve : Composition::AnimationCurve::Linear();
    markElementDirty(tag,false,false,true,false,false);
}

bool UIView::advanceAnimations(){
    auto now = std::chrono::steady_clock::now();
    bool changed = false;
    OmegaCommon::Vector<UIElementTag> removePropertyTags {};

    for(auto & tagEntry : elementAnimations){
        OmegaCommon::Vector<int> removeKeys {};
        for(auto & propertyEntry : tagEntry.second){
            auto & state = propertyEntry.second;
            if(!state.active){
                removeKeys.push_back(propertyEntry.first);
                continue;
            }

            float elapsedSec = std::chrono::duration<float>(now - state.startTime).count();
            if(!std::isfinite(elapsedSec) || elapsedSec < 0.f){
                elapsedSec = 0.f;
            }

            float t = state.durationSec <= 0.f ? 1.f : clamp01(elapsedSec / state.durationSec);
            float sampled = state.curve != nullptr ? clamp01(state.curve->sample(t)) : t;
            float nextValue = state.from + ((state.to - state.from) * sampled);
            if(!std::isfinite(nextValue)){
                nextValue = state.to;
            }

            if(std::fabs(nextValue - state.value) > 0.0001f){
                state.value = nextValue;
                markElementDirty(tagEntry.first,false,false,true,false,false);
                changed = true;
            }

            if(t >= 1.f){
                state.value = state.to;
                state.active = false;
                markElementDirty(tagEntry.first,false,false,true,false,false);
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

                float elapsedSec = std::chrono::duration<float>(now - propertyState.startTime).count();
                if(!std::isfinite(elapsedSec) || elapsedSec < 0.f){
                    elapsedSec = 0.f;
                }

                float t = propertyState.durationSec <= 0.f ? 1.f : clamp01(elapsedSec / propertyState.durationSec);
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

    return changed;
}

Core::Optional<float> UIView::animatedValue(const UIElementTag &tag,ElementAnimationKey key) const{
    auto tagIt = elementAnimations.find(tag);
    if(tagIt == elementAnimations.end()){
        return {};
    }
    auto propertyIt = tagIt->second.find(static_cast<int>(key));
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
                        propertyState.active = false;
                        propertyState.value = to;
                        return;
                    }
                    if(durationSec <= 0.f || std::fabs(to - from) <= 0.0001f){
                        propertyState.active = false;
                        propertyState.value = to;
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
                    propertyState.durationSec = std::max(0.001f,durationSec);
                    propertyState.startTime = now;
                    propertyState.curve = curve != nullptr ? curve : defaultCurve;
                    markElementDirty(elementTag,false,false,true,false,false);
                };

                startPathAnimationProperty(existingNodeIt->x,
                                           previousPoints[idx].x,
                                           currentPoints[idx].x);
                startPathAnimationProperty(existingNodeIt->y,
                                           previousPoints[idx].y,
                                           currentPoints[idx].y);
            };

            if(entry.kind == StyleSheet::Entry::Kind::ElementBrush ||
               entry.kind == StyleSheet::Entry::Kind::TextColor){
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
    const bool forceCoherentFrame = firstFrameCoherentSubmit || layoutChanged || styleChanged;

    if(layoutChanged){
        rootLayoutDirty = true;
        rootContentDirty = true;
    }
    if(styleChanged){
        rootStyleDirty = true;
    }

    syncElementDirtyState(currentLayout.elements(),layoutChanged,styleChanged,orderChanged);

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
    if(styleUsesAnimation){
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
        firstFrameCoherentSubmit = false;
        return;
    }

    startCompositionSession();

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
                    target.canvas->drawRect(rect,brush);
                    emittedVisual = true;
                    break;
                }
                case Shape::Type::RoundedRect: {
                    auto rect = shapeToDraw.roundedRect;
                    target.canvas->drawRoundedRect(rect,brush);
                    emittedVisual = true;
                    break;
                }
                case Shape::Type::Ellipse: {
                    const auto & srcEllipse = shapeToDraw.ellipse;
                    Core::Ellipse ellipse {
                            srcEllipse.x,
                            srcEllipse.y,
                            srcEllipse.rad_x,
                            srcEllipse.rad_y
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
                auto textRect = layoutIt->textRect.value_or(localBoundsFromView(this));
                auto textColor = styleUsesAnimation ? applyAnimatedColor(dirtyTag,textStyle.color) : textStyle.color;
                auto unicodeText = UniString::fromUTF32(
                        reinterpret_cast<const UChar32 *>(layoutIt->str->data()),
                        static_cast<int32_t>(layoutIt->str->size()));
                target.canvas->drawText(unicodeText,font,textRect,textColor,textStyle.layout);
                emittedVisual = true;
            }
        }

        if(!emittedVisual){
            auto clearBrush = Composition::ColorBrush(Composition::Color::Transparent);
            auto clearRect = localBoundsFromView(this);
            target.canvas->drawRect(clearRect,clearBrush);
        }

        target.canvas->sendFrame();
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
    firstFrameCoherentSubmit = false;
}

}
