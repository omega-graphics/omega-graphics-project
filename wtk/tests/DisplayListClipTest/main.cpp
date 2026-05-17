// Tier 3 Phase 3.5 validator: drives a fabricated `DisplayList`
// through `DisplayListReplay::replay` into a real `Canvas` and visually
// verifies that `DrawOp::PushClip` actually clips subsequent draws
// while `DrawOp::PopClip` restores the prior (here: unclipped) state.
//
// What you should see in a 400x400 window:
//
//   - White background.
//   - A red rectangle covering the region (50,50)..(150,150) — the
//     intersection of the *unclipped* rect (30,30)..(230,230) with the
//     PushClip rect (50,50)..(150,150). The portions of the red rect
//     outside (50,50)..(150,150) must NOT render. If they do, the
//     clip wiring (DisplayListReplay -> Canvas::pushClip ->
//     backend setScissor) is broken.
//   - A blue rectangle covering (200,200)..(300,300). It must render
//     fully — proving PopClip cleared the scissor before this rect.
//   - No other colored pixels (i.e. no red bleed past the clip,
//     no blue bleed beyond its own rect).
//
// Independent of OMEGAWTK_WINDOW_SCOPED_PAINT: replay drives the
// same Canvas API in both modes, and the backend's SetClip handler
// is the same handler in both routes.

#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/Composition/Canvas.h>
#include <omegaWTK/Composition/Brush.h>
#include <omegaWTK/Composition/DisplayList.h>
#include <omegaWTK/Main.h>
#include <iostream>

using namespace OmegaWTK;

namespace {

Composition::DisplayList buildValidatorList(const Composition::Rect & viewRect){
    Composition::DisplayList list;

    auto white = Composition::Color::create8Bit(Composition::Color::White8);
    auto red   = Composition::Color::create8Bit(Composition::Color::Red8);
    auto blue  = Composition::Color::create8Bit(Composition::Color::Blue8);

    // Background — full view rect.
    Composition::Rect bg{Composition::Point2D{0.f, 0.f}, viewRect.w, viewRect.h};
    list.append(Composition::DrawOp{bg, Composition::ColorBrush(white)});

    // PushClip — 100x100 box at (50, 50)..(150, 150).
    list.append(Composition::DrawOp::makePushClip(
        Composition::Rect{Composition::Point2D{50.f, 50.f}, 100.f, 100.f}));

    // Red rect — extends 20px past the clip on every side. Visible
    // area must equal exactly the clip rect after replay.
    list.append(Composition::DrawOp{
        Composition::Rect{Composition::Point2D{30.f, 30.f}, 200.f, 200.f},
        Composition::ColorBrush(red)});

    // PopClip — scissor returns to slice-natural.
    list.append(Composition::DrawOp::makePopClip());

    // Blue rect, fully inside the view but fully outside the prior
    // clip rect. Must render without truncation.
    list.append(Composition::DrawOp{
        Composition::Rect{Composition::Point2D{200.f, 200.f}, 100.f, 100.f},
        Composition::ColorBrush(blue)});

    return list;
}

} // namespace

class ClipValidatorWidget final : public Widget {
    SharedHandle<Composition::Canvas> canvas_;

protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }

    void onMount() override {
        // Lazy — getLayerTree()->getRootLayer() is the only layer
        // we need to draw onto; one Canvas per Layer (per
        // Canvas-Layer-Exclusivity-Plan).
        auto rootLayer = view->getLayerTree()->getRootLayer();
        canvas_ = view->makeCanvas(rootLayer);
    }

    void onPaint(PaintReason reason) override {
        (void)reason;
        if(canvas_ == nullptr){
            return;
        }

        view->startCompositionSession();

        auto list = buildValidatorList(view->getRect());
        Composition::DisplayListReplay::replay(list, *canvas_);
        canvas_->sendFrame();

        view->endCompositionSession();
    }

    bool isLayoutResizable() const override { return false; }

public:
    explicit ClipValidatorWidget(Composition::Rect rect)
        : Widget(rect) {}
};

class MyWindowDelegate final : public AppWindowDelegate {
public:
    void windowWillClose(Native::NativeEventPtr event) override {
        (void)event;
        AppInst::terminate();
    }
};

int omegaWTKMain(AppInst * app) {
    const Composition::Rect windowRect{{0.f, 0.f}, 400.f, 400.f};

#ifdef OMEGAWTK_WINDOW_SCOPED_PAINT
    std::cerr << "[DisplayListClipTest] window-scoped paint route" << std::endl;
#else
    std::cerr << "[DisplayListClipTest] per-view canvas route" << std::endl;
#endif
    std::cerr << "[DisplayListClipTest] expected output:" << std::endl
              << "  - 100x100 red square at (50,50)..(150,150)" << std::endl
              << "    (clipped from a 200x200 red rect at (30,30)..(230,230))" << std::endl
              << "  - 100x100 blue square at (200,200)..(300,300)" << std::endl
              << "  - white everywhere else" << std::endl;

    auto window = make<AppWindow>(windowRect, new MyWindowDelegate());

    auto widget = make<ClipValidatorWidget>(windowRect);
    window->setRootWidget(widget);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return AppInst::start();
}
