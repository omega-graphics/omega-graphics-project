#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/TextLayoutEngine.h"
#include "PathImpl.h"
#include "omegaWTK/UI/View.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <unordered_map>
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

VisualCommand::Data::Data(Core::SharedPtr<OmegaCommon::Img::BitmapImage> img,const Composition::Rect &rect) :
bitmapParams({img,nullptr,nullptr,rect,std::nullopt,std::nullopt}){

};

VisualCommand::Data::Data(Core::SharedPtr<OmegaGTE::GETexture> texture,Core::SharedPtr<OmegaGTE::GEFence> textureFence,const Composition::Rect &rect) :
bitmapParams({nullptr,texture,textureFence,rect,std::nullopt,std::nullopt}){

};

VisualCommand::Data::Data(Core::SharedPtr<OmegaCommon::Img::BitmapImage> img,
                          const Composition::Rect & rect,
                          Core::Optional<Composition::Rect> sourceRect,
                          Core::Optional<Composition::Color> tintColor) :
bitmapParams({img,nullptr,nullptr,rect,std::move(sourceRect),std::move(tintColor)}){

};

VisualCommand::Data::Data(const LayerEffect::DropShadowParams & shadow,const Composition::Rect & shapeRect,float cornerRadius,bool isEllipse) :
shadowParams({shadow,shapeRect,cornerRadius,isEllipse}){

};

VisualCommand::Data::Data(OmegaCommon::Vector<TextSubRun> subRuns,
                          const Composition::Rect & rect,
                          const Composition::Color & color) :
textRunParams({std::move(subRuns),rect,color}){

};

VisualCommand::Data::Data(const Matrix4x4 & matrix) :
transformMatrix(matrix){

};

VisualCommand::Data::Data(float opacityVal) :
opacityValue(opacityVal){

};

VisualCommand::Data::Data(Core::Optional<Composition::Rect> clip) :
clipRect(clip){

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

ShapedTextRun shapeTextForDisplayList(
    const OmegaCommon::UniString & text,
    const Core::SharedPtr<Font> & font,
    const Composition::Rect & rect,
    const Composition::Color & color,
    const TextLayoutDescriptor & layoutDesc,
    float renderScale){
    ShapedTextRun out {};
    if(font == nullptr || text.length() == 0 || rect.w <= 0.f || rect.h <= 0.f){
        return out;
    }

    // Text-Layout-Engine-Plan.md Phase 7: the WTK-owned
    // `TextLayoutEngine` is the *only* layout pipeline now. MSDF and
    // BitmapFallback fonts both run through it; the per-sub-run
    // `Font::mode()` only decides how the resolved glyphs reach the
    // screen (atlas-based GPU path vs. CPU rasterizer + texture
    // blit). The legacy `GlyphRun::shape()` / `TextRect::drawRun`
    // pipeline is gone.
    auto * engine = FontEngine::inst();
    auto * shaper = (engine != nullptr) ? engine->shaper() : nullptr;
    if(engine == nullptr || shaper == nullptr){
        return out;
    }
    const FontMetrics metrics = font->getMetrics();
    auto * fallback = engine->fallback();
    auto layoutResult = TextLayoutEngine::layout(
        text, font, metrics, rect, layoutDesc, *shaper, fallback);
    if(layoutResult.glyphs.empty()){
        return out;
    }

    // Group laid-out glyphs by resolved font into sub-runs. Mixed
    // MSDF + BitmapFallback strings (e.g. Latin against a vector
    // face + a color-emoji fallback) produce one sub-run per face;
    // each sub-run is dispatched per its own `Font::mode()` below.
    std::unordered_map<Font *, std::size_t> subRunIndex;
    OmegaCommon::Vector<TextSubRun> subRuns;
    for(const auto & g : layoutResult.glyphs){
        if(g.resolvedFont == nullptr) continue;
        auto it = subRunIndex.find(g.resolvedFont.get());
        if(it == subRunIndex.end()){
            TextSubRun sr;
            sr.resolvedFont = g.resolvedFont;
            subRuns.push_back(std::move(sr));
            it = subRunIndex.emplace(g.resolvedFont.get(),
                                     subRuns.size() - 1).first;
        }
        auto & sr = subRuns[it->second];
        sr.glyphIds.push_back(g.glyphId);
        sr.positions.push_back(Composition::Point2D{g.canvasX, g.canvasY});
    }

    // Partition by mode: MSDF sub-runs are collected into one batch
    // (the renderer emits one atlas-textured quad batch per font).
    // BitmapFallback sub-runs each rasterize to their own offscreen
    // texture via the engine's CPU rasterizer and ride the standard
    // bitmap-blit path.
    for(auto & sr : subRuns){
        if(sr.resolvedFont == nullptr || sr.glyphIds.empty()) continue;
        if(sr.resolvedFont->mode() == Font::Mode::MSDF){
            // Atlas population must happen here on the paint-
            // recording thread — `GlyphAtlas::ensureGlyph` uploads
            // tiles via a texture transfer, which is illegal inside
            // the compositor's frame render pass.
            sr.resolvedFont->ensureGlyphsResident(sr.glyphIds);
            out.msdfSubRuns.push_back(std::move(sr));
        } else {
            auto bmp = engine->rasterizeSubRunToTexture(
                sr, rect, color, renderScale);
            if(bmp.texture != nullptr){
                out.bitmapBlits.push_back({std::move(bmp.texture),
                                           std::move(bmp.fence)});
            }
        }
    }
    return out;
}

void Canvas::drawText(const OmegaCommon::UniString &text,
                      Core::SharedPtr<Font> font,
                      const Composition::Rect &rect,
                      const Color &color,
                      const TextLayoutDescriptor &layoutDesc){
    const float renderScale = (ownerView_ != nullptr)
        ? ownerView_->getRenderScale() : 1.f;
    auto shaped = shapeTextForDisplayList(
        text, font, rect, color, layoutDesc, renderScale);
    for(auto & blit : shaped.bitmapBlits){
        drawGETexture(blit.texture, rect, blit.fence);
    }
    if(!shaped.msdfSubRuns.empty()){
        drawTextRun(std::move(shaped.msdfSubRuns), rect, color);
    }
}

void Canvas::drawText(const OmegaCommon::UniString &text,
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

void Canvas::drawImage(SharedHandle<OmegaCommon::Img::BitmapImage> &img,const Composition::Rect & rect) {
    current->currentVisuals.emplace_back(img,rect);
}

void Canvas::drawImage(SharedHandle<OmegaCommon::Img::BitmapImage> &img,
                       const Composition::Rect & destRect,
                       Core::Optional<Composition::Rect> sourceRect,
                       Core::Optional<Composition::Color> tintColor) {
    current->currentVisuals.emplace_back(img,
                                          destRect,
                                          std::move(sourceRect),
                                          std::move(tintColor));
}

void Canvas::drawImage(SharedHandle<OmegaCommon::Img::BitmapImage> &img,
                       const Composition::Rect & destRect,
                       const NineSliceInsets & insets,
                       Core::Optional<Composition::Rect> sourceRect,
                       Core::Optional<Composition::Color> tintColor) {
    if(img == nullptr){
        return;
    }

    // Resolve the texture-pixel-space source rect: explicit sourceRect
    // when provided, otherwise the full bitmap.
    Composition::Rect srcFull;
    if(sourceRect.has_value()){
        srcFull = *sourceRect;
    }
    else {
        srcFull.pos.x = 0.f;
        srcFull.pos.y = 0.f;
        srcFull.w = static_cast<float>(img->header.width);
        srcFull.h = static_cast<float>(img->header.height);
    }

    // Clamp insets to the source rect's extents so the inner stretch
    // region never collapses below zero.
    const float srcL = std::max(0.f, std::min(insets.left,   srcFull.w));
    const float srcT = std::max(0.f, std::min(insets.top,    srcFull.h));
    const float srcR = std::max(0.f, std::min(insets.right,  srcFull.w - srcL));
    const float srcB = std::max(0.f, std::min(insets.bottom, srcFull.h - srcT));

    // Destination corner sizes mirror the source corner sizes 1:1 (the
    // corners do not stretch). Edges and center take what's left of
    // the destination rect.
    const float dstL = srcL;
    const float dstT = srcT;
    const float dstR = srcR;
    const float dstB = srcB;

    const float dstCx = std::max(0.f, destRect.w - dstL - dstR);
    const float dstCy = std::max(0.f, destRect.h - dstT - dstB);
    const float srcCx = std::max(0.f, srcFull.w - srcL - srcR);
    const float srcCy = std::max(0.f, srcFull.h - srcT - srcB);

    struct Slice {
        float dx, dy, dw, dh;
        float sx, sy, sw, sh;
    };
    const Slice slices[9] = {
        // top-left corner
        { 0.f,           0.f,           dstL, dstT,
          0.f,           0.f,           srcL, srcT },
        // top edge
        { dstL,          0.f,           dstCx, dstT,
          srcL,          0.f,           srcCx, srcT },
        // top-right corner
        { dstL + dstCx,  0.f,           dstR, dstT,
          srcL + srcCx,  0.f,           srcR, srcT },
        // left edge
        { 0.f,           dstT,          dstL, dstCy,
          0.f,           srcT,          srcL, srcCy },
        // center
        { dstL,          dstT,          dstCx, dstCy,
          srcL,          srcT,          srcCx, srcCy },
        // right edge
        { dstL + dstCx,  dstT,          dstR, dstCy,
          srcL + srcCx,  srcT,          srcR, srcCy },
        // bottom-left corner
        { 0.f,           dstT + dstCy,  dstL, dstB,
          0.f,           srcT + srcCy,  srcL, srcB },
        // bottom edge
        { dstL,          dstT + dstCy,  dstCx, dstB,
          srcL,          srcT + srcCy,  srcCx, srcB },
        // bottom-right corner
        { dstL + dstCx,  dstT + dstCy,  dstR, dstB,
          srcL + srcCx,  srcT + srcCy,  srcR, srcB },
    };

    for(const auto & s : slices){
        if(s.dw <= 0.f || s.dh <= 0.f) continue;
        if(s.sw <= 0.f || s.sh <= 0.f) continue;

        Composition::Rect dst;
        dst.pos.x = destRect.pos.x + s.dx;
        dst.pos.y = destRect.pos.y + s.dy;
        dst.w = s.dw;
        dst.h = s.dh;

        Composition::Rect src;
        src.pos.x = srcFull.pos.x + s.sx;
        src.pos.y = srcFull.pos.y + s.sy;
        src.w = s.sw;
        src.h = s.sh;

        current->currentVisuals.emplace_back(img,
                                              dst,
                                              Core::Optional<Composition::Rect>(src),
                                              tintColor);
    }
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

void Canvas::drawTextRun(OmegaCommon::Vector<TextSubRun> subRuns,
                         const Composition::Rect & rect,
                         const Composition::Color & color){
    if(subRuns.empty()){
        return;
    }
    current->currentVisuals.emplace_back(std::move(subRuns), rect, color);
}

void Canvas::setElementTransform(const Matrix4x4 & matrix){
    current->currentVisuals.emplace_back(matrix);
}

void Canvas::setElementOpacity(float opacity){
    current->currentVisuals.emplace_back(opacity);
}

namespace {

// Intersect two rectangles. Returns nullopt if the intersection is
// empty. Used by Canvas::pushClip to compose nested clips and by the
// backend's scissor application to clip against the slice bounds.
Core::Optional<Composition::Rect> intersectRects(const Composition::Rect & a,
                                                 const Composition::Rect & b){
    const float left   = std::max(a.pos.x, b.pos.x);
    const float top    = std::max(a.pos.y, b.pos.y);
    const float right  = std::min(a.pos.x + a.w, b.pos.x + b.w);
    const float bottom = std::min(a.pos.y + a.h, b.pos.y + b.h);
    if(right <= left || bottom <= top){
        return std::nullopt;
    }
    return Composition::Rect{
        Composition::Point2D{left, top},
        right - left,
        bottom - top
    };
}

} // namespace

void Canvas::pushClip(const Composition::Rect & rectIn){
    // Intersect with the current top of stack (when non-empty);
    // when the stack is empty, the new clip stands on its own. The
    // backend receives the *intersected* effective rect — Canvas
    // owns the nesting math so the backend stays stateless.
    Composition::Rect effective = rectIn;
    if(!clipStack_.empty()){
        auto intersected = intersectRects(clipStack_.back(), rectIn);
        if(intersected.has_value()){
            effective = *intersected;
        }
        else {
            // Empty intersection — push a zero-area rect at the
            // current top's origin so a matching popClip restores
            // the prior clip correctly. Backend gets a degenerate
            // scissor (all draws culled while this clip is active),
            // which is the correct visual answer.
            effective = Composition::Rect{
                clipStack_.back().pos, 0.f, 0.f};
        }
    }
    clipStack_.push_back(effective);
    current->currentVisuals.emplace_back(
        Core::Optional<Composition::Rect>{effective});
}

void Canvas::popClip(){
    if(clipStack_.empty()){
        // Imbalanced pop — `FrameBuilder::submitView` asserts in
        // debug. Treat as a no-op in release to avoid corrupting
        // the backend's scissor state.
        return;
    }
    clipStack_.pop_back();
    Core::Optional<Composition::Rect> next =
        clipStack_.empty()
            ? Core::Optional<Composition::Rect>{}
            : Core::Optional<Composition::Rect>{clipStack_.back()};
    current->currentVisuals.emplace_back(next);
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
    // Tier 3 Phase 3.5: defensively clear the clip stack at frame
    // boundaries. If a producer's display list ended with an
    // unbalanced push, this prevents the leaked clip from being
    // intersected into the next frame's first pushClip. The
    // FrameBuilder's debug balance assert is the upstream catch.
    clipStack_.clear();
    return frame;
}

void Canvas::sendFrame() {
    auto frame = nextFrame();
    Timestamp ts = std::chrono::high_resolution_clock::now();
    pushFrame(frame,ts);
}

}
