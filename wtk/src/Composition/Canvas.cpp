#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Layer.h"

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
                          float strokeWidth,
                          bool contour,
                          bool fill):
pathParams({path,brush,strokeWidth,contour,fill}){

}

VisualCommand::Data::Data(Core::SharedPtr<Media::BitmapImage> img,const Core::Rect &rect) :
bitmapParams({img,nullptr,nullptr,rect}){

};

VisualCommand::Data::Data(Core::SharedPtr<OmegaGTE::GETexture> texture,Core::SharedPtr<OmegaGTE::GEFence> textureFence,const Core::Rect &rect) :
bitmapParams({nullptr,texture,textureFence,rect}){

};



void VisualCommand::Data::_destroy(Type t){
    (void)t;
}

VisualCommand::~VisualCommand(){
    // Data is a regular struct (not a tagged union), so RAII handles destruction.
}

Canvas::Canvas(CompositorClientProxy &proxy,Layer &layer): CompositorClient(proxy),rect(layer.getLayerRect()),layer(layer),current(new CanvasFrame {&layer,rect}){

};

Layer & Canvas::getCorrespondingLayer(){
    return layer;
}

// Layer * Canvas::getParentLayer(){
//     return parentLayer;
// };

void Canvas::drawRect(Core::Rect &rect, Core::SharedPtr<Brush> &brush){
    current->currentVisuals.emplace_back(rect,brush,Core::Optional<Border>{});
};

void Canvas::drawRoundedRect(Core::RoundedRect &rect, Core::SharedPtr<Brush> &brush){
    current->currentVisuals.emplace_back(rect,brush,Core::Optional<Border>{});
}

void Canvas::drawEllipse(Core::Ellipse &ellipse, Core::SharedPtr<Brush> &brush){
    current->currentVisuals.emplace_back(ellipse,brush,Core::Optional<Border>{});
}

void Canvas::drawText(const UniString &text,
                      Core::SharedPtr<Font> font,
                      const Core::Rect &rect,
                      const Color &color,
                      const TextLayoutDescriptor &layoutDesc){
    if(font == nullptr || text.isEmpty() || rect.w <= 0.f || rect.h <= 0.f){
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
    for(auto & segment : path.segments){
        if(segment.path.size() < 2){
            continue;
        }
        auto pathData = std::make_shared<OmegaGTE::GVectorPath2D>(segment.path);
        current->currentVisuals.emplace_back(pathData,brush,strokeWidth,segment.closed,false);
    }
}

void Canvas::drawImage(SharedHandle<Media::BitmapImage> &img,const Core::Rect & rect) {
    current->currentVisuals.emplace_back(img,rect);
}

void Canvas::drawGETexture(SharedHandle<OmegaGTE::GETexture> &img,const Core::Rect & rect,SharedHandle<OmegaGTE::GEFence> fence) {
    current->currentVisuals.emplace_back(img,fence,rect);
}

SharedHandle<CanvasFrame> Canvas::getCurrentFrame() {
    return current;
}

SharedHandle<CanvasFrame> Canvas::nextFrame() {
    auto frame = getCurrentFrame();
    current.reset(new CanvasFrame {&layer,rect});
    return frame;
}

void Canvas::sendFrame() {
    auto frame = nextFrame();
    Timestamp ts = std::chrono::high_resolution_clock::now();
    pushFrame(frame,ts);
}

}
