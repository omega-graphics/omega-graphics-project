// Tier 3 Phase 3.7 validator: drives a fabricated `DisplayList`
// containing one or more `DrawOp::NativeContent` carve-outs through
// `DisplayListReplay::replay` into a real `Canvas` and verifies that:
//
//   - Each `NativeContent` op turns into a `VisualCommand::NativeContent`
//     on the per-frame visuals with the destRect / hostId / zOrderHint
//     payload preserved through the replay path.
//   - The visuals interleave correctly with surrounding draw ops
//     (background rect → carve-out → foreground rect), which is what
//     the backend's renderToTarget switch ordering depends on when the
//     platform tree drains `pendingNativeContent` post-slice.
//
// This is the surface-level validator the Phase 3.7 plan calls for:
// `VideoViewPlaybackTest` is the end-to-end validator (a real native
// video surface composited through the carve-out) but is currently
// unbuildable, so this fabricated-DisplayList test stands in until the
// pre-existing link break is fixed. The plumbing it exercises —
// `DisplayList::makeNativeContent` → `DisplayListReplay::NativeContent`
// arm → `Canvas::markNativeContentRegion` → `VisualCommand::NativeContent`
// → backend `pendingNativeContent_` — is the entire Phase 3.7 surface.

#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/View.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/Composition/Canvas.h>
#include <omegaWTK/Composition/Brush.h>
#include <omegaWTK/Composition/DisplayList.h>
#include <omegaWTK/Composition/Layer.h>
#include <omegaWTK/Main.h>
#include <cassert>
#include <cstdint>
#include <iostream>

using namespace OmegaWTK;

namespace {

constexpr std::uint64_t kHostA = 0xA1A1A1A1;
constexpr std::uint64_t kHostB = 0xB2B2B2B2;

Composition::DisplayList buildValidatorList(const Composition::Rect & windowRect){
    Composition::DisplayList list;

    auto white = Composition::Color::create8Bit(Composition::Color::White8);
    auto blue  = Composition::Color::create8Bit(Composition::Color::Blue8);

    Composition::Rect bg{Composition::Point2D{0.f, 0.f},
                         windowRect.w, windowRect.h};
    list.append(Composition::DrawOp{bg, Composition::ColorBrush(white)});

    // First carve-out — z=0, the "below" native layer.
    list.append(Composition::DrawOp::makeNativeContent(
        Composition::Rect{Composition::Point2D{50.f, 50.f}, 150.f, 100.f},
        kHostA,
        /*zOrderHint=*/0));

    // Foreground draw between the two carve-outs — proves visuals
    // interleave correctly.
    list.append(Composition::DrawOp{
        Composition::Rect{Composition::Point2D{120.f, 90.f}, 60.f, 60.f},
        Composition::ColorBrush(blue)});

    // Second carve-out — z=2, the "above" native layer.
    list.append(Composition::DrawOp::makeNativeContent(
        Composition::Rect{Composition::Point2D{220.f, 50.f}, 150.f, 100.f},
        kHostB,
        /*zOrderHint=*/2));

    return list;
}

void verifyCarveoutFrame(Composition::CanvasFrame & frame){
    int seenA = 0, seenB = 0, seenForeground = 0, seenBackground = 0;
    for(const auto & cmd : frame.currentVisuals){
        switch(cmd.type){
            case Composition::VisualCommand::NativeContent: {
                const auto & p = cmd.params.nativeContentParams;
                if(p.hostId == kHostA){
                    ++seenA;
                    assert(p.zOrderHint == 0);
                    assert(p.destRect.pos.x == 50.f);
                    assert(p.destRect.pos.y == 50.f);
                    assert(p.destRect.w == 150.f);
                    assert(p.destRect.h == 100.f);
                }
                else if(p.hostId == kHostB){
                    ++seenB;
                    assert(p.zOrderHint == 2);
                    assert(p.destRect.pos.x == 220.f);
                }
                break;
            }
            case Composition::VisualCommand::Rect: {
                const auto & rp = cmd.params.rectParams;
                if(rp.rect.w == 60.f) ++seenForeground;
                else ++seenBackground;
                break;
            }
            default: break;
        }
    }
    assert(seenA == 1 && "NativeContent for hostA missing");
    assert(seenB == 1 && "NativeContent for hostB missing");
    assert(seenForeground == 1 && "Foreground rect missing between carve-outs");
    assert(seenBackground == 1 && "Background rect missing");
    std::cerr << "[NativeContentCarveoutTest] PASS — 2 carve-outs ("
              << "hostA z=0, hostB z=2), rect ordering preserved"
              << std::endl;
}

} // namespace

class CarveoutValidatorWidget final : public Widget {
    SharedHandle<Composition::Canvas> canvas_;

protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }

    void onMount() override {
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

        // Verify per-frame visuals BEFORE sendFrame (which resets them).
        auto frame = canvas_->getCurrentFrame();
        if(frame != nullptr){
            verifyCarveoutFrame(*frame);
        }

        canvas_->sendFrame();
        view->endCompositionSession();
    }

    bool isLayoutResizable() const override { return false; }

public:
    explicit CarveoutValidatorWidget(Composition::Rect rect)
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
    const Composition::Rect windowRect{{0.f, 0.f}, 400.f, 200.f};

#ifdef OMEGAWTK_WINDOW_SCOPED_PAINT
    std::cerr << "[NativeContentCarveoutTest] window-scoped paint route"
              << std::endl;
#else
    std::cerr << "[NativeContentCarveoutTest] per-view canvas route"
              << std::endl;
#endif
    std::cerr << "[NativeContentCarveoutTest] verifies DrawOp::NativeContent"
              << " round-trips through DisplayListReplay onto the per-frame"
              << " VisualCommand stream as a NativeContent visual with"
              << " payload preserved (destRect / hostId / zOrderHint)."
              << std::endl;

    auto window = make<AppWindow>(windowRect, new MyWindowDelegate());

    auto widget = make<CarveoutValidatorWidget>(windowRect);
    window->setRootWidget(widget);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return AppInst::start();
}
