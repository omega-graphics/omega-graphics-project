#include "UIViewImpl.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/UI/AppWindow.h"
#include "FrameBuilder.h"

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

Composition::Rect localBoundsFromView(UIView *view){
    // UIView-Render-Redesign-Plan Tier 2 Phase 2.2: per-call
    // computation, no cache. The previous static
    // `unordered_map<UIView*, StableBoundsState>` smoothed transient
    // invalid dimensions during resize by returning the last
    // known-good rect; per plan §1.4 + Render-Execution-Efficiency-
    // Plan NV-1..NV-3, the single-surface refactor already removed
    // the resize races that produced those transient invalid
    // dimensions, so the cache no longer earns its lifetime hazard
    // (pointer-keyed, never erased on UIView destruction, not
    // thread-safe).
    constexpr Composition::Rect kFallbackRect{
            Composition::Point2D{0.f,0.f},
            1.f,
            1.f
    };
    if(view == nullptr){
        return kFallbackRect;
    }

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
    if(viewValid){
        width = viewWidth;
        height = viewHeight;
    }
    else if(limbValid){
        width = limbWidth;
        height = limbHeight;
    }
    else {
        return kFallbackRect;
    }

    return Composition::Rect{
            Composition::Point2D{0.f,0.f},
            clampDrawableDimension(width),
            clampDrawableDimension(height)
    };
}

}

// Tier B / B5: assert the active window FrameBuilder is in the expected
// lifecycle phase. No-op when no frame is in flight (a stray update()
// outside a paint pass, or headless tests) — the phase is only defined
// while a frame is being built. Debug-only via assert().
namespace {
void assertActivePhase(FramePhase expected){
    if(auto * fb = AppWindow::activeFrameBuilder(); fb != nullptr){
        fb->assertPhase(expected);
    }
}
}

void UIView::tickAnimations(){
    assertActivePhase(FramePhase::Tick);
    // Tier B / B3: the Tick phase. Advance the per-view tween pump so
    // Paint reads freshly-ticked animation values. Inert today — nothing
    // starts a tween (startOrUpdateAnimation has no caller), so this is a
    // no-op beyond animation diagnostics; Tier D swaps the body for the
    // AnimationScheduler's tick().
    (void)impl_->advanceAnimations();
}

void UIView::arrange(){
    assertActivePhase(FramePhase::Layout);
    // Tier B / B3: the Layout phase. Resolve each element's rect from its
    // layout spec, stable-sort by (zIndex, insertion order), and record
    // the active tag order. Results land in impl_->arranged_ /
    // impl_->arrangedLocalBounds_ for the Paint phase to consume.
    const auto & v2Elements = impl_->currentLayoutV2_.elements();

    impl_->arrangedLocalBounds_ = UIViewInternal::localBoundsFromView(this);
    const float dpiScale = 1.f;
    LayoutContext ctx {};
    ctx.availableRectPx = impl_->arrangedLocalBounds_;
    ctx.dpiScale = dpiScale;
    const auto availDp = ctx.availableRectDp();

    auto & arranged = impl_->arranged_;
    arranged.clear();
    arranged.reserve(v2Elements.size());

    for(std::size_t i = 0; i < v2Elements.size(); ++i){
        const auto & spec = v2Elements[i];
        // Tier B / B1: layout is authored directly on the element spec
        // (`spec.layout`); it no longer flows through the Style sheet.
        const LayoutStyle & effectiveStyle = spec.layout;

        Composition::Rect rectDp = resolveClampedRect(effectiveStyle,availDp,dpiScale);
        Composition::Rect rectPx {
            Composition::Point2D{rectDp.pos.x * dpiScale,rectDp.pos.y * dpiScale},
            rectDp.w * dpiScale,
            rectDp.h * dpiScale
        };

        if(impl_->diagnosticSink_ != nullptr){
            impl_->diagnosticSink_->record(LayoutDiagnosticEntry{
                spec.tag,rectDp,rectPx,LayoutDiagnosticEntry::Pass::Arrange});
        }

        UIViewInternal::ArrangedElement entry {};
        entry.tag = spec.tag;
        entry.spec = &spec;
        entry.resolvedRectDp = rectDp;
        entry.resolvedRectPx = rectPx;
        entry.zIndex = spec.zIndex;
        entry.insertionOrder = i;
        arranged.push_back(entry);
    }

    std::stable_sort(arranged.begin(),arranged.end(),
        [](const UIViewInternal::ArrangedElement & a,const UIViewInternal::ArrangedElement & b){
            if(a.zIndex != b.zIndex){
                return a.zIndex < b.zIndex;
            }
            return a.insertionOrder < b.insertionOrder;
        });

    OmegaCommon::Vector<UIElementTag> nextOrder {};
    nextOrder.reserve(arranged.size());
    for(const auto & r : arranged){
        nextOrder.push_back(r.tag);
    }
    impl_->activeTagOrder = nextOrder;
}

void UIView::paint(Composition::PaintContext & pc){
    assertActivePhase(FramePhase::Paint);
    // Tier B / B3: the Paint phase. A pure function of arranged layout +
    // resolved style (ComputedStyle) + animation values: it appends
    // DrawOps to pc.displayList and mutates no view state. Local aliases
    // keep the existing per-element draw code below unchanged.
    auto & displayList = pc.displayList;
    const auto & localBounds = impl_->arrangedLocalBounds_;
    const auto & resolved = impl_->arranged_;

    const auto & viewStyle = impl_->resolvedViewStyle_;
    auto backgroundColor = viewStyle.backgroundColor.value_or(Composition::Color::Transparent);

    // UIView-Render-Redesign-Plan Tier 2 Phase 2.1: paint is a pure
    // function of model + layout + style + animation. Every would-be-
    // Canvas call below appends to pc.displayList; FrameBuilder replays
    // it into the window canvas at Commit. Background rect is the first op.
    auto rootBgBrush = Composition::ColorBrush(backgroundColor);
    auto rootBgRect = localBounds;
    displayList.append(Composition::DrawOp{rootBgRect, rootBgBrush});

    ChildResizeSpec layoutClamp {};
    layoutClamp.resizable = true;
    layoutClamp.policy = ChildResizePolicy::FitContent;

    for(const auto & entry : resolved){
        const auto & spec = *entry.spec;
        const auto & computed = impl_->computedStyleFor(entry.tag);

        // Tier B / B1: the sheet-authored layout-transition path
        // (lastResolvedV2Rects_ delta → resolveLayoutTransition →
        // applyLayoutDelta) is removed along with layout authoring on
        // Style. Layout transitions will be re-homed onto the layout
        // surface in a later tier; nothing authors them today.

        if(impl_->diagnosticSink_ != nullptr){
            impl_->diagnosticSink_->record(LayoutDiagnosticEntry{
                entry.tag,entry.resolvedRectDp,entry.resolvedRectPx,
                LayoutDiagnosticEntry::Pass::Commit});
        }

        const auto & effectStyle = computed.effects;

        if(spec.shape){
            auto shapeToDraw = *spec.shape;
            auto brush = computed.brush;

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
                        displayList.append(Composition::DrawOp{
                            shadowParams, rect, 0.f, false});
                        break;
                    }
                    case Shape::Type::RoundedRect: {
                        auto rr = shapeToDraw.roundedRect;
                        Composition::Rect clampedRect {rr.pos,rr.w,rr.h};
                        clampedRect = ViewResizeCoordinator::clampRectToParent(clampedRect,localBounds,layoutClamp);
                        rr.pos = clampedRect.pos;
                        rr.w = clampedRect.w;
                        rr.h = clampedRect.h;
                        rr.rad_x = std::min(rr.rad_x,rr.w * 0.5f);
                        rr.rad_y = std::min(rr.rad_y,rr.h * 0.5f);
                        Composition::Rect shapeRect {rr.pos, rr.w, rr.h};
                        const float radius = std::min(rr.rad_x, rr.rad_y);
                        displayList.append(Composition::DrawOp{
                            shadowParams, shapeRect, radius, false});
                        break;
                    }
                    case Shape::Type::Ellipse: {
                        const auto & srcEllipse = shapeToDraw.ellipse;
                        Composition::Rect ellipseRect {
                            Composition::Point2D{srcEllipse.x - srcEllipse.rad_x,srcEllipse.y - srcEllipse.rad_y},
                            std::max(1.f,srcEllipse.rad_x * 2.f),
                            std::max(1.f,srcEllipse.rad_y * 2.f)
                        };
                        ellipseRect = ViewResizeCoordinator::clampRectToParent(ellipseRect,localBounds,layoutClamp);
                        displayList.append(Composition::DrawOp{
                            shadowParams, ellipseRect, 0.f, true});
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
                    displayList.append(Composition::DrawOp{rect, brush});
                    break;
                }
                case Shape::Type::RoundedRect: {
                    auto rect = shapeToDraw.roundedRect;
                    Composition::Rect clampedRect {rect.pos,rect.w,rect.h};
                    clampedRect = ViewResizeCoordinator::clampRectToParent(clampedRect,localBounds,layoutClamp);
                    rect.pos = clampedRect.pos;
                    rect.w = clampedRect.w;
                    rect.h = clampedRect.h;
                    rect.rad_x = std::min(rect.rad_x,rect.w * 0.5f);
                    rect.rad_y = std::min(rect.rad_y,rect.h * 0.5f);
                    displayList.append(Composition::DrawOp{rect, brush});
                    break;
                }
                case Shape::Type::Ellipse: {
                    const auto & srcEllipse = shapeToDraw.ellipse;
                    Composition::Rect ellipseRect {
                        Composition::Point2D{
                            srcEllipse.x - srcEllipse.rad_x,
                            srcEllipse.y - srcEllipse.rad_y
                        },
                        std::max(1.f,srcEllipse.rad_x * 2.f),
                        std::max(1.f,srcEllipse.rad_y * 2.f)
                    };
                    ellipseRect = ViewResizeCoordinator::clampRectToParent(ellipseRect,localBounds,layoutClamp);
                    Composition::Ellipse ellipse {
                        ellipseRect.pos.x + (ellipseRect.w * 0.5f),
                        ellipseRect.pos.y + (ellipseRect.h * 0.5f),
                        ellipseRect.w * 0.5f,
                        ellipseRect.h * 0.5f
                    };
                    displayList.append(Composition::DrawOp{ellipse, brush});
                    break;
                }
                case Shape::Type::Path: {
                    if(shapeToDraw.path){
                        auto & path = *shapeToDraw.path;
                        path.setStroke(shapeToDraw.pathStrokeWidth);
                        if(shapeToDraw.closePath){
                            path.close();
                        }
                        path.setPathBrush(brush);
                        displayList.append(Composition::DrawOp{shapeToDraw.path});
                    }
                    break;
                }
                default:
                    break;
            }
        }
        else if(spec.text){
            // Tier B / B2: text style was resolved (against this
            // element's text-style tag) in resolveStyles() and cached
            // under the element's own tag.
            const auto & textStyle = computed.text;
            auto font = textStyle.font != nullptr ? textStyle.font : impl_->resolveFallbackTextFont();
            if(font != nullptr){
                auto textRect = spec.textRect.value_or(localBounds);
                textRect = ViewResizeCoordinator::clampRectToParent(textRect,localBounds,layoutClamp);
                auto unicodeText = OmegaCommon::UniString::fromUTF32(
                    reinterpret_cast<const OmegaCommon::Unicode32Char *>(spec.text->data()),
                    static_cast<int32_t>(spec.text->size()));
                auto textLayout = textStyle.layout;
                textLayout.lineLimit = textStyle.lineLimit;
                // Shape upstream of DrawOp emission so paint stays a
                // pure function. Bitmap-fallback sub-runs ride
                // `DrawOp::Bitmap`; MSDF sub-runs ride `DrawOp::TextRun`.
                auto shaped = Composition::shapeTextForDisplayList(
                    unicodeText, font, textRect, textStyle.color,
                    textLayout, getRenderScale());
                for(auto & blit : shaped.bitmapBlits){
                    displayList.append(Composition::DrawOp{
                        blit.texture, blit.fence, textRect});
                }
                if(!shaped.msdfSubRuns.empty()){
                    displayList.append(Composition::DrawOp{
                        std::move(shaped.msdfSubRuns), textRect, textStyle.color});
                }
            }
        }
        else if(spec.image && *spec.image != nullptr){
            auto imageRect = spec.imageRect.value_or(localBounds);
            imageRect = ViewResizeCoordinator::clampRectToParent(imageRect,localBounds,layoutClamp);
            displayList.append(Composition::DrawOp{*spec.image, imageRect});
        }
    }

}

void UIView::update(){
    // Tier B / B3: in-place phase model. update() orchestrates the
    // ordered phases on the existing types, flipping the window
    // FrameBuilder's currentPhase_ around each via ScopedPhase. (Tier D
    // hoists this walk into FrameBuilder::buildFrame across the whole
    // tree.) The composition session is owned by Widget::executePaint /
    // AppWindow (FrameBuilder::ScopedFrame), so update() no longer opens
    // its own — the duplicate start/endCompositionSession is gone. An
    // empty layout still renders: paint() appends the default full-bounds
    // background unconditionally.
    auto * fb = AppWindow::activeFrameBuilder();

    { FrameBuilder::ScopedPhase phase(fb, FramePhase::Tick);   tickAnimations(); }
    { FrameBuilder::ScopedPhase phase(fb, FramePhase::Style);  resolveStyles(); }
    { FrameBuilder::ScopedPhase phase(fb, FramePhase::Layout); arrange(); }

    Composition::DisplayList displayList;
    {
        FrameBuilder::ScopedPhase phase(fb, FramePhase::Paint);
        Composition::PaintContext pc { displayList };
        paint(pc);
    }

    // Commit. Tier 3 Phase 3.4: ScopedViewOffset pushes this view's
    // absolute window offset and must outlive submitView, which reads it
    // via View::computeWindowOffset. Tier 3 Phase 3.8: the FrameBuilder
    // bracketing this pass (Widget::executePaint / AppWindow) accumulates
    // the DisplayList in tree order and replays it into the window canvas
    // at endFrame. No active frame (a stray update() outside a paint
    // pass) ⇒ the DisplayList is dropped.
    FrameBuilder::ScopedViewOffset offsetScope(fb, this);
    {
        FrameBuilder::ScopedPhase phase(fb, FramePhase::Commit);
        if(fb != nullptr){
            fb->submitView(this, std::move(displayList));
        }
    }

    impl_->layoutDirty = false;
    impl_->styleDirty = false;
    impl_->styleDirtyGlobal = false;
    impl_->styleChangeRequiresCoherentFrame = false;
    impl_->firstFrameCoherentSubmit = false;
    ++impl_->lastUpdateDiagnostics.revision;
}

}
