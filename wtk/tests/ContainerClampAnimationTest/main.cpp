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

#include <limits>

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
//
// Widget-View-Paint-Lifecycle-Plan Tier D / D7.4 (2026-06-04):
// the "Toggle Selected" menu item flips a `:state(selected)` custom
// pseudo-class on the `blue_rect_view` UIView. The initial sheet
// carries a higher-specificity rule
// (`tag="blue_rect", customStates=["selected"]`) that overrides the
// rect's fill to green when the state is on. This is the canonical
// D7.4 verification path — set a string-keyed state, see a different
// cascade outcome. The state set is independent of the
// pseudo-class bitmask (Hover / Pressed / Focused / Disabled) and
// is open-ended, so widget / app code names states freely without
// growing the enum.

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

// D7.3: the pulse keyframe animation runs one cycle in this many
// milliseconds and ping-pongs forever via `Direction::Alternate`.
constexpr std::uint32_t kPulseHalfCycleMs = 1500;

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

    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.4 (2026-06-04):
    // flip the `:state(selected)` custom pseudo-class on the
    // `blue_rect_view` UIView. The initial sheet carries a higher-
    // specificity rule keyed on this state; the cascade re-resolves
    // automatically because `View::setState` calls `markDirty(Style)`
    // when the set actually changes.
    void toggleSelected(){
        if(uiView == nullptr){
            return;
        }
        selected_ = !selected_;
        uiView->setState("selected", selected_);
    }

private:
    bool selected_ = false;
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

    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.4 (2026-06-04):
    // higher-specificity override that only matches when the
    // `blue_rect_view` UIView carries `:state(selected)`. Same `tag`
    // as the base `blue_rect` rule, plus one custom-state token —
    // specificity 11 vs the base rule's 1, so this wins the
    // `FillBrush` cell when the state is on. The base rule still
    // wins the `DropShadow` cell because this override doesn't
    // declare it; only fill changes between state on/off.
    OmegaWTK::StyleSheets::StyleRule rectSelectedRule;
    rectSelectedRule.selector.tag = "blue_rect";
    rectSelectedRule.selector.customStates.push_back("selected");
    rectSelectedRule.setFillBrush(OmegaWTK::Composition::ColorBrush(
        OmegaWTK::Composition::Color::create8Bit(
            OmegaWTK::Composition::Color::Green8)));
    builder.addRule(std::move(rectSelectedRule));

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

// Widget-View-Paint-Lifecycle-Plan Tier D / D7.3 (2026-06-04):
// the `pulse` keyframe-animation sheet. The blue_rect rule binds
// `animation: pulse`; the sheet also declares a `KeyframeAnimation`
// named `pulse` with one property (DropShadow) animating between a
// yellow shadow and a cyan shadow over `kPulseHalfCycleMs`. The
// animation runs effectively forever (`iterations = infinity`)
// and ping-pongs (`Direction::Alternate`) so the visible effect is
// a continuous yellow ⇄ cyan glow under the rounded rect.
//
// Adding this sheet to the window stack while the highlight sheet
// is OFF produces a pulse from yellow to cyan on the otherwise-
// black drop shadow. Adding it while the highlight sheet is on
// races the pulse against the transition for the same (node,
// DropShadow) cell — the scheduler's `registerProperty` cancels
// the prior animation on each new registration, so the pulse
// wins (per CSS, animations dominate transitions). Removing the
// pulse sheet cancels the keyframe binding (via
// `applyKeyframeBindings`); Paint then falls back to whatever the
// cascade resolves for the cell — black if only the initial
// sheet is on, red if the highlight is on.
SharedHandle<OmegaWTK::StyleSheets::StyleSheet> buildClampPulseSheet(){
    using namespace OmegaWTK;
    using namespace OmegaWTK::Composition;

    StyleSheets::StyleSheet::Builder builder;

    // Define the keyframe animation.
    StyleSheets::KeyframeAnimation pulse;
    pulse.name = "pulse";
    pulse.defaultTiming.durationMs = kPulseHalfCycleMs;
    pulse.defaultTiming.iterations = std::numeric_limits<float>::infinity();
    pulse.defaultTiming.direction  = Direction::Alternate;

    StyleSheets::KeyframeAnimationProperty shadowProp;
    shadowProp.key = PropertyKey::DropShadow;

    LayerEffect::DropShadowParams startShadow = makeShadow(0.f, kShadowOffsetY,
                                                           kShadowGeometryRadius,
                                                           kShadowBlur, kShadowOpacity);
    startShadow.color = Color::create8Bit(Color::Yellow8);

    LayerEffect::DropShadowParams endShadow = makeShadow(0.f, kShadowOffsetY,
                                                         kShadowGeometryRadius,
                                                         kShadowBlur, kShadowOpacity);
    // Cyan: no named constant in `Composition::Color`; pack the RGB
    // manually. 0x00FFFF = R=0, G=255, B=255.
    endShadow.color = Color::create8Bit(0x00FFFFu);

    OmegaCommon::Vector<KeyframeValue<AnimatedValue>> kfs;
    kfs.push_back(KeyframeValue<AnimatedValue>{
        0.f, AnimatedValue{startShadow}, AnimationCurve::EaseInOut()});
    kfs.push_back(KeyframeValue<AnimatedValue>{
        1.f, AnimatedValue{endShadow}, nullptr});
    shadowProp.track = KeyframeTrack<AnimatedValue>::From(kfs);

    pulse.properties.push_back(std::move(shadowProp));
    builder.addKeyframeAnimation(std::move(pulse));

    // Rule that binds the pulse to the blue_rect element.
    StyleSheets::StyleRule rectRule;
    rectRule.selector.tag = "blue_rect";
    rectRule.animationName = "pulse";
    builder.addRule(std::move(rectRule));

    return builder.build();
}

class AnimMenuDelegate final : public OmegaWTK::MenuDelegate {
    OmegaWTK::AppWindow * window_;
    SharedHandle<OmegaWTK::StyleSheets::StyleSheet> highlightSheet_;
    SharedHandle<OmegaWTK::StyleSheets::StyleSheet> pulseSheet_;
    // D7.4: the widget that owns the UIView whose
    // `:state(selected)` we flip. Raw pointer — the widget outlives
    // the menu delegate (`make<>` holds a SharedHandle in the app
    // root) and there's no ownership transfer here.
    BlueRectWidget * rectWidget_;
    bool highlightOn_ = false;
    bool pulseOn_ = false;
public:
    AnimMenuDelegate(OmegaWTK::AppWindow * window,
                     SharedHandle<OmegaWTK::StyleSheets::StyleSheet> highlight,
                     SharedHandle<OmegaWTK::StyleSheets::StyleSheet> pulse,
                     BlueRectWidget * rectWidget):
        window_(window),
        highlightSheet_(std::move(highlight)),
        pulseSheet_(std::move(pulse)),
        rectWidget_(rectWidget) {}

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
            case 1:
                // D7.3: Toggle the pulse sheet. Adding pushes a rule
                // with `animation: pulse` onto the cascade — the next
                // Style phase records the binding in
                // `sheetBindings_.animationBindings`, and
                // `applyKeyframeBindings` starts the keyframe
                // animation (yellow ⇄ cyan ping-pong). Removing the
                // sheet drops the binding from the cascade, the
                // reconciliation pass cancels the handles, and the
                // shadow snaps back to whatever the cascade still
                // resolves (black or red, depending on highlight).
                if(window_ == nullptr || pulseSheet_ == nullptr){
                    return;
                }
                if(pulseOn_){
                    window_->removeStyleSheet(pulseSheet_);
                }
                else {
                    window_->addStyleSheet(pulseSheet_);
                }
                pulseOn_ = !pulseOn_;
                break;
            case 2:
                // D7.4: flip `:state(selected)` on the blue_rect_view
                // UIView. The initial sheet's higher-specificity
                // override (selector `tag="blue_rect",
                // customStates=["selected"]`) wins the FillBrush cell
                // when the state is on; the rect paints green. The
                // base rule wins back the cell when the state is off
                // and the rect goes back to blue. No transition is
                // declared on the override, so the change is a snap
                // (CSS-equivalent: no `transition` property on the
                // `:state(selected)` rule = instant on switch).
                //
                // Layering note: `setState` only records the dirty
                // bit on the view. As the idle-context caller, this
                // menu handler is responsible for poking the run
                // loop — `AppWindow::refresh()` schedules the next
                // paint. Multiple state flips in the same callback
                // collapse into ONE frame because the request
                // coalesces.
                if(rectWidget_ != nullptr){
                    rectWidget_->toggleSelected();
                }
                if(window_ != nullptr){
                    window_->refresh();
                }
                break;
            case 4:
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
    // D7.3: build the pulse sheet up front so it's stable for the
    // lifetime of the app (the menu delegate captures it by
    // SharedHandle and we re-use the same handle for add / remove).
    auto pulseSheet = buildClampPulseSheet();

    auto container = make<WhiteRootContainer>(
            OmegaWTK::Composition::Rect{{0, 0}, kWindowW, kWindowH});

    auto child = make<BlueRectWidget>(
            OmegaWTK::Composition::Rect{
                {(kWindowW - kRectW) * 0.5f,
                 (kWindowH - kRectH) * 0.5f - 16.f},   // nudge up so the shadow has room
                kRectW, kRectH});
    container->addChild(child);

    window->setRootWidget(container);

    // Menu: "Animate" > "Animate Shadow" (item 0) | "Toggle Pulse"
    // (item 1) | "Toggle Selected" (item 2) | sep (3) | "Quit" (4).
    // The delegate is `static` so it outlives this function — the
    // menu captures it by raw pointer.
    //
    // D7.4 (2026-06-04): "Toggle Selected" is the new item — flips
    // `:state(selected)` on the blue_rect_view UIView so the cascade
    // re-resolves with the higher-specificity override active.
    static AnimMenuDelegate animMenuDelegate(window.get(),
                                             highlightSheet,
                                             pulseSheet,
                                             child.get());
    auto menu = make<OmegaWTK::Menu>(
        "MainMenu",
        std::initializer_list<SharedHandle<OmegaWTK::MenuItem>>{
            OmegaWTK::CategoricalMenu("Animate", {
                OmegaWTK::ButtonMenuItem("Animate Shadow"),
                OmegaWTK::ButtonMenuItem("Toggle Pulse"),
                OmegaWTK::ButtonMenuItem("Toggle Selected"),
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
