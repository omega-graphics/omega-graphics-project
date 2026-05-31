#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/UIView.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/UI/Menu.h>
#include <omegaWTK/Widgets/BasicWidgets.h>
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include <omegaWTK/Main.h>
#include <chrono>

// Scene: one blue rounded rect with a nice big drop shadow, centered
// in a white container. The "Animate" menu's "Animate Shadow" item
// tweens the shadow's R channel from 0 → 1 over 1.5s EaseInOut
// (resting black → vivid red → revert on completion via the
// AnimationScheduler's clear-on-complete rule). This is the visible
// validator for the Phase 4.4 AnimationScheduler integration; the
// clamp / red→blue / requestRect machinery that used to live here
// belonged to an older test purpose and is gone (2026-05-31).

namespace {

OmegaWTK::Composition::Rect localBounds(const OmegaWTK::Composition::Rect & rect){
    return OmegaWTK::Composition::Rect{
        OmegaWTK::Composition::Point2D{0.f, 0.f},
        rect.w,
        rect.h
    };
}

OmegaWTK::Composition::LayerEffect::DropShadowParams makeShadow(
        float xOffset, float yOffset, float radius, float blur, float opacity){
    OmegaWTK::Composition::LayerEffect::DropShadowParams params {};
    params.x_offset = xOffset;
    params.y_offset = yOffset;
    params.radius = radius;
    params.blurAmount = blur;
    params.opacity = opacity;
    params.color = OmegaWTK::Composition::Color::create8Bit(
        OmegaWTK::Composition::Color::Black8);
    return params;
}

// Visible from across the room. Big offset, big blur, healthy opacity.
constexpr float kCornerRadius        = 24.f;
constexpr float kShadowOffsetY       = 12.f;
constexpr float kShadowGeometryRadius= 16.f;   // shadow geometry corner
constexpr float kShadowBlur          = 28.f;
constexpr float kShadowOpacity       = 0.65f;

// Menu-triggered tween: black → vivid red.
constexpr float kShadowAnimDurationSec = 1.5f;
constexpr float kShadowAnimFromColorR  = 0.f;
constexpr float kShadowAnimToColorR    = 1.f;

}

class BlueRectWidget final : public OmegaWTK::Widget {
    OmegaWTK::UIViewPtr uiView {};

    // While `shadowAnimActive_` is true the widget self-invalidates each
    // repaint so the per-window FrameBuilder keeps ticking the
    // AnimationScheduler — every frame, Paint re-reads the shadow side-
    // table cell. Without the loop a single repaint would freeze on
    // whatever sampled value the scheduler had at that instant.
    bool shadowAnimActive_ = false;
    std::chrono::steady_clock::time_point shadowAnimDeadline_ {};

    // DirtyBits-gap workaround (Phase 4.4/4.5 era — fixed in Phase F /
    // Phase 4.7). `Widget::invalidate()` only sets THIS widget's Paint
    // bit. The FrameBuilder's composite frame is rebuilt per frame from
    // whichever widgets paint; widgets whose Paint bit is NOT set
    // contribute no draw ops, so their previous visuals (e.g. the
    // parent container's white backdrop) disappear from the framebuffer.
    // For this two-deep tree (root container → blue rect) invalidating
    // the immediate parent is enough — the root container repaints
    // its white backdrop, our child repaints its blue rect, and the
    // animating shadow color lands in the same composite frame. C++'s
    // protected-access rule blocks walking past `this->parent` from a
    // Widget* (we can call `parent->invalidate()` — public — but not
    // read `parent->parent` from a derived class on a base-class
    // pointer), so we only invalidate one level here. Phase F /
    // Phase 4.7 makes this irrelevant.
    void invalidateAncestors(){
        if(parent != nullptr){
            parent->invalidate(OmegaWTK::PaintReason::StateChanged);
        }
    }

    void ensureUIView(const OmegaWTK::Composition::Rect & bounds){
        auto local = localBounds(bounds);
        if(uiView == nullptr){
            uiView = makeSubView<OmegaWTK::UIView>(local, "blue_rect_view");
        }
        else {
            uiView->resize(local);
        }
    }

    void applyLayout(const OmegaWTK::Composition::Rect & bounds){
        OmegaWTK::UIViewLayout layout {};
        layout.shape("blue_rect", OmegaWTK::Shape::RoundedRect(
            OmegaWTK::Composition::RoundedRect{
                OmegaWTK::Composition::Point2D{0.f, 0.f},
                bounds.w,
                bounds.h,
                kCornerRadius,
                kCornerRadius
            }));
        uiView->setLayout(layout);
    }

    void applyStyle(){
        auto style = OmegaWTK::Style::Create();
        style = style->backgroundColor(
            "blue_rect_view",
            OmegaWTK::Composition::Color::Transparent);
        style = style->elementBrush(
            "blue_rect",
            OmegaWTK::Composition::ColorBrush(
                OmegaWTK::Composition::Color::create8Bit(
                    OmegaWTK::Composition::Color::Blue8)),
            false, 0.f);
        style = style->elementDropShadow(
            "blue_rect",
            makeShadow(0.f, kShadowOffsetY,
                       kShadowGeometryRadius, kShadowBlur,
                       kShadowOpacity),
            false, 0.f);
        uiView->setStyle(style);
    }

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onMount() override {
        auto bounds = rect();
        ensureUIView(bounds);
        applyLayout(localBounds(bounds));
        applyStyle();
    }

    void onPaint(OmegaWTK::PaintReason reason) override {
        (void)reason;
        // Refresh layout in case the widget was resized; style is
        // already cached on the UIView. The UIView's own update()
        // path drives the actual paint.
        auto bounds = rect();
        ensureUIView(bounds);
        applyLayout(localBounds(bounds));
        uiView->update();

        // Keep the frame loop alive while the menu-triggered tween is
        // in flight so the scheduler ticks each turn and Paint re-
        // samples the updated shadow-color value. Invalidate every
        // ancestor too — see the comment on `invalidateAncestors`.
        if(shadowAnimActive_){
            if(std::chrono::steady_clock::now() >= shadowAnimDeadline_){
                shadowAnimActive_ = false;
            }
            else {
                invalidateAncestors();
                invalidate(OmegaWTK::PaintReason::StateChanged);
            }
        }
    }

public:
    explicit BlueRectWidget(OmegaWTK::Composition::Rect rect):
            OmegaWTK::Widget(rect){}

    // Called from the menu delegate. Registers a scheduler tween on
    // the rounded rect's drop-shadow R channel: shadow shifts from
    // black to vivid red over 1.5s EaseInOut and back (the scheduler
    // clears the side-table cell on completion → the resolved style's
    // black wins again). Arms the self-invalidate loop + kicks one
    // repaint so the pump starts immediately.
    void triggerShadowAnimation(){
        if(uiView == nullptr){
            return;
        }
        const auto durationSec = kShadowAnimDurationSec;
        uiView->animateElement(
            "blue_rect",
            OmegaWTK::UIView::AnimationChannel::ShadowColorR,
            kShadowAnimFromColorR,
            kShadowAnimToColorR,
            durationSec,
            OmegaWTK::Composition::AnimationCurve::EaseInOut());
        shadowAnimActive_ = true;
        shadowAnimDeadline_ = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(static_cast<long>(durationSec * 1000.f));
        invalidateAncestors();
        invalidate(OmegaWTK::PaintReason::StateChanged);
    }
};

class WhiteRootContainer final : public OmegaWTK::Container {
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onPaint(OmegaWTK::PaintReason reason) override {
        // White backdrop via the hosted UIView (a full-bounds rect
        // element). Phase 3.9 collapse — no CanvasView::clear.
        auto & uv = viewAs<OmegaWTK::UIView>();
        auto r = rect();
        OmegaWTK::UIViewLayout layout {};
        layout.shape("bg", OmegaWTK::Shape::Rect(
            OmegaWTK::Composition::Rect{
                OmegaWTK::Composition::Point2D{0.f, 0.f}, r.w, r.h}));
        uv.setLayout(layout);
        auto style = OmegaWTK::Style::Create();
        style = style->elementBrush(
            "bg",
            OmegaWTK::Composition::ColorBrush(
                OmegaWTK::Composition::Color::create8Bit(
                    OmegaWTK::Composition::Color::White8)),
            false, 0.f);
        uv.setStyle(style);
        uv.update();
        OmegaWTK::Container::onPaint(reason);
    }

public:
    explicit WhiteRootContainer(OmegaWTK::Composition::Rect rect):
            OmegaWTK::Container(OmegaWTK::ViewPtr(
                new OmegaWTK::UIView(rect, nullptr, "root_view"))){}
};

class MyWindowDelegate final : public OmegaWTK::AppWindowDelegate {
public:
    void windowWillClose(OmegaWTK::Native::NativeEventPtr event) override {
        (void)event;
        OmegaWTK::AppInst::terminate();
    }
};

class AnimMenuDelegate final : public OmegaWTK::MenuDelegate {
    SharedHandle<BlueRectWidget> target_;
public:
    explicit AnimMenuDelegate(SharedHandle<BlueRectWidget> target):
        target_(std::move(target)) {}

    void onSelectItem(unsigned itemIndex) override {
        switch(itemIndex){
            case 0:
                if(target_){
                    target_->triggerShadowAnimation();
                }
                break;
            case 2:
                OmegaWTK::AppInst::terminate();
                break;
            default:
                break;
        }
    }
};

int omegaWTKMain(OmegaWTK::AppInst *app) {
    // 500×500 window, white root container, one 260×180 blue rounded
    // rect centered with room around it for the drop shadow to spread.
    constexpr float kWindowW = 500.f;
    constexpr float kWindowH = 500.f;
    constexpr float kRectW   = 260.f;
    constexpr float kRectH   = 180.f;

    auto window = make<OmegaWTK::AppWindow>(
            OmegaWTK::Composition::Rect{{0, 0}, kWindowW, kWindowH},
            new MyWindowDelegate());

    auto container = make<WhiteRootContainer>(
            OmegaWTK::Composition::Rect{{0, 0}, kWindowW, kWindowH});

    auto child = make<BlueRectWidget>(
            OmegaWTK::Composition::Rect{
                {(kWindowW - kRectW) * 0.5f,
                 (kWindowH - kRectH) * 0.5f - 16.f},   // nudge up so the shadow has room
                kRectW, kRectH});
    container->addChild(child);

    window->setRootWidget(container);

    // Menu: "Animate" > "Animate Shadow" (item 0) | sep (1) | "Quit" (2).
    // Delegate is `static` so it outlives this function — the menu
    // captures it by raw pointer.
    static AnimMenuDelegate animMenuDelegate(child);
    auto menu = make<OmegaWTK::Menu>(
        "MainMenu",
        std::initializer_list<SharedHandle<OmegaWTK::MenuItem>>{
            OmegaWTK::CategoricalMenu("Animate", {
                OmegaWTK::ButtonMenuItem("Animate Shadow"),
                OmegaWTK::MenuItemSeperator(),
                OmegaWTK::ButtonMenuItem("Quit")
            }, &animMenuDelegate)
        });
    window->setMenu(menu);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return OmegaWTK::AppInst::start();
}
