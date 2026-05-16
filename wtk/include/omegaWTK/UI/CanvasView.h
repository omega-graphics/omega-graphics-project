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

 UIView-Render-Redesign-Plan Tier 2 Phase 2.6: the imperative
 `draw*` methods are deprecated. They still work — internally they
 now build a one-op `Composition::DisplayList` and replay it into
 `rootCanvas_`, matching the Phase 2.1 contract UIView migrated to.
 The deprecation marker is the §8 grep handle: every remaining
 caller must move to `UIView` + `UIViewLayoutV2` before Tier 3
 deletes this class. `clear()` and `submitPaintFrame(int)` are not
 deprecated yet — `clear` writes a frame-channel background color
 (not a draw op), and `submitPaintFrame` is part of the session
 lifecycle that the FrameBuilder relocates in Tier 3.
*/
class OMEGAWTK_EXPORT CanvasView : public View {
    SharedHandle<Composition::Canvas> rootCanvas_;
    friend class Widget;
public:
    CanvasView(const Composition::Rect & rect,ViewPtr parent = nullptr);

    static SharedHandle<CanvasView> Create(const Composition::Rect & rect,ViewPtr parent = nullptr){
        return SharedHandle<CanvasView>(new CanvasView(rect,parent));
    }

    Composition::Canvas & rootCanvas();

    void clear(const Composition::Color & color);

    [[deprecated("CanvasView's imperative draw API is removed in Tier 3 of UIView-Render-Redesign-Plan. Migrate to UIView + UIViewLayoutV2.")]]
    void drawRect(const Composition::Rect & rect,const SharedHandle<Composition::Brush> & brush);

    [[deprecated("CanvasView's imperative draw API is removed in Tier 3 of UIView-Render-Redesign-Plan. Migrate to UIView + UIViewLayoutV2.")]]
    void drawRoundedRect(const Composition::RoundedRect & rect,const SharedHandle<Composition::Brush> & brush);

    [[deprecated("CanvasView's imperative draw API is removed in Tier 3 of UIView-Render-Redesign-Plan. Migrate to UIView + UIViewLayoutV2.")]]
    void drawImage(const SharedHandle<OmegaCommon::Img::BitmapImage> & img,const Composition::Rect & rect);

    [[deprecated("CanvasView's imperative draw API is removed in Tier 3 of UIView-Render-Redesign-Plan. Migrate to UIView + UIViewLayoutV2.")]]
    void drawText(const OmegaCommon::UniString & text,
                  const SharedHandle<Composition::Font> & font,
                  const Composition::Rect & rect,
                  const Composition::Color & color,
                  const Composition::TextLayoutDescriptor & layoutDesc);

    [[deprecated("CanvasView's imperative draw API is removed in Tier 3 of UIView-Render-Redesign-Plan. Migrate to UIView + UIViewLayoutV2.")]]
    void drawText(const OmegaCommon::UniString & text,
                  const SharedHandle<Composition::Font> & font,
                  const Composition::Rect & rect,
                  const Composition::Color & color);

    void submitPaintFrame(int submissions) override;
};

}

#endif
