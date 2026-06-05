#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/UIView.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/UI/StyleSheet.h>
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include <omegaWTK/Composition/FontEngine.h>
#include <omegaWTK/Composition/Path.h>
#include <omegaWTK/Main.h>

#include <algorithm>
#include <iostream>

using namespace OmegaWTK;

// UIView-Render-Redesign-Plan Phase 2.1 validator scene. Exercises
// each DrawOp variant that `UIView::update()` can now emit. If any
// of the eight converted branches is broken (Rect, RoundedRect,
// Ellipse, VectorPath, Shadow, TextRun + Bitmap-fallback) the
// matching element goes missing or paints differently from the
// pre-Phase-2.1 baseline. Background rect is also a DrawOp now.

namespace {

Composition::Rect localBounds(const Composition::Rect & r){
    return Composition::Rect{Composition::Point2D{0.f, 0.f}, r.w, r.h};
}

Composition::LayerEffect::DropShadowParams makeShadow(){
    Composition::LayerEffect::DropShadowParams p {};
    p.x_offset = 4.f;
    p.y_offset = 6.f;
    p.radius = 2.f;
    p.blurAmount = 12.f;
    p.opacity = 0.55f;
    p.color = Composition::Color::create8Bit(Composition::Color::Black8);
    return p;
}

}

class Phase21Widget : public Widget {
    UIViewPtr uiView_;
    SharedHandle<Composition::Font> font_;

    void ensureUIView(const Composition::Rect & bounds){
        auto local = localBounds(bounds);
        if(uiView_ == nullptr){
            uiView_ = makeSubView<UIView>(local, "phase21_view");
        }
        else {
            uiView_->resize(local);
        }
    }

    void ensureFont(){
        if(font_ != nullptr) return;
        auto * engine = Composition::FontEngine::inst();
        if(engine == nullptr) return;
        Composition::FontDescriptor desc("Arial", 18);
        font_ = engine->CreateFont(desc);
    }

public:
    explicit Phase21Widget(Composition::Rect rect) : Widget(rect) {}

protected:
    void onThemeSet(Native::ThemeDesc & desc) override {
        (void)desc;
    }

    // Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
    // pre-D8 the entire layout + style authoring lived in `onPaint`.
    // With `Widget::onPaint` retired, the rebuild moved into a helper
    // called from `onMount` (first attach) and `resize` (window
    // dimension change). The central FrameBuilder walker handles
    // every subsequent paint — no per-frame `update()` self-pump is
    // needed.
    void rebuildContent(){
        const auto bounds = localBounds(rect());
        ensureUIView(bounds);
        ensureFont();

        // Five elements laid out in a column. Each exercises a
        // different DrawOp variant the update() refactor now emits.
        const float colX = 60.f;
        const float colW = std::max(1.f, bounds.w - (colX * 2.f));

        // 1) Plain rect — DrawOp::Rect
        Composition::Rect rectEl {Composition::Point2D{colX, 40.f}, colW, 40.f};

        // 2) Rounded rect with drop shadow — DrawOp::Shadow + DrawOp::RoundedRect
        Composition::RoundedRect roundedEl {
            Composition::Point2D{colX, 100.f}, colW, 50.f, 12.f, 12.f};

        // 3) Ellipse — DrawOp::Ellipse
        const float ellY = 180.f;
        const float ellRadX = colW * 0.5f;
        const float ellRadY = 25.f;
        Composition::Ellipse ellipseEl {colX + ellRadX, ellY + ellRadY, ellRadX, ellRadY};

        // 4) Vector path — DrawOp::VectorPath (open polyline)
        Composition::Path pathEl(Composition::Point2D{colX, 240.f}, 4.f);
        pathEl.addLine({colX + colW * 0.5f, 280.f});
        pathEl.addLine({colX + colW,        240.f});

        // 5) Text — DrawOp::TextRun (+ DrawOp::Bitmap if fallback fires)
        Composition::Rect textRect {Composition::Point2D{colX, 320.f}, colW, 40.f};

        UIViewLayout layout {};
        layout.shape("el_rect",        Shape::Rect(rectEl));
        layout.shape("el_rounded",     Shape::RoundedRect(roundedEl));
        layout.shape("el_ellipse",     Shape::Ellipse(ellipseEl));
        layout.shape("el_path",        Shape::Path(std::move(pathEl), 4u, false));
        layout.text ("el_text",        OmegaCommon::UString(U"Phase 2.1 DrawOp"), textRect);
        uiView_->setLayout(layout);

        auto style = Style::Create();
        style = style->backgroundColor("phase21_view",
            Composition::Color::create8Bit(Composition::Color::White8));
        style = style->elementBrush("el_rect",
            Composition::ColorBrush(Composition::Color::create8Bit(Composition::Color::Green8)),
            false, 0.f);
        style = style->elementBrush("el_rounded",
            Composition::ColorBrush(Composition::Color::create8Bit(Composition::Color::Red8)),
            false, 0.f);
        style = style->elementBrush("el_ellipse",
            Composition::ColorBrush(Composition::Color::create8Bit(Composition::Color::Blue8)),
            false, 0.f);
        style = style->elementBrush("el_path",
            Composition::ColorBrush(Composition::Color::create8Bit(Composition::Color::Black8)),
            false, 0.f);
        style = style->elementDropShadow("el_rounded", makeShadow(), false, 0.f);
        style = style->textFont("el_text", font_);
        style = style->textColor("el_text",
            Composition::Color::create8Bit(Composition::Color::Black8));

        uiView_->setStyle(style);
    }

    void onMount() override {
        rebuildContent();
    }

    void resize(Composition::Rect & newRect) override {
        (void)newRect;
        rebuildContent();
    }
};

// UIView-Render-Redesign-Plan Tier 3 Phase 3.2 / 3.4 validator scene.
// Two non-overlapping UIViews on one window, plus a *nested* child
// UIView (Phase 3.4 addition) parked at a non-trivial offset inside
// the left view. Every UIView submits its DisplayList to the
// window-level FrameBuilder; submitView reads the offset from the
// accumulator (pushed by the widget walker for the root widget and
// by UIView::update for each leaf submission). Expected: green
// rectangle filling the left half, red filling the right, and a blue
// rectangle at {40, 80}-relative inside the green half (i.e. absolute
// {40, 80}). Any miss in the accumulator's nested offset composition
// shows up as the inner blue rect landing
// at {0,0} (or worse) instead of inside the green half.
class Phase32Widget : public Widget {
    UIViewPtr leftView_;
    UIViewPtr rightView_;
    UIViewPtr innerView_;

    // Inner rect is expressed in leftView_-local coordinates so that
    // the accumulator's per-step composition is what positions it on
    // screen, not the outer scene re-doing the math.
    static constexpr float kInnerOffsetX = 40.f;
    static constexpr float kInnerOffsetY = 80.f;
    static constexpr float kInnerW       = 120.f;
    static constexpr float kInnerH       = 100.f;

    void ensureViews(const Composition::Rect & bounds){
        const float halfW = bounds.w * 0.5f;
        Composition::Rect leftRect  {Composition::Point2D{0.f,    0.f}, halfW, bounds.h};
        Composition::Rect rightRect {Composition::Point2D{halfW,  0.f}, halfW, bounds.h};
        if(leftView_ == nullptr){
            leftView_ = makeSubView<UIView>(leftRect, "phase32_left");
        }
        else {
            leftView_->resize(leftRect);
        }
        if(rightView_ == nullptr){
            rightView_ = makeSubView<UIView>(rightRect, "phase32_right");
        }
        else {
            rightView_->resize(rightRect);
        }
        // Phase 3.4 nested UIView: child of leftView_ at a non-trivial
        // offset. Constructed directly (not via makeSubView, which
        // hard-codes the widget's root view as parent) so its
        // parent_ptr chain is innerView_ -> leftView_ -> widget.view
        // -> nullptr — three steps for the accumulator to compose.
        Composition::Rect innerRect {
            Composition::Point2D{kInnerOffsetX, kInnerOffsetY},
            kInnerW, kInnerH};
        if(innerView_ == nullptr){
            innerView_ = UIViewPtr(new UIView(innerRect, leftView_, "phase32_inner"));
        }
        else {
            innerView_->resize(innerRect);
        }
    }

public:
    explicit Phase32Widget(Composition::Rect rect) : Widget(rect) {}

    // Widget-View-Paint-Lifecycle-Plan Tier D (2026-06-03):
    // pre-D6 the static empty-layout + per-view backgroundColor
    // setup ran inside `Widget::onPaint(PaintReason)`. After D6 the
    // backgrounds are sheet-driven and the layouts never change, so
    // the entire setup runs ONCE in `onMount()` (and again in
    // `resize()` when the window dimensions change). `Widget::onPaint`
    // is dead in the new model — the framework no longer dispatches
    // it (see `Widget.h` deprecation note) — so an `onPaint` override
    // here would be inert.
    //
    // Each nested UIView ends with `update()` — i.e.
    // `markDirty(Style | Layout | Paint)`. `Widget::init()` marks
    // the widget's OWN view dirty, but `makeSubView<UIView>(...)`
    // creates UIViews as CHILDREN of the widget's view. The Style
    // walker recurses only when `(self.dirty | self.descendant) &
    // Style` is set, so without the per-child markDirty here the
    // walker never visits these UIViews — `resolveStyles` never
    // runs, the sheet cells never land in `styleTable_`, and Paint
    // reads back the UA-default `Color::Transparent` for every
    // background. (Same fix on every other test that holds sub-
    // UIViews via `makeSubView`.)
    void rebuildScene(){
        const auto bounds = localBounds(rect());
        ensureViews(bounds);

        // Empty layouts — every UIView paints only its background via
        // UIView's default layout (no elements => full-bounds fill of
        // the resolved backgroundColor). This is the regression test
        // for that default-layout behavior, and it verifies submissions
        // land in tree order with the right window offsets: any
        // off-by-one in FrameBuilder's offset stamping (Phase 3.2) or
        // in the nested accumulator composition (Phase 3.4) produces a
        // single-color window or a misplaced inner rect instead of the
        // expected three-color layout.
        UIViewLayout emptyLayout {};
        leftView_->setLayout(emptyLayout);
        rightView_->setLayout(emptyLayout);
        innerView_->setLayout(emptyLayout);

        leftView_->update();
        rightView_->update();
        innerView_->update();
    }

protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }

    void onMount() override {
        rebuildScene();
    }

    void resize(Composition::Rect & newRect) override {
        (void)newRect;
        rebuildScene();
        invalidate(PaintReason::Resize);
    }
};

// Widget-View-Paint-Lifecycle-Plan Tier D / D6 (2026-06-03):
// the post-D6 sheet for the Phase 3.2 / 3.4 scene. One rule per UIView,
// each binding the view-scope `BackgroundColor` cell to the legacy
// per-view color (green / red / blue) by selector tag. The sheet is
// installed on the AppWindow in `omegaWTKMain`; the cascade reads it
// fresh every Style phase.
SharedHandle<StyleSheets::StyleSheet> buildScene32Sheet(){
    StyleSheets::StyleSheet::Builder builder;

    StyleSheets::StyleRule leftRule;
    leftRule.selector.tag = "phase32_left";
    leftRule.setBackgroundColor(
        Composition::Color::create8Bit(Composition::Color::Green8));
    builder.addRule(std::move(leftRule));

    StyleSheets::StyleRule rightRule;
    rightRule.selector.tag = "phase32_right";
    rightRule.setBackgroundColor(
        Composition::Color::create8Bit(Composition::Color::Red8));
    builder.addRule(std::move(rightRule));

    StyleSheets::StyleRule innerRule;
    innerRule.selector.tag = "phase32_inner";
    innerRule.setBackgroundColor(
        Composition::Color::create8Bit(Composition::Color::Blue8));
    builder.addRule(std::move(innerRule));

    return builder.build();
}

class MyWindowDelegate : public AppWindowDelegate {
public:
    void windowWillClose(Native::NativeEventPtr event) override {
        (void)event;
    }
};

int omegaWTKMain(AppInst *app){
    // Tier 3 Phase 3.8: window-scoped paint is the only route, so the
    // multi-UIView scene is unconditional.
    std::cout << "RootWidgetTest: Phase 3.8 multi-UIView window-scoped scene"
              << " — Tier D / D6 (2026-06-03): styling now lives on a"
              << " window-installed StyleSheet" << std::endl;

    const Composition::Rect windowRect{{0,0}, 420, 420};

    auto window = make<AppWindow>(windowRect, new MyWindowDelegate());

    // Widget-View-Paint-Lifecycle-Plan Tier D / D6 (2026-06-03):
    // install the cascade sheet BEFORE the widget tree is built.
    // Order doesn't actually matter — the resolver reads the
    // AppWindow's stack fresh on every Style phase — but installing
    // up-front keeps the relationship "this is the styling, this is
    // the geometry" obvious at the call site.
    window->addStyleSheet(buildScene32Sheet());

    auto widget = make<Phase32Widget>(windowRect);
    window->setRootWidget(widget);

    app->windowManager->setRootWindow(window);
    app->windowManager->displayRootWindow();

    return AppInst::start();
}
