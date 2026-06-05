#include "UIViewImpl.h"
#include <omegaGTE/GTEBase.h>

namespace OmegaWTK {

UIView::Impl::Impl(UIView & ownerRef,UIViewTag tagValue)
    : owner(ownerRef),
      tag(std::move(tagValue)) {
}

Shape Shape::Scalar(int width,int height){
    Shape shape {};
    shape.type = Shape::Type::Rect;
    shape.rect = Composition::Rect{{0.f,0.f},static_cast<float>(width),static_cast<float>(height)};
    return shape;
}

Shape Shape::Rect(const Composition::Rect &rect){
    Shape shape {};
    shape.type = Shape::Type::Rect;
    shape.rect = rect;
    return shape;
}

Shape Shape::RoundedRect(const Composition::RoundedRect &rect){
    Shape shape {};
    shape.type = Shape::Type::RoundedRect;
    shape.roundedRect = rect;
    return shape;
}

Shape Shape::Ellipse(const OmegaGTE::GEllipsoid &ellipse){
    Shape shape {};
    shape.type = Shape::Type::Ellipse;
    shape.ellipse = Composition::Ellipse{ellipse.x, ellipse.y, ellipse.rad_x, ellipse.rad_y};
    return shape;
}

Shape Shape::Ellipse(const Composition::Ellipse &ellipse){
    Shape shape {};
    shape.type = Shape::Type::Ellipse;
    shape.ellipse = ellipse;
    return shape;
}

Shape Shape::Path(Composition::Path path,unsigned strokeWidth,bool closePath){
    Shape shape {};
    shape.type = Shape::Type::Path;
    shape.path = std::make_shared<Composition::Path>(std::move(path));
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

void UIViewLayout::text(UIElementTag tag,OmegaCommon::UString content,const Composition::Rect & rect){
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

void UIViewLayout::text(UIElementTag tag,OmegaCommon::UString content,const Composition::Rect & rect,UIElementTag styleTag){
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

void UIViewLayout::image(UIElementTag tag,const SharedHandle<OmegaCommon::Img::BitmapImage> & img,const Composition::Rect & rect){
    auto taggedElementIt = std::find_if(_content.begin(),_content.end(),[&](const Element & element){
        return element.tag == tag;
    });

    if(taggedElementIt != _content.end()){
        if(taggedElementIt->type != Element::Type::Image){
            return;
        }
        taggedElementIt->image = img;
        taggedElementIt->imageRect = rect;
        taggedElementIt->shape = {};
        taggedElementIt->str = {};
        taggedElementIt->textRect = {};
        taggedElementIt->textStyleTag = {};
        return;
    }

    Element element {};
    element.type = Element::Type::Image;
    element.tag = tag;
    element.image = img;
    element.imageRect = rect;
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

StylePtr Style::Create(){
    return make<Style>();
}

StylePtr Style::copy(){
    return StylePtr(new Style(*this));
}

Style::Style() = default;

StylePtr Style::backgroundColor(UIViewTag tag,
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

StylePtr Style::border(UIViewTag tag,bool use){
    Entry entry {};
    entry.kind = Entry::Kind::BorderEnabled;
    entry.viewTag = tag;
    entry.boolValue = use;
    entries.push_back(entry);
    return copy();
}

StylePtr Style::borderColor(UIViewTag tag,
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

StylePtr Style::borderWidth(UIViewTag tag,
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

StylePtr Style::dropShadow(UIViewTag tag,
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

StylePtr Style::gaussianBlur(UIViewTag tag,
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

StylePtr Style::directionalBlur(UIViewTag tag,
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

StylePtr Style::elementDropShadow(UIElementTag elementTag,
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

StylePtr Style::elementGaussianBlur(UIElementTag elementTag,
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

StylePtr Style::elementDirectionalBlur(UIElementTag elementTag,
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

StylePtr Style::elementBrush(UIElementTag elementTag,
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

StylePtr Style::elementBrushAnimation(SharedHandle<Composition::Brush> brush,
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

StylePtr Style::elementAnimation(UIElementTag elementTag,
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

StylePtr Style::elementPathAnimation(UIElementTag elementTag,
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

StylePtr Style::textFont(UIElementTag elementTag,
                                   SharedHandle<Composition::Font> font){
    Entry entry {};
    entry.kind = Entry::Kind::TextFont;
    entry.elementTag = elementTag;
    entry.font = font;
    entries.push_back(entry);
    return copy();
}

StylePtr Style::textColor(UIElementTag elementTag,
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

StylePtr Style::textAlignment(UIElementTag elementTag,
                                        Composition::TextLayoutDescriptor::Alignment alignment){
    Entry entry {};
    entry.kind = Entry::Kind::TextAlignment;
    entry.elementTag = elementTag;
    entry.textAlignment = alignment;
    entries.push_back(entry);
    return copy();
}

StylePtr Style::textWrapping(UIElementTag elementTag,
                                       Composition::TextLayoutDescriptor::Wrapping wrapping){
    Entry entry {};
    entry.kind = Entry::Kind::TextWrapping;
    entry.elementTag = elementTag;
    entry.textWrapping = wrapping;
    entries.push_back(entry);
    return copy();
}

StylePtr Style::textLineLimit(UIElementTag elementTag,unsigned lineLimit){
    Entry entry {};
    entry.kind = Entry::Kind::TextLineLimit;
    entry.elementTag = elementTag;
    entry.uintValue = lineLimit;
    entries.push_back(entry);
    return copy();
}

// Layout authoring methods (layoutWidth/Height/Size/Margin/Padding/
// Clamp/Transition) were removed from Style in Tier B / B1. Layout is
// authored directly on `UIElementLayoutSpec::layout`.

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

UIView::UIView(const Composition::Rect &rect,ViewPtr parent,UIViewTag tag)
    : View(rect,parent),
      impl_(std::make_unique<Impl>(*this,std::move(tag))){
    enable();
}

UIView::~UIView() = default;

UIViewLayout & UIView::layout(){
    impl_->layoutDirty = true;
    impl_->firstFrameCoherentSubmit = true;
    return impl_->currentLayout;
}

void UIView::setLayout(const UIViewLayout &layout){
    impl_->currentLayout = layout;
    impl_->convertLegacyLayoutToV2();
    // Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
    // model-mutators dirty the cascade themselves. Pre-D8 callers
    // had to remember an explicit `update()` after `setLayout` to
    // make the FrameBuilder walkers visit this view; coupling the
    // dirty flip to the mutator is the right model and removes the
    // rediscover-every-time bug class. Style is included because a
    // layout change reshuffles element tags (which the cascade
    // matches on) and Paint follows naturally once the cells
    // re-resolve.
    markDirty(View::Style | View::Layout | View::Paint);
}

StylePtr UIView::getStyle() const{
    return impl_->currentStyle;
}

const UIView::UpdateDiagnostics & UIView::getLastUpdateDiagnostics() const{
    return impl_->lastUpdateDiagnostics;
}

// Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
// `UIView::getLastAnimationDiagnostics()` deleted alongside the
// `AnimationDiagnostics` struct + `Impl::lastAnimationDiagnostics`
// field. See `UIView.h` for the deletion note;
// `AnimationScheduler::stats()` is the live diagnostics surface.

UIViewLayoutV2 & UIView::layoutV2(){
    impl_->layoutDirty = true;
    impl_->firstFrameCoherentSubmit = true;
    return impl_->currentLayoutV2_;
}

void UIView::setLayoutV2(const UIViewLayoutV2 & layout){
    impl_->currentLayoutV2_ = layout;
    impl_->layoutDirty = true;
    impl_->markAllElementsDirty();
    impl_->firstFrameCoherentSubmit = true;
    // Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
    // mirror `setLayout`'s self-dirtying — see that comment for the
    // rationale. The element tags this layout publishes drive
    // cascade matching, so Style is the lead bit.
    markDirty(View::Style | View::Layout | View::Paint);
}

void UIView::setDiagnosticSink(LayoutDiagnosticSink * sink){
    impl_->diagnosticSink_ = sink;
}

}
