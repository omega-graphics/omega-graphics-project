#include "UIViewImpl.h"

#include <unordered_map>

namespace OmegaWTK {

namespace UIViewInternal {

namespace {

bool isValidDimension(float v){
    return std::isfinite(v) && v > 0.f;
}

float clampDrawableDimension(float v){
#if defined(TARGET_MACOS)
    constexpr float kMaxDrawableDimension = 8192.f;
#else
    constexpr float kMaxDrawableDimension = 16384.f;
#endif
    if(!std::isfinite(v)){
        return 1.f;
    }
    return std::clamp(v,1.f,kMaxDrawableDimension);
}

bool isSuspiciousDimensionPair(float w,float h){
    if(!std::isfinite(w) || !std::isfinite(h) || w <= 0.f || h <= 0.f){
        return true;
    }
    const float maxDim = std::max(w,h);
    const float minDim = std::min(w,h);
    if(maxDim >= 4096.f && minDim <= 1.5f){
        return true;
    }
    if(maxDim >= 1024.f && minDim > 0.f){
        const float aspect = maxDim / minDim;
        if(aspect > 256.f){
            return true;
        }
    }
    return false;
}

}

Core::Rect localBoundsFromView(UIView *view){
    constexpr Core::Rect kFallbackRect{
            Core::Position{0.f,0.f},
            1.f,
            1.f
    };
    if(view == nullptr){
        return kFallbackRect;
    }

    struct StableBoundsState {
        bool hasStable = false;
        Core::Rect bounds {Core::Position{0.f,0.f},1.f,1.f};
    };
    static std::unordered_map<UIView *,StableBoundsState> stableBoundsByView {};

    const auto & viewRect = view->getRect();
    float viewWidth = viewRect.w;
    float viewHeight = viewRect.h;
    float limbWidth = 0.f;
    float limbHeight = 0.f;

    auto *tree = view->getLayerTree();
    if(tree != nullptr && tree->getRootLayer() != nullptr){
        const auto & treeRect = tree->getRootLayer()->getLayerRect();
        limbWidth = treeRect.w;
        limbHeight = treeRect.h;
    }

    const bool viewValid = isValidDimension(viewWidth) &&
                           isValidDimension(viewHeight) &&
                           !isSuspiciousDimensionPair(viewWidth,viewHeight);
    const bool limbValid = isValidDimension(limbWidth) &&
                           isValidDimension(limbHeight) &&
                           !isSuspiciousDimensionPair(limbWidth,limbHeight);

    float width = 0.f;
    float height = 0.f;
    bool candidateValid = false;
    if(viewValid){
        width = viewWidth;
        height = viewHeight;
        candidateValid = true;
    }
    else if(limbValid){
        width = limbWidth;
        height = limbHeight;
        candidateValid = true;
    }

    auto & stable = stableBoundsByView[view];
    if(!candidateValid){
        if(stable.hasStable){
            return stable.bounds;
        }
        return kFallbackRect;
    }

    width = clampDrawableDimension(width);
    height = clampDrawableDimension(height);

    Core::Rect resolved {
            Core::Position{0.f,0.f},
            width,
            height
    };
    stable.hasStable = true;
    stable.bounds = resolved;
    return resolved;
}

}

void UIView::update(){
    if(impl_->rootCanvas == nullptr){
        impl_->rootCanvas = makeCanvas(getLayerTree()->getRootLayer());
        impl_->firstFrameCoherentSubmit = true;
    }

    const auto & v2Elements = impl_->currentLayoutV2_.elements();
    if(v2Elements.empty()){
        impl_->layoutDirty = false;
        impl_->styleDirty = false;
        impl_->styleDirtyGlobal = false;
        impl_->styleChangeRequiresCoherentFrame = false;
        impl_->firstFrameCoherentSubmit = false;
        ++impl_->lastUpdateDiagnostics.revision;
        return;
    }

    const auto localBounds = UIViewInternal::localBoundsFromView(this);
    const float dpiScale = 1.f;
    LayoutContext ctx {};
    ctx.availableRectPx = localBounds;
    ctx.dpiScale = dpiScale;
    const auto availDp = ctx.availableRectDp();

    OmegaCommon::Vector<StyleRule> layoutRules {};
    if(impl_->currentStyle != nullptr){
        layoutRules = convertEntriesToRules(*impl_->currentStyle,impl_->tag);
    }

    struct V2Resolved {
        UIElementTag tag;
        const UIElementLayoutSpec * spec;
        Core::Rect resolvedRectDp {};
        Core::Rect resolvedRectPx {};
        int zIndex = 0;
        std::size_t insertionOrder = 0;
    };
    OmegaCommon::Vector<V2Resolved> resolved {};
    resolved.reserve(v2Elements.size());

    for(std::size_t i = 0; i < v2Elements.size(); ++i){
        const auto & spec = v2Elements[i];
        LayoutStyle effectiveStyle = spec.style;
        mergeLayoutRulesIntoStyle(effectiveStyle,layoutRules,spec.tag);

        Core::Rect rectDp = resolveClampedRect(effectiveStyle,availDp,dpiScale);
        Core::Rect rectPx {
            Core::Position{rectDp.pos.x * dpiScale,rectDp.pos.y * dpiScale},
            rectDp.w * dpiScale,
            rectDp.h * dpiScale
        };

        if(impl_->diagnosticSink_ != nullptr){
            impl_->diagnosticSink_->record(LayoutDiagnosticEntry{
                spec.tag,rectDp,rectPx,LayoutDiagnosticEntry::Pass::Arrange});
        }

        V2Resolved entry {};
        entry.tag = spec.tag;
        entry.spec = &spec;
        entry.resolvedRectDp = rectDp;
        entry.resolvedRectPx = rectPx;
        entry.zIndex = spec.zIndex;
        entry.insertionOrder = i;
        resolved.push_back(entry);
    }

    std::stable_sort(resolved.begin(),resolved.end(),
        [](const V2Resolved & a,const V2Resolved & b){
            if(a.zIndex != b.zIndex){
                return a.zIndex < b.zIndex;
            }
            return a.insertionOrder < b.insertionOrder;
        });

    OmegaCommon::Vector<UIElementTag> nextOrder {};
    nextOrder.reserve(resolved.size());
    for(const auto & r : resolved){
        nextOrder.push_back(r.tag);
    }

    OmegaCommon::Vector<UIElementTag> previousOrder = impl_->activeTagOrder;
    const bool orderChanged = previousOrder.size() != nextOrder.size() ||
                              !std::equal(previousOrder.begin(),previousOrder.end(),
                                          nextOrder.begin(),nextOrder.end());
    (void)orderChanged;
    impl_->activeTagOrder = nextOrder;

    startCompositionSession();

    auto viewStyle = UIViewInternal::resolveViewStyle(impl_->currentStyle,impl_->tag);
    auto backgroundColor = viewStyle.backgroundColor.value_or(Composition::Color::Transparent);

    auto & rootBackground = impl_->rootCanvas->getCurrentFrame()->background;
    rootBackground.r = backgroundColor.r;
    rootBackground.g = backgroundColor.g;
    rootBackground.b = backgroundColor.b;
    rootBackground.a = backgroundColor.a;

    auto rootBgBrush = Composition::ColorBrush(backgroundColor);
    auto rootBgRect = localBounds;
    impl_->rootCanvas->drawRect(rootBgRect,rootBgBrush);

    ChildResizeSpec layoutClamp {};
    layoutClamp.resizable = true;
    layoutClamp.policy = ChildResizePolicy::FitContent;

    for(const auto & entry : resolved){
        const auto & spec = *entry.spec;

        auto previousRectIt = impl_->lastResolvedV2Rects_.find(entry.tag);
        Core::Rect previousRectPx = (previousRectIt != impl_->lastResolvedV2Rects_.end())
                                        ? previousRectIt->second
                                        : entry.resolvedRectPx;
        LayoutDelta delta = computeLayoutDelta(previousRectPx,entry.resolvedRectPx);
        impl_->lastResolvedV2Rects_[entry.tag] = entry.resolvedRectPx;

        if(!delta.changedProperties.empty()){
            auto transSpec = resolveLayoutTransition(layoutRules,entry.tag);
            if(transSpec && transSpec->enabled){
                applyLayoutDelta(entry.tag,delta,*transSpec);
            }
        }

        if(impl_->diagnosticSink_ != nullptr){
            impl_->diagnosticSink_->record(LayoutDiagnosticEntry{
                entry.tag,entry.resolvedRectDp,entry.resolvedRectPx,
                LayoutDiagnosticEntry::Pass::Commit});
        }

        auto effectStyle = UIViewInternal::resolveElementEffectStyle(impl_->currentStyle,impl_->tag,entry.tag);

        if(spec.shape){
            auto shapeToDraw = *spec.shape;
            auto brush = UIViewInternal::resolveElementBrush(impl_->currentStyle,impl_->tag,entry.tag);

            if(effectStyle.dropShadow){
                auto shadowParams = *effectStyle.dropShadow;
                if(auto v = impl_->animatedValue(entry.tag,Impl::EffectAnimationKeyShadowOffsetX); v)
                    shadowParams.x_offset = *v;
                if(auto v = impl_->animatedValue(entry.tag,Impl::EffectAnimationKeyShadowOffsetY); v)
                    shadowParams.y_offset = *v;
                if(auto v = impl_->animatedValue(entry.tag,Impl::EffectAnimationKeyShadowRadius); v)
                    shadowParams.radius = *v;
                if(auto v = impl_->animatedValue(entry.tag,Impl::EffectAnimationKeyShadowBlur); v)
                    shadowParams.blurAmount = *v;
                if(auto v = impl_->animatedValue(entry.tag,Impl::EffectAnimationKeyShadowOpacity); v)
                    shadowParams.opacity = *v;

                switch(shapeToDraw.type){
                    case Shape::Type::Rect: {
                        auto rect = shapeToDraw.rect;
                        rect = ViewResizeCoordinator::clampRectToParent(rect,localBounds,layoutClamp);
                        impl_->rootCanvas->drawShadow(rect,shadowParams);
                        break;
                    }
                    case Shape::Type::RoundedRect: {
                        auto rr = shapeToDraw.roundedRect;
                        Core::Rect clampedRect {rr.pos,rr.w,rr.h};
                        clampedRect = ViewResizeCoordinator::clampRectToParent(clampedRect,localBounds,layoutClamp);
                        rr.pos = clampedRect.pos;
                        rr.w = clampedRect.w;
                        rr.h = clampedRect.h;
                        rr.rad_x = std::min(rr.rad_x,rr.w * 0.5f);
                        rr.rad_y = std::min(rr.rad_y,rr.h * 0.5f);
                        impl_->rootCanvas->drawShadow(rr,shadowParams);
                        break;
                    }
                    case Shape::Type::Ellipse: {
                        const auto & srcEllipse = shapeToDraw.ellipse;
                        Core::Rect ellipseRect {
                            Core::Position{srcEllipse.x - srcEllipse.rad_x,srcEllipse.y - srcEllipse.rad_y},
                            std::max(1.f,srcEllipse.rad_x * 2.f),
                            std::max(1.f,srcEllipse.rad_y * 2.f)
                        };
                        ellipseRect = ViewResizeCoordinator::clampRectToParent(ellipseRect,localBounds,layoutClamp);
                        Core::Ellipse ellipse {
                            ellipseRect.pos.x + (ellipseRect.w * 0.5f),
                            ellipseRect.pos.y + (ellipseRect.h * 0.5f),
                            ellipseRect.w * 0.5f,
                            ellipseRect.h * 0.5f
                        };
                        impl_->rootCanvas->drawShadow(ellipse,shadowParams);
                        break;
                    }
                    default:
                        break;
                }
            }

            switch(shapeToDraw.type){
                case Shape::Type::Rect: {
                    auto rect = shapeToDraw.rect;
                    rect = ViewResizeCoordinator::clampRectToParent(rect,localBounds,layoutClamp);
                    impl_->rootCanvas->drawRect(rect,brush);
                    break;
                }
                case Shape::Type::RoundedRect: {
                    auto rect = shapeToDraw.roundedRect;
                    Core::Rect clampedRect {rect.pos,rect.w,rect.h};
                    clampedRect = ViewResizeCoordinator::clampRectToParent(clampedRect,localBounds,layoutClamp);
                    rect.pos = clampedRect.pos;
                    rect.w = clampedRect.w;
                    rect.h = clampedRect.h;
                    rect.rad_x = std::min(rect.rad_x,rect.w * 0.5f);
                    rect.rad_y = std::min(rect.rad_y,rect.h * 0.5f);
                    impl_->rootCanvas->drawRoundedRect(rect,brush);
                    break;
                }
                case Shape::Type::Ellipse: {
                    const auto & srcEllipse = shapeToDraw.ellipse;
                    Core::Rect ellipseRect {
                        Core::Position{
                            srcEllipse.x - srcEllipse.rad_x,
                            srcEllipse.y - srcEllipse.rad_y
                        },
                        std::max(1.f,srcEllipse.rad_x * 2.f),
                        std::max(1.f,srcEllipse.rad_y * 2.f)
                    };
                    ellipseRect = ViewResizeCoordinator::clampRectToParent(ellipseRect,localBounds,layoutClamp);
                    Core::Ellipse ellipse {
                        ellipseRect.pos.x + (ellipseRect.w * 0.5f),
                        ellipseRect.pos.y + (ellipseRect.h * 0.5f),
                        ellipseRect.w * 0.5f,
                        ellipseRect.h * 0.5f
                    };
                    impl_->rootCanvas->drawEllipse(ellipse,brush);
                    break;
                }
                case Shape::Type::Path: {
                    if(shapeToDraw.path){
                        auto vectorPath = *shapeToDraw.path;
                        auto path = Composition::Path(vectorPath,shapeToDraw.pathStrokeWidth);
                        if(shapeToDraw.closePath){
                            path.close();
                        }
                        path.setPathBrush(brush);
                        impl_->rootCanvas->drawPath(path);
                    }
                    break;
                }
                default:
                    break;
            }
        }
        else if(spec.text){
            UIElementTag textStyleTag = spec.textStyleTag.value_or(entry.tag);
            auto textStyle = UIViewInternal::resolveTextStyle(impl_->currentStyle,impl_->tag,textStyleTag);
            auto font = textStyle.font != nullptr ? textStyle.font : impl_->resolveFallbackTextFont();
            if(font != nullptr){
                auto textRect = spec.textRect.value_or(localBounds);
                textRect = ViewResizeCoordinator::clampRectToParent(textRect,localBounds,layoutClamp);
                auto unicodeText = UniString::fromUTF32(
                    reinterpret_cast<const Unicode32Char *>(spec.text->data()),
                    static_cast<int32_t>(spec.text->size()));
                auto textLayout = textStyle.layout;
                textLayout.lineLimit = textStyle.lineLimit;
                impl_->rootCanvas->drawText(unicodeText,font,textRect,textStyle.color,textLayout);
            }
        }
    }

    impl_->rootCanvas->sendFrame();

    endCompositionSession();
    impl_->layoutDirty = false;
    impl_->styleDirty = false;
    impl_->styleDirtyGlobal = false;
    impl_->styleChangeRequiresCoherentFrame = false;
    impl_->firstFrameCoherentSubmit = false;
    ++impl_->lastUpdateDiagnostics.revision;
}

}
