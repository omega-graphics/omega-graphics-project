#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/UIView.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/Composition/Canvas.h>
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

    void onMount() override {
        ensureUIView(rect());
    }

    void onPaint(PaintReason reason) override {
        (void)reason;
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

        auto style = StyleSheet::Create();
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

        uiView_->setStyleSheet(style);
        uiView_->update();
    }
};

class MyWindowDelegate : public AppWindowDelegate {
public:
    void windowWillClose(Native::NativeEventPtr event) override {
        (void)event;
        AppInst::terminate();
    }
};

int omegaWTKMain(AppInst *app){
    std::cout << "RootWidgetTest: Phase 2.1 DisplayList full-variant scene" << std::endl;

    const Composition::Rect windowRect{{0,0}, 420, 420};

    auto window = make<AppWindow>(windowRect, new MyWindowDelegate());
    auto widget = make<Phase21Widget>(windowRect);
    window->setRootWidget(widget);

    app->windowManager->setRootWindow(window);
    app->windowManager->displayRootWindow();

    return AppInst::start();
}
