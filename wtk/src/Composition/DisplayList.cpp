#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/Canvas.h"

namespace OmegaWTK::Composition {

void DisplayListReplay::replay(const DisplayList & list, Canvas & canvas){
    for(const auto & op : list.ops()){
        switch(op.type){
            case DrawOp::Rect: {
                auto rect = op.params.rectParams.rect;
                auto brush = op.params.rectParams.brush;
                canvas.drawRect(rect, brush, op.params.rectParams.border);
                break;
            }
            case DrawOp::RoundedRect: {
                auto rect = op.params.roundedRectParams.rect;
                auto brush = op.params.roundedRectParams.brush;
                canvas.drawRoundedRect(rect, brush, op.params.roundedRectParams.border);
                break;
            }
            case DrawOp::Ellipse: {
                auto ellipse = op.params.ellipseParams.ellipse;
                auto brush = op.params.ellipseParams.brush;
                canvas.drawEllipse(ellipse, brush, op.params.ellipseParams.border);
                break;
            }
            case DrawOp::VectorPath: {
                if(op.params.pathParams.path){
                    canvas.drawPath(*op.params.pathParams.path,
                                    op.params.pathParams.border);
                }
                break;
            }
            case DrawOp::TextRun: {
                // Replay copies the sub-runs because the source op is
                // `const` (a `DisplayList` can be replayed more than
                // once). The Tier 4 direct-dispatch path will move
                // them through to the backend instead.
                auto subRuns = op.params.textRunParams.subRuns;
                canvas.drawTextRun(std::move(subRuns),
                                   op.params.textRunParams.rect,
                                   op.params.textRunParams.color);
                break;
            }
            case DrawOp::Bitmap: {
                if(op.params.bitmapParams.img != nullptr){
                    auto img = op.params.bitmapParams.img;
                    canvas.drawImage(img,
                                     op.params.bitmapParams.rect,
                                     op.params.bitmapParams.sourceRect,
                                     op.params.bitmapParams.tintColor);
                }
                else if(op.params.bitmapParams.texture != nullptr){
                    auto tex = op.params.bitmapParams.texture;
                    canvas.drawGETexture(tex,
                                         op.params.bitmapParams.rect,
                                         op.params.bitmapParams.textureFence);
                }
                break;
            }
            case DrawOp::Shadow: {
                const auto & sp = op.params.shadowParams;
                if(sp.isEllipse){
                    Composition::Ellipse ell {
                        sp.shapeRect.pos.x + (sp.shapeRect.w * 0.5f),
                        sp.shapeRect.pos.y + (sp.shapeRect.h * 0.5f),
                        sp.shapeRect.w * 0.5f,
                        sp.shapeRect.h * 0.5f
                    };
                    canvas.drawShadow(ell, sp.shadow);
                }
                else if(sp.cornerRadius > 0.f){
                    Composition::RoundedRect rr {
                        sp.shapeRect.pos,
                        sp.shapeRect.w,
                        sp.shapeRect.h,
                        sp.cornerRadius,
                        sp.cornerRadius
                    };
                    canvas.drawShadow(rr, sp.shadow);
                }
                else {
                    auto rect = sp.shapeRect;
                    canvas.drawShadow(rect, sp.shadow);
                }
                break;
            }
            case DrawOp::SetTransform: {
                canvas.setElementTransform(op.params.transformMatrix);
                break;
            }
            case DrawOp::SetOpacity: {
                canvas.setElementOpacity(op.params.opacityValue);
                break;
            }
            case DrawOp::NativeContent: {
                // Phase 2.5: the Canvas-based GPU path has no
                // native-carve-out concept. The op exists so the
                // NativeViewHost-Adoption-Plan migrations have a
                // shape to emit against; the platform compositor
                // gets a real implementation in Tier 3 when
                // FrameBuilder owns the session.
                break;
            }
            case DrawOp::PushClip:
            case DrawOp::PopClip:
            case DrawOp::PushTransform:
            case DrawOp::PopTransform: {
                // Phase 2.4: state ops exist in the type so Tier 3's
                // FrameBuilder + ScrollView migration is mechanical,
                // but Canvas has no clip / scoped-transform surface
                // for the replay to drive. No Tier-2 producer emits
                // these (UIView::update doesn't push state; SVGView's
                // parsed display list only emits shape ops). Option
                // (b) in plan §2.4: no-op replay. When ScrollView
                // starts emitting `PushClip` in Tier 3, replay
                // becomes a stack accumulator OR the backend
                // dispatch takes over directly.
                break;
            }
        }
    }
}

}
