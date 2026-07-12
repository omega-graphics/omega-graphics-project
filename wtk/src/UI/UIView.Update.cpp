

#include "UIViewImpl.h"
#include "ViewImpl.h"   // canonical ViewInternal::suspiciousDimensionPair / kMaxViewDimension
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/TextLayoutEngine.h"   // Text-Measurement-API-Plan §3: measureText reuses the CPU-only layout core.
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/LayoutManager.h"   // Phase 4.5: clampRectToParent lifted onto LayoutManager.
#include "FrameBuilder.h"

namespace OmegaWTK {

namespace UIViewInternal {

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
    const float viewWidth = viewRect.w;
    const float viewHeight = viewRect.h;

    // Phase 4.8: the per-view `LayerTree` fallback for resize-transient
    // invalid dimensions is gone (the tree itself is gone — the window
    // owns the single tree now). The View::rect is the only source of
    // local bounds; an invalid rect falls through to the constant
    // fallback below.
    //
    // Validity + the "is this rect garbage" heuristic are the single
    // canonical `ViewInternal::suspiciousDimensionPair` (ViewImpl.h) — it
    // already rejects non-finite / <=0 dims, so it subsumes the old local
    // `isValidDimension`. A previous *duplicate* of this heuristic here
    // still carried an aspect-ratio test that collapsed wide separators to
    // the fallback; the copies are now one to keep them from diverging
    // again.
    if(ViewInternal::suspiciousDimensionPair(viewWidth,viewHeight)){
        return kFallbackRect;
    }

    return Composition::Rect{
            Composition::Point2D{0.f,0.f},
            std::clamp(viewWidth,  1.f, ViewInternal::kMaxViewDimension),
            std::clamp(viewHeight, 1.f, ViewInternal::kMaxViewDimension)
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
    // Phase 4.4 (Anim Tier C): the per-view tween pump is gone. The
    // window's `AnimationScheduler::tick` runs once per outermost frame
    // from `FrameBuilder::beginFrame`; UIView's local Tick phase no
    // longer drives animation state. The method is kept for source
    // compatibility (it is a public surface on `UIView`). Tier D / D4
    // (2026-06-03) deleted the `Impl::advanceAnimations` no-op stub
    // this comment used to pair with.
}

namespace {

// Text-Measurement-API-Plan §5: the `TextLayoutDescriptor::Alignment` enum
// groups in threes — {Left, Middle, Right} × {Upper, Center, Lower} — so the
// vertical band is `index % 3` (0 = Upper, 1 = Center, 2 = Lower) and the
// Upper variant of any alignment is `(index / 3) * 3`.

// Strip vertical alignment to the Upper variant (preserving horizontal) so a
// cached layout is top-origin and rect.h-independent, hence shareable between
// measure (any height) and paint (the real box height).
Composition::TextLayoutDescriptor::Alignment
upperVariant(Composition::TextLayoutDescriptor::Alignment a){
    const int v = static_cast<int>(a);
    return static_cast<Composition::TextLayoutDescriptor::Alignment>((v / 3) * 3);
}

// The vertical offset to add to a top-origin layout so it sits correctly in a
// box of height `rectH` under the element's real alignment. Upper → 0,
// Center → half the slack, Lower → all of it. Negative slack (text taller than
// the box) collapses to top, matching `firstBaselineY`.
float verticalAlignOffset(float rectH, float layoutHeight,
                          Composition::TextLayoutDescriptor::Alignment a){
    const float extra = rectH - layoutHeight;
    if(extra <= 0.f){
        return 0.f;
    }
    switch(static_cast<int>(a) % 3){
        case 1:  return extra * 0.5f;  // Center
        case 2:  return extra;         // Lower
        default: return 0.f;           // Upper
    }
}

} // namespace

void UIView::arrangeContent(){
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

    // Tier 4 (absolute-coords decision 2026-05-29): bake the view's
    // absolute window offset into every emitted rect just before append.
    // Element geometry is authored / clamped in view-local space (localBounds
    // origin is {0,0}); this lifts it into absolute window space so the
    // compositor needs no per-slice viewport translation. Applied LAST,
    // after clampRectToParent (which operates in local space). Path ops are
    // the one geometry kind not yet lifted — see the Path branch below.
    const auto paintOffset = pc.offset;
    auto withOffset = [paintOffset](Composition::Rect r){
        r.pos.x += paintOffset.x;
        r.pos.y += paintOffset.y;
        return r;
    };

    // Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
    // view-level background reads through the unified per-property
    // table (animation → style → UA default). The pre-D5
    // `resolvedViewStyle_` aggregate is gone.
    auto backgroundColor = impl_->resolved<Composition::Color>(
        nodeId(), PropertyKey::BackgroundColor, Composition::Color::Transparent);

    // UIView-Render-Redesign-Plan Tier 2 Phase 2.1: paint is a pure
    // function of model + layout + style + animation. Every would-be-
    // Canvas call below appends to pc.displayList; FrameBuilder replays
    // it into the window canvas at Commit. Background rect is the first op.
    auto rootBgBrush = Composition::ColorBrush(backgroundColor);
    auto rootBgRect = withOffset(localBounds);
    displayList.append(Composition::DrawOp{rootBgRect, rootBgBrush});

    ChildResizeSpec layoutClamp {};
    layoutClamp.resizable = true;
    layoutClamp.policy = ChildResizePolicy::FitContent;

    for(const auto & entry : resolved){
        const auto & spec = *entry.spec;
        // Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
        // per-element reads route through the unified per-property
        // table via `resolved<T>` / `resolvedOptional<T>`. The pre-D5
        // `computedStyleFor(entry.tag)` aggregate read collapsed
        // here — each field below is one cell lookup, scheduler then
        // style-table then fallback / nullopt.
        const auto elementNodeId = impl_->ensureElementNodeId(entry.tag);

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

        if(spec.shape){
            auto shapeToDraw = *spec.shape;
            auto brush = impl_->resolved<SharedHandle<Composition::Brush>>(
                elementNodeId, PropertyKey::FillBrush,
                SharedHandle<Composition::Brush>{nullptr});

            auto dropShadowOpt = impl_->resolvedOptional<Composition::LayerEffect::DropShadowParams>(
                elementNodeId, PropertyKey::DropShadow);
            if(dropShadowOpt){
                auto shadowParams = *dropShadowOpt;
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
                // 4.4 follow-up (2026-05-31): the ShadowColor channels
                // were dead in the Phase 4.4 paint walk (paint never
                // read them — Phase I sweep flagged them for deletion).
                // Reviving them here: each channel overrides one float
                // of `shadowParams.color` in [0,1]. Now reachable from
                // the public `UIView::AnimationChannel` enum.
                if(auto v = impl_->animatedValue(entry.tag,Impl::EffectAnimationKeyShadowColorR); v)
                    shadowParams.color.r = std::clamp(*v, 0.f, 1.f);
                if(auto v = impl_->animatedValue(entry.tag,Impl::EffectAnimationKeyShadowColorG); v)
                    shadowParams.color.g = std::clamp(*v, 0.f, 1.f);
                if(auto v = impl_->animatedValue(entry.tag,Impl::EffectAnimationKeyShadowColorB); v)
                    shadowParams.color.b = std::clamp(*v, 0.f, 1.f);
                if(auto v = impl_->animatedValue(entry.tag,Impl::EffectAnimationKeyShadowColorA); v)
                    shadowParams.color.a = std::clamp(*v, 0.f, 1.f);

                switch(shapeToDraw.type){
                    case Shape::Type::Rect: {
                        auto rect = shapeToDraw.rect;
                        rect = LayoutManager::clampRectToParent(rect,localBounds,layoutClamp);
                        displayList.append(Composition::DrawOp{
                            shadowParams, withOffset(rect), 0.f, false});
                        break;
                    }
                    case Shape::Type::RoundedRect: {
                        auto rr = shapeToDraw.roundedRect;
                        Composition::Rect clampedRect {rr.pos,rr.w,rr.h};
                        clampedRect = LayoutManager::clampRectToParent(clampedRect,localBounds,layoutClamp);
                        rr.pos = clampedRect.pos;
                        rr.w = clampedRect.w;
                        rr.h = clampedRect.h;
                        rr.rad_x = std::min(rr.rad_x,rr.w * 0.5f);
                        rr.rad_y = std::min(rr.rad_y,rr.h * 0.5f);
                        Composition::Rect shapeRect {rr.pos, rr.w, rr.h};
                        const float radius = std::min(rr.rad_x, rr.rad_y);
                        displayList.append(Composition::DrawOp{
                            shadowParams, withOffset(shapeRect), radius, false});
                        break;
                    }
                    case Shape::Type::Ellipse: {
                        const auto & srcEllipse = shapeToDraw.ellipse;
                        Composition::Rect ellipseRect {
                            Composition::Point2D{srcEllipse.x - srcEllipse.rad_x,srcEllipse.y - srcEllipse.rad_y},
                            std::max(1.f,srcEllipse.rad_x * 2.f),
                            std::max(1.f,srcEllipse.rad_y * 2.f)
                        };
                        ellipseRect = LayoutManager::clampRectToParent(ellipseRect,localBounds,layoutClamp);
                        displayList.append(Composition::DrawOp{
                            shadowParams, withOffset(ellipseRect), 0.f, true});
                        break;
                    }
                    default:
                        break;
                }
            }

            // Element-scoped stroke outline (Style::elementBorder →
            // BorderColor/BorderWidth cells written in resolveStyles).
            // Build a Composition::Border once and hand it to whichever
            // shape DrawOp we emit; the backend strokes it. Only color
            // brushes are supported for the stroke today (matches the
            // backend's Brush::Type::Color check). std::nullopt = no
            // outline, the pre-existing behavior.
            Core::Optional<Composition::Border> elementBorder;
            {
                auto borderColor = impl_->resolvedOptional<Composition::Color>(
                    elementNodeId, PropertyKey::BorderColor);
                auto borderWidth = impl_->resolvedOptional<std::uint32_t>(
                    elementNodeId, PropertyKey::BorderWidth);
                if(borderColor && borderWidth && *borderWidth > 0){
                    Core::SharedPtr<Composition::Brush> strokeBrush =
                        Composition::ColorBrush(*borderColor);
                    elementBorder = Composition::Border{
                        strokeBrush,
                        static_cast<unsigned>(*borderWidth)};
                }
            }

            switch(shapeToDraw.type){
                case Shape::Type::Rect: {
                    auto rect = shapeToDraw.rect;
                    rect = LayoutManager::clampRectToParent(rect,localBounds,layoutClamp);
                    displayList.append(Composition::DrawOp{withOffset(rect), brush, elementBorder});
                    break;
                }
                case Shape::Type::RoundedRect: {
                    auto rect = shapeToDraw.roundedRect;
                    Composition::Rect clampedRect {rect.pos,rect.w,rect.h};
                    clampedRect = LayoutManager::clampRectToParent(clampedRect,localBounds,layoutClamp);
                    rect.pos = clampedRect.pos;
                    rect.w = clampedRect.w;
                    rect.h = clampedRect.h;
                    rect.rad_x = std::min(rect.rad_x,rect.w * 0.5f);
                    rect.rad_y = std::min(rect.rad_y,rect.h * 0.5f);
                    rect.pos.x += paintOffset.x;
                    rect.pos.y += paintOffset.y;
                    displayList.append(Composition::DrawOp{rect, brush, elementBorder});
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
                    ellipseRect = LayoutManager::clampRectToParent(ellipseRect,localBounds,layoutClamp);
                    Composition::Ellipse ellipse {
                        ellipseRect.pos.x + (ellipseRect.w * 0.5f) + paintOffset.x,
                        ellipseRect.pos.y + (ellipseRect.h * 0.5f) + paintOffset.y,
                        ellipseRect.w * 0.5f,
                        ellipseRect.h * 0.5f
                    };
                    displayList.append(Composition::DrawOp{ellipse, brush, elementBorder});
                    break;
                }
                case Shape::Type::Path: {
                    if(shapeToDraw.path){
                        // Work on a per-frame deep copy: shapeToDraw.path is a
                        // SharedPtr aliasing the persistent layout spec, so
                        // mutating it in place would (a) revive the §1.3
                        // mutate-the-spec-during-paint smell and (b) — unlike
                        // the idempotent setStroke/close/setPathBrush — make
                        // translate() ACCUMULATE paintOffset every repaint.
                        auto pathCopy = std::make_shared<Composition::Path>(*shapeToDraw.path);
                        pathCopy->setStroke(float(shapeToDraw.pathStrokeWidth));
                        if(shapeToDraw.closePath){
                            pathCopy->close();
                        }
                        pathCopy->setPathBrush(brush);
                        // Absolute-coords (2026-05-29): a path carries its
                        // geometry as points, not a positioned rect, so lift
                        // it into absolute window space by translating every
                        // point — the path counterpart to withOffset() for
                        // rect/ellipse ops.
                        pathCopy->translate(paintOffset);
                        displayList.append(Composition::DrawOp{std::move(pathCopy)});
                    }
                    break;
                }
                default:
                    break;
            }
        }
        else if(spec.text){
            // Text-Measurement-API-Plan §5: paint consumes the SAME shared
            // layout `measureText` produced, so the box reserved by layout and
            // the glyphs drawn here can never disagree about wrapping. The text
            // area is the element's LIVE rect — `spec.textRect` is honored only
            // when explicitly set (a sub-region); the Label leaves it unset so
            // its text fills the resized view (the prior stale-rect freeze was
            // what clipped wrapped text to its construction-time 60dp box).
            auto textRect = spec.textRect.value_or(localBounds);
            textRect = LayoutManager::clampRectToParent(textRect,localBounds,layoutClamp);
            textRect = withOffset(textRect);

            const auto * layout = impl_->ensureTextLayout(entry.tag, textRect.w);
            if(layout != nullptr && !layout->glyphs.empty()){
                auto textColor = impl_->resolved<Composition::Color>(
                    elementNodeId, PropertyKey::TextColor,
                    Composition::Color::create8Bit(Composition::Color::Black8));
                // The shared layout is top-origin; apply the box's vertical
                // alignment (Upper → 0 for the description Label) here.
                auto textLayout = impl_->resolved<Composition::TextLayoutDescriptor>(
                    elementNodeId, PropertyKey::TextLayout,
                    Composition::TextLayoutDescriptor{
                        Composition::TextLayoutDescriptor::LeftUpper,
                        Composition::TextLayoutDescriptor::None
                    });
                const float yOffset = verticalAlignOffset(
                    textRect.h, layout->layoutHeight, textLayout.alignment);

                // Shape upstream of DrawOp emission so paint stays a pure
                // function. Bitmap-fallback sub-runs ride `DrawOp::Bitmap`;
                // MSDF sub-runs ride `DrawOp::TextRun`.
                auto shaped = Composition::shapeTextFromLayout(
                    *layout, textRect, textColor, getRenderScale(), yOffset);
                for(auto & blit : shaped.bitmapBlits){
                    displayList.append(Composition::DrawOp{
                        blit.texture, blit.fence, textRect});
                }
                if(!shaped.msdfSubRuns.empty()){
                    displayList.append(Composition::DrawOp{
                        std::move(shaped.msdfSubRuns), textRect, textColor});
                }
            }
        }
        else if(spec.image && *spec.image != nullptr){
            auto imageRect = spec.imageRect.value_or(localBounds);
            imageRect = LayoutManager::clampRectToParent(imageRect,localBounds,layoutClamp);
            displayList.append(Composition::DrawOp{*spec.image, withOffset(imageRect)});
        }
    }

}

View::PaintBleed UIView::paintBleed(){
    // UIView-Render-Redesign-Plan §G.3.4: union the per-side bleed of every
    // resolved drop shadow this view paints, so the content cache can inflate
    // its capture region and stop scissoring the shadow to the layout rect.
    //
    // A shadow's quad reaches `pad + |offset|` past the shape on each side,
    // where `pad = max(2, blurAmount + 2)` — this MUST mirror the quad
    // padding `emitSdfPrimitive` computes for `kindCode >= 2.5` (shadow), or
    // the shadow re-clips. The shape is always clamped within the view's
    // bounds, so `pad + |offset|` is a sound upper bound on how far the
    // shadow reaches past the LAYOUT rect. We apply that margin symmetrically
    // (ignoring the shape's inset within the view, which would only reduce
    // the bleed) to avoid re-deriving per-element shape rects — over-
    // inflation only adds a transparent texture border that composites as a
    // no-op. Animated-shadow views are cache-ineligible (FrameBuilder skips
    // the marker), so reading the resolved (static) shadow here is sufficient.
    PaintBleed bleed{};
    const auto & resolved = impl_->arranged_;
    for(const auto & entry : resolved){
        const auto & spec = *entry.spec;
        if(!spec.shape){
            continue;   // only shape elements carry a drop shadow (mirrors paint)
        }
        const auto elementNodeId = impl_->ensureElementNodeId(entry.tag);
        auto dropShadowOpt = impl_->resolvedOptional<Composition::LayerEffect::DropShadowParams>(
                elementNodeId, PropertyKey::DropShadow);
        if(!dropShadowOpt){
            continue;
        }
        const auto & s = *dropShadowOpt;
        const float blur = s.blurAmount > 0.f ? s.blurAmount : 0.f;
        const float pad  = std::max(2.f, blur + 2.f);
        const float ax   = s.x_offset < 0.f ? -s.x_offset : s.x_offset;
        const float ay   = s.y_offset < 0.f ? -s.y_offset : s.y_offset;
        const float mx   = pad + ax;
        const float my   = pad + ay;
        bleed.left   = std::max(bleed.left,   mx);
        bleed.right  = std::max(bleed.right,  mx);
        bleed.top    = std::max(bleed.top,    my);
        bleed.bottom = std::max(bleed.bottom, my);
    }
    return bleed;
}

const Composition::LayoutResult *
UIView::Impl::ensureTextLayout(const UIElementTag & tag, float availWidthDp){
    // Text-Measurement-API-Plan §3/§5: the single shared text layout. Both
    // measure (reads `layoutHeight`) and paint (consumes `glyphs`) call this,
    // so the text is laid out once per (content, width) and the two passes can
    // never disagree about wrapping. The CPU-only `TextLayoutEngine::layout`
    // does all the width-driven wrapping + line-limit truncation; no surface
    // or GPU work happens here.
    if(availWidthDp <= 0.f){
        return nullptr;
    }

    // Cache hit: already laid out at exactly this width since the last
    // content/style edit (which clears the cache in resolveStyles()). A width
    // change falls through to recompute and re-cache.
    if(auto it = textLayoutCache_.find(tag);
       it != textLayoutCache_.end() &&
       it->second.availWidthDp == availWidthDp){
        return &it->second.layout;
    }

    // Locate the tagged text element. A Shape/Image element under the same tag,
    // or empty text, has nothing to lay out.
    const UIElementLayoutSpec * textSpec = nullptr;
    for(const auto & spec : currentLayoutV2_.elements()){
        if(spec.tag == tag && spec.text && !spec.text->empty()){
            textSpec = &spec;
            break;
        }
    }
    if(textSpec == nullptr){
        return nullptr;
    }

    // Lay the element's own text out (shared resolution + engine call) and
    // cache it keyed by width. `layoutTaggedText` returns an empty result when
    // the font/shaper is unavailable — treat that as "nothing cached".
    auto layout = layoutTaggedText(tag, *textSpec->text, availWidthDp);
    if(layout.lineBaselines.empty() && layout.glyphs.empty()
       && layout.layoutHeight <= 0.f){
        return nullptr;
    }
    auto & slot = textLayoutCache_[tag];
    slot.availWidthDp = availWidthDp;
    slot.layout = std::move(layout);
    return &slot.layout;
}

Composition::LayoutResult
UIView::Impl::layoutTaggedText(const UIElementTag & tag,
                              const OmegaCommon::UString & text,
                              float availWidthDp){
    Composition::LayoutResult empty{};
    if(availWidthDp <= 0.f || text.empty()){
        return empty;
    }

    // Effective font: resolved `TextFont` cell, else the Arial-18 fallback —
    // byte-for-byte the resolution the paint branch uses.
    auto font = resolveTagFont(tag);
    if(font == nullptr){
        return empty;
    }
    const auto elementNodeId = ensureElementNodeId(tag);

    // Shaper + fallback driver off the process-wide FontEngine. Without a
    // shaper there is no layout to run.
    auto * engine = Composition::FontEngine::inst();
    auto * shaper = (engine != nullptr) ? engine->shaper() : nullptr;
    if(engine == nullptr || shaper == nullptr){
        return empty;
    }

    // Descriptor (horizontal alignment / wrapping) + line limit, resolved as
    // paint does. Vertical alignment is neutralized to Upper so the glyphs are
    // top-origin (paint re-applies the real vertical offset).
    auto textLayout = resolved<Composition::TextLayoutDescriptor>(
        elementNodeId, PropertyKey::TextLayout,
        Composition::TextLayoutDescriptor{
            Composition::TextLayoutDescriptor::LeftUpper,
            Composition::TextLayoutDescriptor::None
        });
    textLayout.lineLimit = resolved<std::uint32_t>(
        elementNodeId, PropertyKey::TextLineLimit, 0u);
    textLayout.alignment = upperVariant(textLayout.alignment);

    auto unicodeText = OmegaCommon::UniString::fromUTF32(
        reinterpret_cast<const OmegaCommon::Unicode32Char *>(text.data()),
        static_cast<int32_t>(text.size()));

    // Only `rect.w` drives wrapping; with Upper alignment `rect.h` no longer
    // affects glyph positions, so a large height is a safe sentinel. Units are
    // dp throughout (`getMetrics()` is nominal-point-size, no render scale),
    // matching the dp-in/dp-out ContentMeasureFn contract.
    const float kLayoutHeight = 1.0e6f;
    const Composition::FontMetrics metrics = font->getMetrics();
    const Composition::Rect layoutRect{
        Composition::Point2D{0.f, 0.f}, availWidthDp, kLayoutHeight};

    return Composition::TextLayoutEngine::layout(
        unicodeText, font, metrics, layoutRect, textLayout, *shaper,
        engine->fallback());
}

SharedHandle<Composition::Font>
UIView::Impl::resolveTagFont(const UIElementTag & tag){
    const auto elementNodeId = ensureElementNodeId(tag);
    auto fontHandle = resolved<SharedHandle<Composition::Font>>(
        elementNodeId, PropertyKey::TextFont,
        SharedHandle<Composition::Font>{nullptr});
    return fontHandle != nullptr ? fontHandle : resolveFallbackTextFont();
}

UIView::TextMeasurement UIView::measureText(const UIElementTag & tag,
                                            float availWidthDp){
    // Text-Measurement-API-Plan §3 + §6: report both extents of the shared
    // layout — the wrapped height and the widest-line width. The layout itself
    // is cached and reused by paint, so this costs a `TextLayoutEngine::layout`
    // only on the first measure per (content, width) — not once per frame, and
    // never a second time in paint. Width is read from the same cached result,
    // so it adds no layout work over measuring height alone.
    const auto * layout = impl_->ensureTextLayout(tag, availWidthDp);
    if(layout == nullptr){
        return TextMeasurement{};
    }
    TextMeasurement measured;
    measured.width  = layout->layoutWidth;
    measured.height = layout->layoutHeight;
    return measured;
}

UIView::TextMeasurement UIView::measureText(const UIElementTag & tag,
                                            const OmegaCommon::UString & text,
                                            float availWidthDp){
    // Substring / arbitrary-text overload: measure `text` (not the element's
    // own text) using the tag's resolved font + descriptor. Unlike the
    // element-text overload this is uncached — it lays out on every call — so
    // a caller measuring a value that changes each edit (e.g. a caret prefix)
    // gets a fresh, correct result instead of the per-(tag,width) memo, which
    // is keyed on width alone and would return the prior text's extent.
    const auto layout = impl_->layoutTaggedText(tag, text, availWidthDp);
    TextMeasurement measured;
    measured.width  = layout.layoutWidth;
    measured.height = layout.layoutHeight;
    return measured;
}

UIView::TextVMetrics UIView::resolvedTextMetrics(const UIElementTag & tag){
    // Vertical font metrics for the tag's effective font. A caller that draws
    // its own vertically-aligned marks (e.g. a TextInput caret) needs these to
    // line up with the glyph ink: the layout engine seats `LeftCenter` text at
    // baseline = ascent + (rectH - lineHeight)/2, so the ink box is
    // [ascent, descent] around that baseline with the line gap trailing below.
    TextVMetrics out;
    auto font = impl_->resolveTagFont(tag);
    if(font == nullptr){
        return out;
    }
    const Composition::FontMetrics m = font->getMetrics();
    out.ascent  = m.ascent;
    out.descent = m.descent;
    out.lineGap = m.lineGap;
    return out;
}

void UIView::update(){
    // Phase 4.7.5: legacy entry point, kept as a thin stub for source
    // compat with the existing primitive overrides (`Rectangle::onPaint`
    // etc.) and a handful of test scenes that call `update()` to force
    // a re-render. The pre-4.7 body (Style → Layout → Paint → Commit
    // orchestrated locally with `ScopedPhase` + `ScopedViewOffset` +
    // `FrameBuilder::submitView`) is gone — `FrameBuilder::buildFrame`
    // owns that orchestration across the whole View tree now. The
    // primitive overrides themselves are dead in 4.7.4+ (the cutover
    // stopped `Widget::executePaint` from dispatching `onPaint`), so
    // this stub never runs in the production paint path; only the
    // explicit test callers reach it, and `markDirty` is the right
    // semantic for them — the test's run-loop frame flush re-paints
    // on the next tick.
    markDirty(View::Style | View::Layout | View::Paint);
}

}
