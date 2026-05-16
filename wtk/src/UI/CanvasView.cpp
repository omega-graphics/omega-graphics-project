#include "omegaWTK/UI/CanvasView.h"
#include "omegaWTK/Composition/DisplayList.h"

namespace OmegaWTK {

    // --- CanvasView ---
    //
    // UIView-Render-Redesign-Plan Tier 2 Phase 2.6. The imperative
    // `draw*` methods are marked `[[deprecated]]` in the header and
    // route through `Composition::DisplayList` + `DisplayListReplay`
    // so they share the Phase 2.1 contract UIView migrated to. Each
    // call builds a one-op DisplayList and replays it into
    // `rootCanvas_` — same GPU path, same output, but every imperative
    // draw now expresses itself as a `DrawOp`. When Tier 3 deletes the
    // class, the replay scaffolding goes away with it.

    CanvasView::CanvasView(const Composition::Rect & rect,ViewPtr parent):
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

    void CanvasView::drawRect(const Composition::Rect & rect,const SharedHandle<Composition::Brush> & brush){
        Composition::DisplayList list;
        list.append(Composition::DrawOp{rect, brush});
        Composition::DisplayListReplay::replay(list, *rootCanvas_);
    }

    void CanvasView::drawRoundedRect(const Composition::RoundedRect & rect,const SharedHandle<Composition::Brush> & brush){
        Composition::DisplayList list;
        list.append(Composition::DrawOp{rect, brush});
        Composition::DisplayListReplay::replay(list, *rootCanvas_);
    }

    void CanvasView::drawImage(const SharedHandle<OmegaCommon::Img::BitmapImage> & img,const Composition::Rect & rect){
        Composition::DisplayList list;
        list.append(Composition::DrawOp{img, rect});
        Composition::DisplayListReplay::replay(list, *rootCanvas_);
    }

    void CanvasView::drawText(const OmegaCommon::UniString & text,
                            const SharedHandle<Composition::Font> & font,
                            const Composition::Rect & rect,
                            const Composition::Color & color,
                            const Composition::TextLayoutDescriptor & layoutDesc){
        // Same shape-up-front-then-emit pattern as UIView::update's
        // text branch. `shapeTextForDisplayList` runs layout +
        // fallback rasterization with no Canvas side effects; the
        // resulting MSDF sub-runs become a `DrawOp::TextRun`, and
        // each bitmap-fallback blit becomes a `DrawOp::Bitmap`.
        auto shaped = Composition::shapeTextForDisplayList(
            text, font, rect, color, layoutDesc, getRenderScale());
        Composition::DisplayList list;
        for (auto & blit : shaped.bitmapBlits){
            list.append(Composition::DrawOp{blit.texture, blit.fence, rect});
        }
        if (!shaped.msdfSubRuns.empty()){
            list.append(Composition::DrawOp{
                std::move(shaped.msdfSubRuns), rect, color});
        }
        Composition::DisplayListReplay::replay(list, *rootCanvas_);
    }

    void CanvasView::drawText(const OmegaCommon::UniString & text,
                            const SharedHandle<Composition::Font> & font,
                            const Composition::Rect & rect,
                            const Composition::Color & color){
        drawText(text, font, rect, color, Composition::TextLayoutDescriptor{});
    }

    void CanvasView::submitPaintFrame(int submissions){
        for(int i = 0; i < submissions; i++){
            rootCanvas_->sendFrame();
        }
    }

};
