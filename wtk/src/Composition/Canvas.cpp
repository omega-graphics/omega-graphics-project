#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Layer.h"

#include <algorithm>
#include <cassert>

namespace OmegaWTK::Composition {

VisualCommand::Data::Data(const Core::Rect & rect,Core::SharedPtr<Brush> brush,Core::Optional<Border> border) :
rectParams({rect,brush,border})
{

}

VisualCommand::Data::Data(const Core::RoundedRect & rect,Core::SharedPtr<Brush> brush,Core::Optional<Border> border) :
roundedRectParams({rect,brush,border}){

};

VisualCommand::Data::Data(const Core::Ellipse & ellipse,Core::SharedPtr<Brush> brush,Core::Optional<Border> border) :
ellipseParams({ellipse,brush,border}){

};

VisualCommand::Data::Data(const Core::SharedPtr<OmegaGTE::GVectorPath2D> &path,
                          Core::SharedPtr<Brush> brush,
                          Core::SharedPtr<Brush> fillBrush,
                          float strokeWidth,
                          bool contour,
                          bool fill):
pathParams({path,brush,fillBrush,strokeWidth,contour,fill}){

}

VisualCommand::Data::Data(Core::SharedPtr<Media::BitmapImage> img,const Core::Rect &rect) :
bitmapParams({img,nullptr,nullptr,rect}){

};

VisualCommand::Data::Data(Core::SharedPtr<OmegaGTE::GETexture> texture,Core::SharedPtr<OmegaGTE::GEFence> textureFence,const Core::Rect &rect) :
bitmapParams({nullptr,texture,textureFence,rect}){

};

VisualCommand::Data::Data(const LayerEffect::DropShadowParams & shadow,const Core::Rect & shapeRect,float cornerRadius,bool isEllipse) :
shadowParams({shadow,shapeRect,cornerRadius,isEllipse}){

};

VisualCommand::Data::Data(const OmegaGTE::FMatrix<4,4> & matrix) :
transformMatrix(matrix){

};

VisualCommand::Data::Data(float opacityVal) :
opacityValue(opacityVal){

};



void VisualCommand::Data::_destroy(Type t){
    (void)t;
}

VisualCommand::~VisualCommand(){
    // Data is a regular struct (not a tagged union), so RAII handles destruction.
}

Canvas::Canvas(CompositorClientProxy &proxy,Layer &layer): CompositorClient(proxy),rect(layer.getLayerRect()),layer(layer),current(new CanvasFrame {&layer,rect}){
    assert(layer.boundCanvas_ == nullptr &&
           "Layer already has a Canvas bound -- one Canvas per Layer");
    layer.boundCanvas_ = this;
};

Canvas::~Canvas(){
    layer.boundCanvas_ = nullptr;
}

Layer & Canvas::getCorrespondingLayer(){
    return layer;
}

// Layer * Canvas::getParentLayer(){
//     return parentLayer;
// };

void Canvas::drawRect(Core::Rect &rect, Core::SharedPtr<Brush> &brush, Core::Optional<Border> border){
    current->currentVisuals.emplace_back(rect,brush,Core::Optional<Border>{});
    if(border.has_value()){
        auto frame = RectFrame(rect, border->width);
        auto borderBrush = border->brush;
        frame->setPathBrush(borderBrush);
        drawPath(*frame);
    }
};

void Canvas::drawRoundedRect(Core::RoundedRect &rect, Core::SharedPtr<Brush> &brush, Core::Optional<Border> border){
    current->currentVisuals.emplace_back(rect,brush,Core::Optional<Border>{});
    if(border.has_value()){
        auto frame = RoundedRectFrame(rect, border->width);
        auto borderBrush = border->brush;
        frame->setPathBrush(borderBrush);
        drawPath(*frame);
    }
}

void Canvas::drawEllipse(Core::Ellipse &ellipse, Core::SharedPtr<Brush> &brush, Core::Optional<Border> border){
    current->currentVisuals.emplace_back(ellipse,brush,Core::Optional<Border>{});
    if(border.has_value()){
        auto frame = EllipseFrame(ellipse, border->width);
        auto borderBrush = border->brush;
        frame->setPathBrush(borderBrush);
        drawPath(*frame);
    }
}

void Canvas::drawText(const UniString &text,
                      Core::SharedPtr<Font> font,
                      const Core::Rect &rect,
                      const Color &color,
                      const TextLayoutDescriptor &layoutDesc){
    if(font == nullptr || text.length() == 0 || rect.w <= 0.f || rect.h <= 0.f){
        return;
    }

    auto textRect = TextRect::Create(rect,layoutDesc);
    if(textRect == nullptr){
        return;
    }

    auto glyphRun = GlyphRun::fromUStringAndFont(text,font);
    if(glyphRun == nullptr){
        return;
    }

    textRect->drawRun(glyphRun,color);
    auto bitmap = textRect->toBitmap();
    if(bitmap.s == nullptr){
        return;
    }
    drawGETexture(bitmap.s,rect,bitmap.textureFence);
}

void Canvas::drawText(const UniString &text,
                      Core::SharedPtr<Font> font,
                      const Core::Rect &rect,
                      const Color &color){
    drawText(text,
             font,
             rect,
             color,
             TextLayoutDescriptor{TextLayoutDescriptor::LeftUpper,TextLayoutDescriptor::None});
}

void Canvas::drawPath(Path &path){
    auto brush = path.pathBrush;
    if(brush == nullptr){
        brush = ColorBrush(Color::create8Bit(Color::White8));
    }

    const float strokeWidth = static_cast<float>(path.currentStroke);
    const bool isFill = (strokeWidth == 0.f);
    for(auto & segment : path.segments){
        if(segment.path.size() < 2){
            continue;
        }
        auto pathData = std::make_shared<OmegaGTE::GVectorPath2D>(segment.path);
        // When stroke is 0, treat as a fill request: the brush becomes the fill brush.
        Core::SharedPtr<Brush> strokeBrush = isFill ? nullptr : brush;
        Core::SharedPtr<Brush> fillBrush = isFill ? brush : nullptr;
        current->currentVisuals.emplace_back(pathData,strokeBrush,fillBrush,strokeWidth,segment.closed,isFill);
    }
}

void Canvas::drawImage(SharedHandle<Media::BitmapImage> &img,const Core::Rect & rect) {
    current->currentVisuals.emplace_back(img,rect);
}

void Canvas::drawGETexture(SharedHandle<OmegaGTE::GETexture> &img,const Core::Rect & rect,SharedHandle<OmegaGTE::GEFence> fence) {
    current->currentVisuals.emplace_back(img,fence,rect);
}

void Canvas::applyEffect(SharedHandle<CanvasEffect> &effect){
    if(effect == nullptr){
        return;
    }

    CanvasEffect copied = *effect;
    if(copied.params != nullptr){
        switch(copied.type){
            case CanvasEffect::DirectionalBlur: {
                auto *params = (CanvasEffect::DirectionalBlurParams *)copied.params;
                copied.directionalBlur = *params;
                break;
            }
            case CanvasEffect::GaussianBlur:
            default: {
                auto *params = (CanvasEffect::GaussianBlurParams *)copied.params;
                copied.gaussianBlur = *params;
                break;
            }
        }
    }

    if(copied.gaussianBlur.radius < 0.f){
        copied.gaussianBlur.radius = 0.f;
    }
    if(copied.directionalBlur.radius < 0.f){
        copied.directionalBlur.radius = 0.f;
    }
    copied.params = nullptr;
    current->currentEffects.push_back(copied);
}

void Canvas::applyLayerEffect(const SharedHandle<LayerEffect> &effect){
    if(effect == nullptr){
        return;
    }
    auto queuedEffect = effect;
    Timestamp start = std::chrono::high_resolution_clock::now();
    Timestamp deadline = start;
    pushLayerEffectCommand(&layer,queuedEffect,start,deadline);
}

void Canvas::drawShadow(Core::Rect & rect,
                        const LayerEffect::DropShadowParams & shadow){
    Core::Rect shapeRect = rect;
    current->currentVisuals.emplace_back(shadow,shapeRect,0.f,false);
}

void Canvas::drawShadow(Core::RoundedRect & rect,
                        const LayerEffect::DropShadowParams & shadow){
    Core::Rect shapeRect {rect.pos,rect.w,rect.h};
    float cornerRadius = std::max(rect.rad_x,rect.rad_y);
    current->currentVisuals.emplace_back(shadow,shapeRect,cornerRadius,false);
}

void Canvas::drawShadow(Core::Ellipse & ellipse,
                        const LayerEffect::DropShadowParams & shadow){
    Core::Rect shapeRect {
        Core::Position{ellipse.x - ellipse.rad_x,ellipse.y - ellipse.rad_y},
        ellipse.rad_x * 2.f,
        ellipse.rad_y * 2.f
    };
    current->currentVisuals.emplace_back(shadow,shapeRect,0.f,true);
}

void Canvas::setElementTransform(const OmegaGTE::FMatrix<4,4> & matrix){
    current->currentVisuals.emplace_back(matrix);
}

void Canvas::setElementOpacity(float opacity){
    current->currentVisuals.emplace_back(opacity);
}

void Canvas::setBackground(const Color & color){
    current->background = {color.r, color.g, color.b, color.a};
}

void Canvas::clear(Core::Optional<Color> color){
    current->currentVisuals.clear();
    current->currentEffects.clear();
    if(color.has_value()){
        setBackground(*color);
    } else {
        current->background = {0.f, 0.f, 0.f, 0.f};
    }
}

SharedHandle<CanvasFrame> Canvas::getCurrentFrame() {
    return current;
}

SharedHandle<CanvasFrame> Canvas::nextFrame() {
    auto frame = getCurrentFrame();
    // Snapshot the current layer rect into the outgoing frame so it
    // matches the draw commands that were just recorded during onPaint.
    // The frame was created at the end of the *previous* paint cycle,
    // so its rect is stale if the layer resized since then.
    frame->rect = rect;
    current.reset(new CanvasFrame {&layer,rect});
    return frame;
}

void Canvas::sendFrame() {
    auto frame = nextFrame();
    Timestamp ts = std::chrono::high_resolution_clock::now();
    pushFrame(frame,ts);
}

}
