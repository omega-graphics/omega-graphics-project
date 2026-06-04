#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/UIView.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/UI/Menu.h>
#include <omegaWTK/UI/StyleSheet.h>
#include <omegaWTK/Widgets/BasicWidgets.h>
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include <omegaWTK/Main.h>

// Scene: one blue rounded rect with a big drop shadow, centered in a
// white container. The "Animate" menu's "Animate Shadow" item swaps
// a "highlight" StyleSheet onto the window — the highlight sheet's
// `blue_rect` rule declares a different DropShadow value PLUS a
// `TransitionSpec` for the DropShadow property. The StyleResolver
// detects the cell change between frames and fires
// `scheduler.transition<DropShadowParams>` via the D7.2 friend hook;
// the scheduler tweens DropShadowParams (lerping every channel)
// over the spec's timing.
//
// Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
// pre-D7.2 this test triggered the animation via
// `uiView->animateElement(ShadowColorR, ...)` — a direct
// scheduler.tweenProperty<float> call against a UserDefined key. That
// validated the scheduler but did NOT exercise the D7.2 cascade →
// transition path. The sheet-swap version below is the canonical
// D7.2 verification.

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

// Menu-triggered transition duration. The TransitionSpec on both
// sheet rules uses this; the EaseInOut curve completes the visible
// effect.
constexpr float kShadowAnimDurationSec = 1.5f;

}

class BlueRectWidget final : public OmegaWTK::Widget {
    OmegaWTK::UIViewPtr uiView {};

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

    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
    // styling is sheet-driven (see `buildClampInitialSheet` /
    // `buildClampHighlightSheet` below). The pre-D7.2 self-pump via
    // `Widget::onPaint(PaintReason)` is GONE — the
    // FrameBuilder-side auto-pump (`AnimationScheduler::stats()` >
    // 0 → invalidate root + requestFrame) now keeps the frame loop
    // alive while any animation is active. The pre-D7.2 hover-class
    // `invalidateAncestors()` helper is also gone — the auto-pump
    // marks the tree-host root Paint-dirty each tick.

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onMount() override {
        auto bounds = rect();
        ensureUIView(bounds);
        applyLayout(localBounds(bounds));
        // Widget-View-Paint-Lifecycle-Plan Tier D (2026-06-03):
        // mark the sub-UIView Style|Layout|Paint-dirty. `uiView` was
        // created via `makeSubView<UIView>(...)` so it is a CHILD of
        // this Widget's view, and `Widget::init()` only marks the
        // widget's OWN view dirty. Without this call the Style
        // walker never visits the child, the sheet cells never get
        // written to `styleTable_`, and Paint reads back the UA
        // defaults — visible as an invisible scene.
        uiView->update();
    }

    // Widget-View-Paint-Lifecycle-Plan Tier D (2026-06-03):
    // resize handles the geometry rebuild that pre-D6 lived inside
    // onPaint. `Widget::onPaint` is deprecated; this is the right
    // hook for layout-on-resize.
    void resize(OmegaWTK::Composition::Rect & newRect) override {
        (void)newRect;
        auto bounds = rect();
        ensureUIView(bounds);
        applyLayout(localBounds(bounds));
        uiView->update();   // see onMount comment
        invalidate(OmegaWTK::PaintReason::Resize);
    }

public:
    explicit BlueRectWidget(OmegaWTK::Composition::Rect rect):
            OmegaWTK::Widget(rect){}
};

class WhiteRootContainer final : public OmegaWTK::Container {
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    // Widget-View-Paint-Lifecycle-Plan Tier D (2026-06-03):
    // backdrop brush is sheet-driven (`buildClampAnimSheet`); only
    // the geometry (a full-bounds rect element) lives here. Runs
    // on mount + resize — `Widget::onPaint` is deprecated.
    void rebuildBackdrop(){
        auto & uv = viewAs<OmegaWTK::UIView>();
        auto r = rect();
        OmegaWTK::UIViewLayout layout {};
        layout.shape("bg", OmegaWTK::Shape::Rect(
            OmegaWTK::Composition::Rect{
                OmegaWTK::Composition::Point2D{0.f, 0.f}, r.w, r.h}));
        uv.setLayout(layout);
    }

    void onMount() override {
        OmegaWTK::Container::onMount();
        rebuildBackdrop();
    }

    void resize(OmegaWTK::Composition::Rect & newRect) override {
        OmegaWTK::Container::resize(newRect);
        rebuildBackdrop();
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

// Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
// the initial sheet — black drop shadow on the blue rect. Lives at
// the bottom of the AppWindow stack for the whole session; menu
// click pushes / pops the `highlight` sheet ABOVE this one to
// trigger the cascade change.
SharedHandle<OmegaWTK::StyleSheets::StyleSheet> buildClampInitialSheet(){
    OmegaWTK::StyleSheets::StyleSheet::Builder builder;

    // White root-container backdrop.
    OmegaWTK::StyleSheets::StyleRule bgRule;
    bgRule.selector.tag = "bg";
    bgRule.setFillBrush(OmegaWTK::Composition::ColorBrush(
        OmegaWTK::Composition::Color::create8Bit(
            OmegaWTK::Composition::Color::White8)));
    builder.addRule(std::move(bgRule));

    // Blue rect view: transparent backdrop.
    OmegaWTK::StyleSheets::StyleRule rectViewRule;
    rectViewRule.selector.tag = "blue_rect_view";
    rectViewRule.setBackgroundColor(OmegaWTK::Composition::Color::Transparent);
    builder.addRule(std::move(rectViewRule));

    // Blue rect element: blue fill + black drop shadow (initial state).
    // Carries a `TransitionSpec` for `DropShadow` so swapping to the
    // highlight sheet's red-shadow value fires
    // `scheduler.transition<DropShadowParams>` via the D7.2 friend
    // hook. The spec stays on the initial rule too so the reverse
    // direction (highlight → initial) ALSO transitions.
    OmegaWTK::StyleSheets::StyleRule rectRule;
    rectRule.selector.tag = "blue_rect";
    rectRule.setFillBrush(OmegaWTK::Composition::ColorBrush(
        OmegaWTK::Composition::Color::create8Bit(
            OmegaWTK::Composition::Color::Blue8)));
    rectRule.setDropShadow(makeShadow(0.f, kShadowOffsetY,
                                      kShadowGeometryRadius,
                                      kShadowBlur, kShadowOpacity));
    OmegaWTK::StyleSheets::TransitionSpec shadowTransition;
    shadowTransition.key = OmegaWTK::PropertyKey::DropShadow;
    shadowTransition.timing.durationMs =
        static_cast<std::uint32_t>(kShadowAnimDurationSec * 1000.f);
    shadowTransition.curve = OmegaWTK::Composition::AnimationCurve::EaseInOut();
    rectRule.transitions.push_back(shadowTransition);
    builder.addRule(std::move(rectRule));

    return builder.build();
}

// Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
// the `highlight` overlay. Same selector (`blue_rect`), different
// shadow color (red). Pushing this on top of the initial sheet via
// `AppWindow::addStyleSheet` makes its rule win the cascade tie
// through StyleRule::beats's source-order `>=` tiebreak — the
// StyleResolver writes the red-shadow cell to styleTable_, the
// previous frame's black-shadow cell sits in previousStyleTable_,
// and `StyleResolver::applyTransitions` fires the registered
// scheduler.transition<DropShadowParams> for the change.
SharedHandle<OmegaWTK::StyleSheets::StyleSheet> buildClampHighlightSheet(){
    OmegaWTK::StyleSheets::StyleSheet::Builder builder;

    auto redShadow = makeShadow(0.f, kShadowOffsetY,
                                kShadowGeometryRadius,
                                kShadowBlur, kShadowOpacity);
    redShadow.color = OmegaWTK::Composition::Color::create8Bit(
        OmegaWTK::Composition::Color::Red8);

    OmegaWTK::StyleSheets::StyleRule rectRule;
    rectRule.selector.tag = "blue_rect";
    // Override only the DropShadow. FillBrush / background-color from
    // the initial sheet still win their cells (the initial rule's
    // values are unchanged and the highlight sheet doesn't author
    // them; the cascade preserves them via the same source-order
    // logic that gave us the DropShadow swap).
    rectRule.setDropShadow(redShadow);
    OmegaWTK::StyleSheets::TransitionSpec shadowTransition;
    shadowTransition.key = OmegaWTK::PropertyKey::DropShadow;
    shadowTransition.timing.durationMs =
        static_cast<std::uint32_t>(kShadowAnimDurationSec * 1000.f);
    shadowTransition.curve = OmegaWTK::Composition::AnimationCurve::EaseInOut();
    rectRule.transitions.push_back(shadowTransition);
    builder.addRule(std::move(rectRule));

    return builder.build();
}

class AnimMenuDelegate final : public OmegaWTK::MenuDelegate {
    OmegaWTK::AppWindow * window_;
    SharedHandle<OmegaWTK::StyleSheets::StyleSheet> highlightSheet_;
    bool highlightOn_ = false;
public:
    AnimMenuDelegate(OmegaWTK::AppWindow * window,
                     SharedHandle<OmegaWTK::StyleSheets::StyleSheet> highlight):
        window_(window), highlightSheet_(std::move(highlight)) {}

    void onSelectItem(unsigned itemIndex) override {
        switch(itemIndex){
            case 0:
                // Toggle the highlight sheet. Push it onto the stack
                // (red shadow wins the cascade) → next Style phase
                // detects the cell change vs the previous frame's
                // black shadow and fires the transition. Pop on the
                // next click → black wins again, transition fires
                // back. Each toggle is one full D7.2 cycle.
                if(window_ == nullptr || highlightSheet_ == nullptr){
                    return;
                }
                if(highlightOn_){
                    window_->removeStyleSheet(highlightSheet_);
                }
                else {
                    window_->addStyleSheet(highlightSheet_);
                }
                highlightOn_ = !highlightOn_;
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

    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
    // install the initial cascade sheet on the window. The menu
    // toggles the `highlight` overlay sheet (built once below, kept
    // alive on the delegate's stack) to fire the DropShadow
    // transition.
    window->addStyleSheet(buildClampInitialSheet());
    auto highlightSheet = buildClampHighlightSheet();

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
    static AnimMenuDelegate animMenuDelegate(window.get(), highlightSheet);
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
