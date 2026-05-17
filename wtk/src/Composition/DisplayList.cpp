#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/Canvas.h"

#include <iostream>

namespace OmegaWTK::Composition {

namespace {

// Tier 3 Phase 3.5: `PushTransform` / `PopTransform` stay no-op for
// now (no in-tree producer emits them; the only matrices a Tier-2
// path would carry would be 3D-effect transforms, and the producers
// don't exist yet). Emit one warning per replay so the absence is
// visible to anyone wiring up a transform producer ahead of the
// Tier-3 implementation. Static-once guard so the warning doesn't
// spam every replay tick.
void warnTransformOpUnsupported(const char * which){
    static bool warned = false;
    if(!warned){
        std::cerr << "[WTK] DisplayListReplay: " << which
                  << " is no-op until a Tier-3 producer wires up "
                  << "a per-canvas transform stack." << std::endl;
        warned = true;
    }
}

} // namespace

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
            case DrawOp::PushClip: {
                // Tier 3 Phase 3.5: route to `Canvas::pushClip`,
                // which owns the clip stack and the intersection
                // math. The Canvas emits a `VisualCommand::SetClip`
                // with the effective (intersected) rect, which the
                // backend turns into a GPU scissor.
                canvas.pushClip(op.params.pushClipParams.rect);
                break;
            }
            case DrawOp::PopClip: {
                canvas.popClip();
                break;
            }
            case DrawOp::PushTransform: {
                // No in-tree producer yet (see warnTransformOpUnsupported).
                // Phase 3.5 keeps this as a logged no-op so a future
                // producer surfaces immediately when it shows up.
                warnTransformOpUnsupported("PushTransform");
                break;
            }
            case DrawOp::PopTransform: {
                warnTransformOpUnsupported("PopTransform");
                break;
            }
        }
    }
}

}
