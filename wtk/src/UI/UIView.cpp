#include "omegaWTK/UI/UIView.h"

#include <algorithm>

namespace OmegaWTK {

namespace {

struct ResolvedViewStyle {
    Core::Optional<Composition::Color> backgroundColor {};
    bool useBorder = false;
    Core::Optional<Composition::Color> borderColor {};
    float borderWidth = 0.f;
};

static bool matchesTag(const OmegaCommon::String & selector,const OmegaCommon::String & tag){
    return selector.empty() || selector == tag;
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

static bool containsTag(const OmegaCommon::Vector<UIElementTag> & tags,const UIElementTag & tag){
    return std::find(tags.begin(),tags.end(),tag) != tags.end();
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
        return;
    }

    _content.push_back({Element::Type::Text,tag,content,{}});
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
        return;
    }

    _content.push_back({Element::Type::Shape,tag,{},shape});
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

UIRenderer::UIRenderer(UIView *view):
view(view){

}

UIRenderer::RenderTargetBundle & UIRenderer::buildLayerRenderTarget(UIElementTag tag){
    auto entry = renderTargetStore.find(tag);
    if(entry != renderTargetStore.end()){
        return entry->second;
    }

    auto layerRect = view->getRect();
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
}

UIViewLayout & UIView::layout(){
    layoutDirty = true;
    return currentLayout;
}

void UIView::setLayout(const UIViewLayout &layout){
    currentLayout = layout;
    layoutDirty = true;
}

void UIView::setStyleSheet(const StyleSheetPtr &style){
    currentStyle = style;
    styleDirty = true;
}

StyleSheetPtr UIView::getStyleSheet() const{
    return currentStyle;
}

void UIView::update(){
    if(rootCanvas == nullptr){
        rootCanvas = makeCanvas(getLayerTreeLimb()->getRootLayer());
    }

    startCompositionSession();

    auto viewStyle = resolveViewStyle(currentStyle,tag);
    auto & rootBackground = rootCanvas->getCurrentFrame()->background;
    auto backgroundColor = viewStyle.backgroundColor.value_or(Composition::Color::Transparent);
    rootBackground.r = backgroundColor.r;
    rootBackground.g = backgroundColor.g;
    rootBackground.b = backgroundColor.b;
    rootBackground.a = backgroundColor.a;

    if(viewStyle.useBorder && viewStyle.borderColor && viewStyle.borderWidth > 0.f){
        auto borderBrush = Composition::ColorBrush(*viewStyle.borderColor);
        auto outer = getRect();
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

    OmegaCommon::Vector<UIElementTag> activeTags {};
    activeTags.reserve(currentLayout.elements().size());

    for(const auto & element : currentLayout.elements()){
        activeTags.push_back(element.tag);

        auto & target = buildLayerRenderTarget(element.tag);
        if(target.layer != nullptr){
            target.layer->setEnabled(true);
        }
        if(target.canvas == nullptr){
            continue;
        }

        auto & elementBg = target.canvas->getCurrentFrame()->background;
        elementBg.r = 0.f;
        elementBg.g = 0.f;
        elementBg.b = 0.f;
        elementBg.a = 0.f;

        if(element.type == UIViewLayout::Element::Type::Shape && element.shape){
            auto brush = resolveElementBrush(currentStyle,tag,element.tag);
            switch(element.shape->type){
                case Shape::Type::Rect: {
                    auto rect = element.shape->rect;
                    target.canvas->drawRect(rect,brush);
                    break;
                }
                case Shape::Type::RoundedRect: {
                    auto rect = element.shape->roundedRect;
                    target.canvas->drawRoundedRect(rect,brush);
                    break;
                }
                case Shape::Type::Ellipse: {
                    const auto & srcEllipse = element.shape->ellipse;
                    Core::Ellipse ellipse {
                            srcEllipse.x,
                            srcEllipse.y,
                            srcEllipse.rad_x,
                            srcEllipse.rad_y
                    };
                    target.canvas->drawEllipse(ellipse,brush);
                    break;
                }
                case Shape::Type::Path: {
                    if(element.shape->path){
                        auto vectorPath = *element.shape->path;
                        auto path = Composition::Path(vectorPath,element.shape->pathStrokeWidth);
                        if(element.shape->closePath){
                            path.close();
                        }
                        path.setPathBrush(brush);
                        target.canvas->drawPath(path);
                    }
                    break;
                }
                default:
                    break;
            }
        }

        target.canvas->sendFrame();
    }

    for(auto & targetEntry : renderTargetStore){
        if(!containsTag(activeTags,targetEntry.first) && targetEntry.second.layer != nullptr){
            targetEntry.second.layer->setEnabled(false);
        }
    }

    endCompositionSession();
    layoutDirty = false;
    styleDirty = false;
}

}
