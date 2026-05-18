// Tier 3 Phase 3.6 validator: drives a fabricated `DisplayList`
// produced by `ScrollView::paint()` / `ScrollView::paintOverlay()`
// through `DisplayListReplay::replay` into a real `Canvas` and
// verifies (visually) that:
//
//   - The PushClip emitted by `ScrollView::paint()` actually
//     scissors content drawn between paint() and paintOverlay() to
//     the ScrollView's viewport.
//   - The PopClip emitted by `ScrollView::paintOverlay()` restores
//     the prior (unclipped) state so the overlay scroll bars draw
//     in full.
//   - Both vertical and horizontal bars render at correct positions
//     for a scroll offset that places the content mid-scroll
//     (content is 800x800; viewport is 200x200; scroll offset is
//     {150, 150}).
//
// What you should see in a 400x400 window:
//
//   - White background covering the full window.
//   - A 200x200 viewport at (50,50)..(250,250). Inside the viewport
//     a green 800x800 content rect is drawn at local offset
//     {-150,-150} (== `ScrollView::contentOffset()`), so the
//     visible portion is the green region (200,200)..(800,800) of
//     the content, displayed at viewport coordinates
//     (50,50)..(250,250) — i.e. solid green covering the entire
//     viewport.
//   - A thin grey vertical scroll-bar thumb on the right edge of
//     the viewport (200x200 viewport over 800-tall content => 25%
//     thumb at the middle of the track for scroll offset 150 of
//     600 scrollable).
//   - A thin grey horizontal scroll-bar thumb on the bottom edge
//     of the viewport, mirroring the vertical bar.
//   - No green bleed *outside* (50,50)..(250,250) — that would
//     mean PushClip didn't scissor the content draw.
//   - Bars render in full — that means PopClip restored before
//     they emitted.

#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/View.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/UI/ScrollView.h>
#include <omegaWTK/Composition/Canvas.h>
#include <omegaWTK/Composition/Brush.h>
#include <omegaWTK/Composition/DisplayList.h>
#include <omegaWTK/Composition/Layer.h>
#include <omegaWTK/Main.h>
#include <iostream>

using namespace OmegaWTK;

namespace {

constexpr float kViewportSide = 200.f;
constexpr float kViewportOriginX = 50.f;
constexpr float kViewportOriginY = 50.f;
constexpr float kContentSide = 800.f;
constexpr float kScrollX = 150.f;
constexpr float kScrollY = 150.f;

Composition::DisplayList buildValidatorList(const Composition::Rect & windowRect,
                                            ScrollView & scrollView){
    Composition::DisplayList list;

    auto white = Composition::Color::create8Bit(Composition::Color::White8);
    auto green = Composition::Color::create8Bit(Composition::Color::Green8);

    // Background — full window rect (so any green bleed past the
    // PushClip stands out clearly against white instead of getting
    // hidden by uninitialized framebuffer contents).
    Composition::Rect bg{Composition::Point2D{0.f, 0.f},
                         windowRect.w, windowRect.h};
    list.append(Composition::DrawOp{bg, Composition::ColorBrush(white)});

    // ScrollView's pre-children draw — emits PushClip(viewport).
    // The clip rect ScrollView::paint() emits is in ScrollView-local
    // coords. For this fabricated test we translate to window coords
    // manually since we're not going through the FrameBuilder
    // window-offset stamp. We do that by overwriting the op's rect
    // with one positioned at the ScrollView's window origin.
    Composition::DisplayList preList;
    scrollView.paint(preList);
    // Adjust the PushClip rect from local -> window coords.
    auto preOps = preList.ops();
    if(!preOps.empty() && preOps.front().type == Composition::DrawOp::PushClip){
        auto clipRect = preOps.front().params.pushClipParams.rect;
        clipRect.pos.x += kViewportOriginX;
        clipRect.pos.y += kViewportOriginY;
        list.append(Composition::DrawOp::makePushClip(clipRect));
    }

    // The visible content draw — green rect representing the content
    // child. Drawn at (viewport_origin + ScrollView.contentOffset())
    // so the visible portion matches what FrameBuilder's accumulator
    // would yield for descendants of ScrollView at scroll offset
    // {kScrollX, kScrollY}.
    auto contentOffset = scrollView.contentOffset();
    Composition::Rect contentRect{
        Composition::Point2D{kViewportOriginX + contentOffset.x,
                             kViewportOriginY + contentOffset.y},
        kContentSide, kContentSide};
    list.append(Composition::DrawOp{contentRect, Composition::ColorBrush(green)});

    // ScrollView's post-children draw — PopClip + RoundedRect bars.
    // The bar rects ScrollView::paintOverlay() emits are in
    // ScrollView-local coords; same window-offset adjustment.
    Composition::DisplayList postList;
    scrollView.paintOverlay(postList);
    for(auto & op : postList.ops()){
        if(op.type == Composition::DrawOp::PopClip){
            list.append(Composition::DrawOp::makePopClip());
            continue;
        }
        if(op.type == Composition::DrawOp::RoundedRect){
            auto translated = op.params.roundedRectParams.rect;
            translated.pos.x += kViewportOriginX;
            translated.pos.y += kViewportOriginY;
            list.append(Composition::DrawOp{
                translated,
                op.params.roundedRectParams.brush,
                op.params.roundedRectParams.border});
        }
    }

    return list;
}

} // namespace

class ScrollViewClipValidatorWidget final : public Widget {
    SharedHandle<Composition::Canvas> canvas_;
    SharedHandle<ScrollView> scrollView_;
    SharedHandle<View> contentChild_;

protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }

    void onMount() override {
        auto rootLayer = view->getLayerTree()->getRootLayer();
        canvas_ = view->makeCanvas(rootLayer);

        // Build a ScrollView sized to the viewport with a content
        // child larger than the viewport, then scroll it mid-content
        // so both bars sit centered. The ScrollView lives standalone
        // (not in the View tree) — we drive its paint() /
        // paintOverlay() directly from buildValidatorList.
        contentChild_ = View::Create(Composition::Rect{
            Composition::Point2D{0.f, 0.f}, kContentSide, kContentSide});
        scrollView_ = std::shared_ptr<ScrollView>(new ScrollView(
            Composition::Rect{
                Composition::Point2D{kViewportOriginX, kViewportOriginY},
                kViewportSide, kViewportSide},
            contentChild_,
            /*hasVerticalScrollBar=*/true,
            /*hasHorizontalScrollBar=*/true));
        scrollView_->setScrollOffset(Composition::Point2D{kScrollX, kScrollY});
    }

    void onPaint(PaintReason reason) override {
        (void)reason;
        if(canvas_ == nullptr){
            return;
        }

        view->startCompositionSession();

        auto list = buildValidatorList(view->getRect(), *scrollView_);
        Composition::DisplayListReplay::replay(list, *canvas_);
        canvas_->sendFrame();

        view->endCompositionSession();
    }

    bool isLayoutResizable() const override { return false; }

public:
    explicit ScrollViewClipValidatorWidget(Composition::Rect rect)
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
    std::cerr << "[ScrollViewClipTest] window-scoped paint route" << std::endl;
#else
    std::cerr << "[ScrollViewClipTest] per-view canvas route" << std::endl;
#endif
    std::cerr << "[ScrollViewClipTest] expected output:" << std::endl
              << "  - 200x200 green viewport at (50,50)..(250,250)" << std::endl
              << "    (clipped from an 800x800 content rect; visible"
                 " region == viewport)" << std::endl
              << "  - vertical grey scroll-bar thumb on right edge of"
                 " viewport" << std::endl
              << "  - horizontal grey scroll-bar thumb on bottom edge"
                 " of viewport" << std::endl
              << "  - white everywhere else (no green bleed past clip)"
              << std::endl;

    auto window = make<AppWindow>(windowRect, new MyWindowDelegate());

    auto widget = make<ScrollViewClipValidatorWidget>(windowRect);
    window->setRootWidget(widget);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return AppInst::start();
}
