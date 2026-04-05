#ifndef OMEGAWTK_UI_CANVASVIEW_H
#define OMEGAWTK_UI_CANVASVIEW_H

#include "View.h"
#include "omegaWTK/Composition/Canvas.h"

namespace OmegaWTK {

/**
 @brief A View subclass that owns a Canvas on its root Layer.

 Widgets backed by a CanvasView draw through the clear/drawRect/drawText
 methods below. Specialized View subclasses (SVGView, UIView, VideoView)
 own their own Canvases and do not inherit from CanvasView.
*/
class OMEGAWTK_EXPORT CanvasView : public View {
    SharedHandle<Composition::Canvas> rootCanvas_;
    friend class Widget;
public:
    CanvasView(const Core::Rect & rect,ViewPtr parent = nullptr);

    static SharedHandle<CanvasView> Create(const Core::Rect & rect,ViewPtr parent = nullptr){
        return SharedHandle<CanvasView>(new CanvasView(rect,parent));
    }

    Composition::Canvas & rootCanvas();

    void clear(const Composition::Color & color);
    void drawRect(const Core::Rect & rect,const SharedHandle<Composition::Brush> & brush);
    void drawRoundedRect(const Core::RoundedRect & rect,const SharedHandle<Composition::Brush> & brush);
    void drawImage(const SharedHandle<Media::BitmapImage> & img,const Core::Rect & rect);
    void drawText(const UniString & text,
                  const SharedHandle<Composition::Font> & font,
                  const Core::Rect & rect,
                  const Composition::Color & color,
                  const Composition::TextLayoutDescriptor & layoutDesc);
    void drawText(const UniString & text,
                  const SharedHandle<Composition::Font> & font,
                  const Core::Rect & rect,
                  const Composition::Color & color);

    void submitPaintFrame(int submissions) override;
};

}

#endif
