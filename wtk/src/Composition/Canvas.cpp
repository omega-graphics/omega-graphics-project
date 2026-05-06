#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Layer.h"
#include "PathImpl.h"
#include "omegaWTK/UI/View.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>

namespace OmegaWTK::Composition {

VisualCommand::Data::Data(const Composition::Rect & rect,Core::SharedPtr<Brush> brush,Core::Optional<Border> border) :
rectParams({rect,std::move(brush),std::move(border)})
{

}

VisualCommand::Data::Data(const Composition::RoundedRect & rect,Core::SharedPtr<Brush> brush,Core::Optional<Border> border) :
roundedRectParams({rect,brush,border}){

};

VisualCommand::Data::Data(const Composition::Ellipse & ellipse,Core::SharedPtr<Brush> brush,Core::Optional<Border> border) :
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

VisualCommand::Data::Data(Core::SharedPtr<Media::BitmapImage> img,const Composition::Rect &rect) :
bitmapParams({img,nullptr,nullptr,rect}){

};

VisualCommand::Data::Data(Core::SharedPtr<OmegaGTE::GETexture> texture,Core::SharedPtr<OmegaGTE::GEFence> textureFence,const Composition::Rect &rect) :
bitmapParams({nullptr,texture,textureFence,rect}){

};

VisualCommand::Data::Data(const LayerEffect::DropShadowParams & shadow,const Composition::Rect & shapeRect,float cornerRadius,bool isEllipse) :
shadowParams({shadow,shapeRect,cornerRadius,isEllipse}){

};

VisualCommand::Data::Data(const Matrix4x4 & matrix) :
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

Canvas::Canvas(CompositorClientProxy &proxy,Layer &layer,::OmegaWTK::View *owner): CompositorClient(proxy),rect(layer.getLayerRect()),layer(layer),ownerView_(owner),current(new CanvasFrame {&layer,rect}){
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

void Canvas::drawRect(Composition::Rect &rect, Core::SharedPtr<Brush> &brush, Core::Optional<Border> border){
    // Phase 6.5: forward the optional border directly into the visual
    // command. The SDF fragment shader emits fill coverage and stroke
    // coverage from the same distance evaluation, so a bordered rect
    // produces exactly one VisualCommand and one draw call. No
    // RectFrame fall-back is emitted for borders any more — the
    // standalone outline helper remains in `Path.h` for clients that
    // want a stand-alone outline visual.
    current->currentVisuals.emplace_back(rect,brush,border);
};

void Canvas::drawRoundedRect(Composition::RoundedRect &rect, Core::SharedPtr<Brush> &brush, Core::Optional<Border> border){
    // Phase 6.5: forward the optional border directly into the visual
    // command. See drawRect for rationale.
    current->currentVisuals.emplace_back(rect,brush,border);
}

void Canvas::drawEllipse(Composition::Ellipse &ellipse, Core::SharedPtr<Brush> &brush, Core::Optional<Border> border){
    // Phase 6.5: forward the optional border directly into the visual
    // command. See drawRect for rationale.
    current->currentVisuals.emplace_back(ellipse,brush,border);
}

void Canvas::drawLine(Composition::Point2D from,
                      Composition::Point2D to,
                      Core::SharedPtr<Brush> &brush,
                      float strokeWidth){
    if(brush == nullptr || strokeWidth <= 0.f){
        return;
    }
    Path p(from, strokeWidth);
    p.addLine(to);
    p.setPathBrush(brush);
    drawPath(p);
}

void Canvas::drawPolyline(const OmegaCommon::Vector<Composition::Point2D> &points,
                          Core::SharedPtr<Brush> &strokeBrush,
                          float strokeWidth,
                          bool closed,
                          Core::Optional<Core::SharedPtr<Brush>> fillBrush){
    if(points.size() < 2){
        return;
    }
    const bool wantStroke = (strokeBrush != nullptr) && (strokeWidth > 0.f);
    const bool wantFill   = fillBrush.has_value() && (fillBrush.value() != nullptr);
    if(!wantStroke && !wantFill){
        return;
    }

    // Path needs a positive stroke for the parallel-band geometry it
    // tracks even when we only want a fill — the dual-attachment pass
    // ignores the stroke band when no stroke brush is attached.
    const float pathStroke = wantStroke ? strokeWidth : 1.f;

    Path p(points.front(), pathStroke);
    for(std::size_t i = 1; i < points.size(); ++i){
        p.addLine(points[i]);
    }
    if(closed){
        p.close();
    }

    if(wantFill){
        auto fb = fillBrush.value();
        p.setPathBrush(fb);
        if(wantStroke){
            // Fill via pathBrush, stroke via Border — single dual-attachment pass.
            // Border::width is currently `unsigned`; rounded for the
            // attachment but Path geometry stays sub-pixel-accurate.
            const unsigned borderWidth =
                static_cast<unsigned>(std::max(1.f, std::round(strokeWidth)));
            drawPath(p, Border(strokeBrush, borderWidth));
        } else {
            // Fill only.
            drawPath(p, std::nullopt);
        }
    } else {
        // Stroke only.
        p.setPathBrush(strokeBrush);
        drawPath(p);
    }
}

void Canvas::drawText(const UniString &text,
                      Core::SharedPtr<Font> font,
                      const Composition::Rect &rect,
                      const Color &color,
                      const TextLayoutDescriptor &layoutDesc){
    if(font == nullptr || text.length() == 0 || rect.w <= 0.F || rect.h <= 0.f){
        return;
    }

    const float renderScale = (ownerView_ != nullptr) ? ownerView_->getRenderScale() : 1.f;
    auto textRect = TextRect::Create(rect,layoutDesc,renderScale);
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
                      const Composition::Rect &rect,
                      const Color &color){
    drawText(text,
             font,
             rect,
             color,
             TextLayoutDescriptor{TextLayoutDescriptor::LeftUpper,TextLayoutDescriptor::None});
}

void Canvas::drawPath(Path &path){
    auto brush = path.impl_->pathBrush;
    if(brush == nullptr){
        brush = ColorBrush(Color::create8Bit(Color::White8));
    }

    const float strokeWidth = path.impl_->currentStroke;
    const bool isFill = (strokeWidth == 0.f);
    for(auto & segment : path.impl_->segments){
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

void Canvas::drawPath(Path &path, Core::Optional<Border> border){
    // Fill brush is whatever the caller stored on the Path; a null
    // pathBrush means stroke-only. Border carries the stroke brush +
    // width and is fed in alongside the fill so the dual-attachment
    // tessellation emits both bands in a single VectorPath visual.
    auto fillBrush = path.impl_->pathBrush;
    Core::SharedPtr<Brush> strokeBrush;
    float strokeWidth = 0.f;
    if(border.has_value()){
        strokeBrush = border->brush;
        strokeWidth = static_cast<float>(border->width);
    }
    const bool hasFill = (fillBrush != nullptr);
    if(!hasFill && strokeBrush == nullptr){
        return;
    }
    for(auto & segment : path.impl_->segments){
        if(segment.path.size() < 2){
            continue;
        }
        auto pathData = std::make_shared<OmegaGTE::GVectorPath2D>(segment.path);
        current->currentVisuals.emplace_back(pathData,strokeBrush,fillBrush,strokeWidth,segment.closed,hasFill);
    }
}

void Canvas::drawImage(SharedHandle<Media::BitmapImage> &img,const Composition::Rect & rect) {
    current->currentVisuals.emplace_back(img,rect);
}

void Canvas::drawGETexture(SharedHandle<OmegaGTE::GETexture> &img,const Composition::Rect & rect,SharedHandle<OmegaGTE::GEFence> fence) {
    current->currentVisuals.emplace_back(img,fence,rect);
}

void Canvas::applyEffect(SharedHandle<CanvasEffect> &effect){
    if(effect == nullptr){
        return;
    }

    CanvasEffect copied = *effect;
    if(copied.params != nullptr){
        switch(copied.type){
            case CanvasEffect::Type::DirectionalBlur: {
                auto *params = (CanvasEffect::DirectionalBlurParams *)copied.params;
                copied.directionalBlur = *params;
                break;
            }
            case CanvasEffect::Type::GaussianBlur:
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

void Canvas::drawShadow(Composition::Rect & rect,
                        const LayerEffect::DropShadowParams & shadow){
    Composition::Rect shapeRect = rect;
    current->currentVisuals.emplace_back(shadow,shapeRect,0.f,false);
}

void Canvas::drawShadow(Composition::RoundedRect & rect,
                        const LayerEffect::DropShadowParams & shadow){
    Composition::Rect shapeRect {rect.pos,rect.w,rect.h};
    float cornerRadius = std::max(rect.rad_x,rect.rad_y);
    current->currentVisuals.emplace_back(shadow,shapeRect,cornerRadius,false);
}

void Canvas::drawShadow(Composition::Ellipse & ellipse,
                        const LayerEffect::DropShadowParams & shadow){
    Composition::Rect shapeRect {
        Composition::Point2D{ellipse.x - ellipse.rad_x,ellipse.y - ellipse.rad_y},
        ellipse.rad_x * 2.f,
        ellipse.rad_y * 2.f
    };
    current->currentVisuals.emplace_back(shadow,shapeRect,0.f,true);
}

void Canvas::setElementTransform(const Matrix4x4 & matrix){
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
    // Phase 3: stamp the View's window-relative position so the backend
    // can render this frame at the correct offset within the shared
    // window surface.
    if(ownerView_ != nullptr){
        frame->windowOffset = ownerView_->computeWindowOffset();
    }
    current.reset(new CanvasFrame {&layer,rect});
    return frame;
}

void Canvas::sendFrame() {
    auto frame = nextFrame();
    Timestamp ts = std::chrono::high_resolution_clock::now();
    pushFrame(frame,ts);
}

}
