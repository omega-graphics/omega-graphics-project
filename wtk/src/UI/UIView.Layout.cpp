#include "UIViewImpl.h"
#include "AnimationScheduler.h"
#include "FrameBuilder.h"
#include "omegaWTK/UI/AppWindow.h"

namespace OmegaWTK {

void UIView::Impl::convertLegacyLayoutToV2(){
    UIViewLayoutV2 v2 {};
    for(const auto & element : currentLayout.elements()){
        UIElementLayoutSpec spec {};
        spec.tag = element.tag;
        spec.layout.width = LayoutLength::Auto();
        spec.layout.height = LayoutLength::Auto();
        if(element.shape){
            spec.shape = element.shape;
        }
        if(element.str){
            spec.text = element.str;
        }
        if(element.textRect){
            spec.textRect = element.textRect;
        }
        if(element.textStyleTag){
            spec.textStyleTag = element.textStyleTag;
        }
        if(element.image){
            spec.image = element.image;
        }
        if(element.imageRect){
            spec.imageRect = element.imageRect;
        }
        v2.element(spec);
    }
    currentLayoutV2_ = std::move(v2);
    layoutDirty = true;
    markAllElementsDirty();
    firstFrameCoherentSubmit = true;
}

void UIView::applyLayoutDelta(const UIElementTag & elementTag,
                              const LayoutDelta & delta,
                              const LayoutTransitionSpec & spec){
    // Phase 4.4 (Anim Tier B): the per-element layout tween. Pre-4.4 this
    // queued `Composition::LayerAnimator::resizeTransition` on the
    // element's animation layer; the AnimationScheduler now carries the
    // four scalar tracks keyed by `(elementNodeId, LayoutX/Y/Width/Height)`.
    //
    // 4.7 seam: same as `View::applyLayoutDelta` — the element rect read-
    // back from the scheduler lands with the centralized Layout phase.
    // No caller exists today.
    if(!spec.enabled || spec.durationSec <= 0.f || delta.changedProperties.empty()){
        return;
    }

    auto * fb = AppWindow::activeFrameBuilder();
    auto * scheduler = (fb != nullptr) ? fb->animationScheduler() : nullptr;
    if(scheduler == nullptr){
        return;
    }

    const auto & from = delta.fromRectPx;
    const auto & to   = delta.toRectPx;
    if(from.pos.x == to.pos.x && from.pos.y == to.pos.y &&
       from.w     == to.w     && from.h     == to.h){
        return;
    }

    Composition::TimingOptions timing {};
    timing.durationMs = static_cast<std::uint32_t>(spec.durationSec * 1000.f);
    if(timing.durationMs == 0){
        return;
    }
    auto curve = spec.curve;
    if(curve == nullptr){
        curve = Composition::AnimationCurve::Linear();
    }

    const auto node = impl_->ensureElementNodeId(elementTag);
    if(from.pos.x != to.pos.x){
        scheduler->tweenProperty<float>(node, PropertyKey::LayoutX,
                                        from.pos.x, to.pos.x, timing, curve);
    }
    if(from.pos.y != to.pos.y){
        scheduler->tweenProperty<float>(node, PropertyKey::LayoutY,
                                        from.pos.y, to.pos.y, timing, curve);
    }
    if(from.w != to.w){
        scheduler->tweenProperty<float>(node, PropertyKey::LayoutWidth,
                                        from.w, to.w, timing, curve);
    }
    if(from.h != to.h){
        scheduler->tweenProperty<float>(node, PropertyKey::LayoutHeight,
                                        from.h, to.h, timing, curve);
    }
}

}
