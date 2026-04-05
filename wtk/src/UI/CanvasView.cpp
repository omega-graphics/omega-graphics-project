#include "omegaWTK/UI/CanvasView.h"

namespace OmegaWTK {

    // --- CanvasView ---

    CanvasView::CanvasView(const Core::Rect & rect,ViewPtr parent):
    View(rect,parent){
        auto & rootLayer = getLayerTree()->getRootLayer();
        rootCanvas_ = makeCanvas(rootLayer);
    }

    Composition::Canvas & CanvasView::rootCanvas(){
        return *rootCanvas_;
    }

    void CanvasView::clear(const Composition::Color & color){
        auto & background = rootCanvas_->getCurrentFrame()->background;
        background.r = color.r;
        background.g = color.g;
        background.b = color.b;
        background.a = color.a;
    }

    void CanvasView::drawRect(const Core::Rect & rect,const SharedHandle<Composition::Brush> & brush){
        auto _rect = rect;
        auto _brush = brush;
        rootCanvas_->drawRect(_rect,_brush);
    }

    void CanvasView::drawRoundedRect(const Core::RoundedRect & rect,const SharedHandle<Composition::Brush> & brush){
        auto _rect = rect;
        auto _brush = brush;
        rootCanvas_->drawRoundedRect(_rect,_brush);
    }

    void CanvasView::drawImage(const SharedHandle<Media::BitmapImage> & img,const Core::Rect & rect){
        auto _rect = rect;
        auto _img = img;
        rootCanvas_->drawImage(_img,_rect);
    }

    void CanvasView::drawText(const UniString & text,
                            const SharedHandle<Composition::Font> & font,
                            const Core::Rect & rect,
                            const Composition::Color & color,
                            const Composition::TextLayoutDescriptor & layoutDesc){
        auto _rect = rect;
        auto _font = font;
        rootCanvas_->drawText(text,_font,_rect,color,layoutDesc);
    }

    void CanvasView::drawText(const UniString & text,
                            const SharedHandle<Composition::Font> & font,
                            const Core::Rect & rect,
                            const Composition::Color & color){
        auto _rect = rect;
        auto _font = font;
        rootCanvas_->drawText(text,_font,_rect,color);
    }

    void CanvasView::submitPaintFrame(int submissions){
        for(int i = 0; i < submissions; i++){
            rootCanvas_->sendFrame();
        }
    }

};