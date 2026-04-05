#include "UIViewImpl.h"

namespace OmegaWTK {

void UIView::Impl::convertLegacyLayoutToV2(){
    UIViewLayoutV2 v2 {};
    for(const auto & element : currentLayout.elements()){
        UIElementLayoutSpec spec {};
        spec.tag = element.tag;
        spec.style.width = LayoutLength::Auto();
        spec.style.height = LayoutLength::Auto();
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
    if(!spec.enabled || spec.durationSec <= 0.f || delta.changedProperties.empty()){
        return;
    }
    auto layerAnimator = impl_->ensureAnimationLayerAnimator(elementTag);
    if(layerAnimator == nullptr){
        return;
    }

    int dx = static_cast<int>(delta.toRectPx.pos.x - delta.fromRectPx.pos.x);
    int dy = static_cast<int>(delta.toRectPx.pos.y - delta.fromRectPx.pos.y);
    int dw = static_cast<int>(delta.toRectPx.w - delta.fromRectPx.w);
    int dh = static_cast<int>(delta.toRectPx.h - delta.fromRectPx.h);

    if(dx == 0 && dy == 0 && dw == 0 && dh == 0){
        return;
    }

    unsigned durationMs = static_cast<unsigned>(spec.durationSec * 1000.f);
    if(durationMs == 0){
        return;
    }

    auto curve = spec.curve;
    if(curve == nullptr){
        curve = Composition::AnimationCurve::Linear();
    }
    layerAnimator->resizeTransition(dx,dy,dw,dh,durationMs,curve);
}

}
