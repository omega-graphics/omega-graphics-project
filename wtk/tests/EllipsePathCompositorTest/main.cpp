#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/UIView.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/UI/StyleSheet.h>
#include <omegaWTK/Widgets/Containers.h>
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include <omegaWTK/Main.h>
#include <algorithm>
#include <iostream>
#include <memory>

namespace {
OmegaWTK::Composition::Rect localViewBounds(const OmegaWTK::Composition::Rect & bounds){
    return OmegaWTK::Composition::Rect{
        OmegaWTK::Composition::Point2D{0.f,0.f},
        bounds.w,
        bounds.h
    };
}

OmegaWTK::Composition::LayerEffect::DropShadowParams makeShadow(float x,float y,float radius,float blur,float opacity){
    OmegaWTK::Composition::LayerEffect::DropShadowParams params {};
    params.x_offset = x;
    params.y_offset = y;
    params.radius = radius;
    params.blurAmount = blur;
    params.opacity = opacity;
    params.color = OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Black8);
    return params;
}
}

class RoundedFrameWidget final : public OmegaWTK::Widget {
    OmegaWTK::UIViewPtr uiView {};
    bool loggedLayout = false;

    void ensureUIView(const OmegaWTK::Composition::Rect & bounds){
        auto localBounds = localViewBounds(bounds);
        if(uiView == nullptr){
            uiView = makeSubView<OmegaWTK::UIView>(localBounds,"rounded_frame_view");
        }
        else {
            uiView->resize(localBounds);
        }
    }
    // Widget-View-Paint-Lifecycle-Plan Tier D (2026-06-03):
    // geometry-only refresh. Brush + drop-shadow are now resolved
    // from the window-installed StyleSheet (`buildScenesSheet`); the
    // pre-D6 inline `Style::Create()->elementBrush/elementDropShadow`
    // block is gone. Runs once at mount and again on resize —
    // `Widget::onPaint` is deprecated (never dispatched after the
    // 4.7.4 cutover) so geometry can't piggy-back on it any more.
    void rebuildGeometry(){
        auto bounds = localViewBounds(rect());
        ensureUIView(bounds);

        const float outerSize = std::min(bounds.w,bounds.h) * 0.70f;
        const float outerRadius = 14.0f;

        OmegaWTK::Composition::RoundedRect outer{
            OmegaWTK::Composition::Point2D{
                (bounds.w - outerSize) * 0.5f,
                (bounds.h - outerSize) * 0.5f},
            outerSize,
            outerSize,
            outerRadius,
            outerRadius};

        OmegaWTK::UIViewLayout layout {};
        layout.shape("rounded_outer",OmegaWTK::Shape::RoundedRect(outer));
        uiView->setLayout(layout);

        // Widget-View-Paint-Lifecycle-Plan Tier D (2026-06-03):
        // mark the sub-UIView Style|Layout|Paint-dirty so the
        // FrameBuilder's pre-order walk actually visits it next
        // frame. Without this the sheet cells never get written —
        // the walker skips subviews whose parent has clean Style
        // descendant masks, and `Widget::init()` only marks the
        // widget's OWN view (a base View, not this UIView).
        uiView->update();

        if(!loggedLayout){
            std::cout << "[EllipsePathCompositorTest] RoundedFrameWidget rendered via UIView." << std::endl;
            loggedLayout = true;
        }
    }
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onMount() override {
        rebuildGeometry();
    }

    void resize(OmegaWTK::Composition::Rect & newRect) override {
        (void)newRect;
        rebuildGeometry();
        invalidate(OmegaWTK::PaintReason::Resize);
    }

    bool isLayoutResizable() const override {
        return false;
    }

public:
    explicit RoundedFrameWidget(OmegaWTK::Composition::Rect rect):
        OmegaWTK::Widget(rect){}
};

class EllipseOnlyWidget final : public OmegaWTK::Widget {
    OmegaWTK::UIViewPtr uiView {};
    bool loggedLayout = false;

    void ensureUIView(const OmegaWTK::Composition::Rect & bounds){
        auto localBounds = localViewBounds(bounds);
        if(uiView == nullptr){
            uiView = makeSubView<OmegaWTK::UIView>(localBounds,"ellipse_view");
        }
        else {
            uiView->resize(localBounds);
        }
    }
    // Widget-View-Paint-Lifecycle-Plan Tier D (2026-06-03):
    // see RoundedFrameWidget — same sheet-driven, geometry-only
    // shape; `Widget::onPaint` is deprecated.
    void rebuildGeometry(){
        auto bounds = localViewBounds(rect());
        ensureUIView(bounds);
        OmegaWTK::Composition::Ellipse ellipse{
            bounds.w * 0.5f,
            bounds.h * 0.5f,
            bounds.w * 0.30f,
            bounds.h * 0.22f};

        OmegaWTK::UIViewLayout layout {};
        layout.shape("ellipse_shape",OmegaWTK::Shape::Ellipse(ellipse));
        uiView->setLayout(layout);
        uiView->update();   // see RoundedFrameWidget's rebuildGeometry comment

        if(!loggedLayout){
            std::cout << "[EllipsePathCompositorTest] EllipseOnlyWidget rendered via UIView." << std::endl;
            loggedLayout = true;
        }
    }
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onMount() override {
        rebuildGeometry();
    }

    void resize(OmegaWTK::Composition::Rect & newRect) override {
        (void)newRect;
        rebuildGeometry();
        invalidate(OmegaWTK::PaintReason::Resize);
    }

    bool isLayoutResizable() const override {
        return false;
    }

public:
    explicit EllipseOnlyWidget(OmegaWTK::Composition::Rect rect):
        OmegaWTK::Widget(rect){}
};

class PathOnlyWidget final : public OmegaWTK::Widget {
    OmegaWTK::UIViewPtr uiView {};
    bool loggedLayout = false;

    void ensureUIView(const OmegaWTK::Composition::Rect & bounds){
        auto localBounds = localViewBounds(bounds);
        if(uiView == nullptr){
            uiView = makeSubView<OmegaWTK::UIView>(localBounds,"path_view");
        }
        else {
            uiView->resize(localBounds);
        }
    }
    // Widget-View-Paint-Lifecycle-Plan Tier D (2026-06-03):
    // see RoundedFrameWidget — same sheet-driven, geometry-only
    // shape; `Widget::onPaint` is deprecated.
    void rebuildGeometry(){
        auto bounds = localViewBounds(rect());
        ensureUIView(bounds);

        const float x0 = bounds.w * 0.12f;
        const float x1 = bounds.w * 0.38f;
        const float x2 = bounds.w * 0.62f;
        const float x3 = bounds.w * 0.88f;
        const float yHigh = bounds.h * 0.36f;
        const float yLow = bounds.h * 0.64f;

        OmegaWTK::Composition::Path compPath(OmegaWTK::Composition::Point2D{x0,yLow});
        compPath.addLine(OmegaWTK::Composition::Point2D{x1,yHigh});
        compPath.addLine(OmegaWTK::Composition::Point2D{x2,yLow});
        compPath.addLine(OmegaWTK::Composition::Point2D{x3,yHigh});

        OmegaWTK::UIViewLayout layout {};
        layout.shape("path_shape",OmegaWTK::Shape::Path(std::move(compPath),6));
        uiView->setLayout(layout);
        uiView->update();   // see RoundedFrameWidget's rebuildGeometry comment

        if(!loggedLayout){
            std::cout << "[EllipsePathCompositorTest] PathOnlyWidget rendered via UIView." << std::endl;
            loggedLayout = true;
        }
    }
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onMount() override {
        rebuildGeometry();
    }

    void resize(OmegaWTK::Composition::Rect & newRect) override {
        (void)newRect;
        rebuildGeometry();
        invalidate(OmegaWTK::PaintReason::Resize);
    }

    bool isLayoutResizable() const override {
        return false;
    }

public:
    explicit PathOnlyWidget(OmegaWTK::Composition::Rect rect):
        OmegaWTK::Widget(rect){}
};

class GeometryHStack final : public OmegaWTK::HStack {
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    // Widget-View-Paint-Lifecycle-Plan Tier D (2026-06-03):
    // the "hstack_bg" element's brush is sheet-driven; the geometry
    // is just a full-bounds rect that needs to track the widget's
    // size. `Widget::onPaint` is deprecated (never dispatched by the
    // framework after the 4.7.4 cutover), so the backdrop rebuild
    // hooks `onMount` + `resize` instead of `onPaint`.
    void rebuildBackdrop(){
        auto & uv = viewAs<OmegaWTK::UIView>();
        auto r = rect();
        OmegaWTK::UIViewLayout layout {};
        layout.shape("hstack_bg",OmegaWTK::Shape::Rect(
            OmegaWTK::Composition::Rect{OmegaWTK::Composition::Point2D{0.f,0.f},r.w,r.h}));
        uv.setLayout(layout);
    }

    void onMount() override {
        OmegaWTK::HStack::onMount();
        rebuildBackdrop();
    }

    void resize(OmegaWTK::Composition::Rect & newRect) override {
        OmegaWTK::HStack::resize(newRect);
        rebuildBackdrop();
    }

public:
    explicit GeometryHStack(OmegaWTK::Composition::Rect rect,
                            const OmegaWTK::StackOptions & options):
        OmegaWTK::HStack(OmegaWTK::ViewPtr(
            new OmegaWTK::UIView(rect,nullptr,"geometry_hstack_view")),options){}
};

class MyWindowDelegate final : public OmegaWTK::AppWindowDelegate {
public:
    void windowWillClose(OmegaWTK::Native::NativeEventPtr event) override {
        (void)event;
    }
};

// Widget-View-Paint-Lifecycle-Plan Tier D / D6 (2026-06-03):
// the cascade sheet for the geometry-showcase scene. One rule per
// (UIView background OR shape element). Selector tag matches either
// the UIView's tag (BackgroundColor → view nodeId) or the element's
// tag (FillBrush / DropShadow → element nodeId); the `scopeOf` helper
// in `StyleResolver` routes each declaration to the correct NodeId
// automatically. The legacy `transition=true, dur=0.28` per-rule
// kwargs the inline `Style::elementBrush(..., true, 0.28f)` carried
// are dropped — the source/destination values never change in this
// scene, so the legacy transition never fired. D7's sheet-driven
// transitions (via the D6.5 recording surface) supersede that
// per-property knob.
SharedHandle<OmegaWTK::StyleSheets::StyleSheet> buildScenesSheet(){
    OmegaWTK::StyleSheets::StyleSheet::Builder builder;

    using OmegaWTK::Composition::Color;
    using OmegaWTK::Composition::ColorBrush;

    // ----- View-level transparent backdrops -----
    for(const auto * viewTag : {"rounded_frame_view",
                                "ellipse_view",
                                "path_view"}){
        OmegaWTK::StyleSheets::StyleRule rule;
        rule.selector.tag = viewTag;
        rule.setBackgroundColor(Color::Transparent);
        builder.addRule(std::move(rule));
    }

    // ----- Shape element brushes + drop shadows -----
    struct ShapeRule {
        const char *                                              tag;
        Color::Eight                                              color;
        OmegaWTK::Composition::LayerEffect::DropShadowParams      shadow;
    };
    const ShapeRule shapes[] = {
        {"rounded_outer", Color::Red8,    makeShadow(0.f,4.f,2.f,8.f,0.55f)},
        {"ellipse_shape", Color::Green8,  makeShadow(0.f,5.f,2.f,9.f,0.55f)},
        {"path_shape",    Color::Yellow8, makeShadow(0.f,5.f,2.f,8.f,0.50f)},
    };
    for(const auto & s : shapes){
        OmegaWTK::StyleSheets::StyleRule rule;
        rule.selector.tag = s.tag;
        rule.setFillBrush(ColorBrush(Color::create8Bit(s.color)));
        rule.setDropShadow(s.shadow);
        builder.addRule(std::move(rule));
    }

    // ----- HStack white backdrop -----
    OmegaWTK::StyleSheets::StyleRule bgRule;
    bgRule.selector.tag = "hstack_bg";
    bgRule.setFillBrush(ColorBrush(Color::create8Bit(Color::White8)));
    builder.addRule(std::move(bgRule));

    return builder.build();
}

int omegaWTKMain(OmegaWTK::AppInst *app) {
    const OmegaWTK::Composition::Rect windowRect{{0,0},500,500};

    auto window = make<OmegaWTK::AppWindow>(
        windowRect,
        new MyWindowDelegate());

    // Widget-View-Paint-Lifecycle-Plan Tier D / D6 (2026-06-03):
    // install the cascade sheet on the window. Each widget's
    // `onPaint` now does geometry only; the cascade fills the
    // resolved-style cells from the sheet at Style phase.
    window->addStyleSheet(buildScenesSheet());

    OmegaWTK::StackOptions options {};
    options.spacing = 18.0f;
    options.padding = {20.0f,20.0f,20.0f,20.0f};
    options.mainAlign = OmegaWTK::StackMainAlign::Center;
    options.crossAlign = OmegaWTK::StackCrossAlign::Center;

    auto stack = make<GeometryHStack>(
        windowRect,
        options);

    const OmegaWTK::Composition::Rect childRect{{0,0},130,220};

    auto pathWidget = make<PathOnlyWidget>(childRect);
    auto roundedFrameWidget = make<RoundedFrameWidget>(childRect);
    auto ellipseWidget = make<EllipseOnlyWidget>(childRect);

    OmegaWTK::StackSlot slot {};
    slot.flexGrow = 0.0f;
    slot.flexShrink = 0.0f;
    slot.margin = {0.0f,0.0f,0.0f,0.0f};
    slot.alignSelf = OmegaWTK::StackCrossAlign::Center;

    stack->addChild(pathWidget,slot);
    stack->addChild(roundedFrameWidget,slot);
    stack->addChild(ellipseWidget,slot);

    window->setRootWidget(stack);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return OmegaWTK::AppInst::start();
}
