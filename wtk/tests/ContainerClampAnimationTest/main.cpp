#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/UIView.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/UI/Menu.h>
#include <omegaWTK/Widgets/BasicWidgets.h>
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include <omegaWTK/Main.h>
#include <algorithm>
#include <chrono>

namespace {

OmegaWTK::Composition::Rect localBounds(const OmegaWTK::Composition::Rect & rect){
    return OmegaWTK::Composition::Rect{
        OmegaWTK::Composition::Point2D{0.f,0.f},
        rect.w,
        rect.h
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

constexpr float kAnimationDurationSec = 0.6f;
// Phase 4.4 menu trigger: longer + more visible than the resting style swap
// so the scheduler-driven shadow tween is unmistakable on screen.
constexpr float kShadowAnimDurationSec = 1.5f;
constexpr float kShadowAnimFromOffsetY  = 6.f;
constexpr float kShadowAnimToOffsetY    = 60.f;

}

class ClampAnimatedChildWidget final : public OmegaWTK::Widget {
    OmegaWTK::UIViewPtr uiView {};
    bool firstClampRequestIssued = false;
    bool secondClampRequestIssued = false;
    bool animationTriggered = false;

    // Phase 4.4 visual-validation state. While `shadowAnimActive_` is
    // true the widget self-invalidates each repaint so the per-window
    // FrameBuilder keeps ticking the AnimationScheduler — every frame
    // Paint re-reads the shadow side-table cell. Without the loop the
    // first repaint would freeze on whatever value the scheduler had at
    // that instant; this drives a real interpolated tween.
    bool shadowAnimActive_ = false;
    std::chrono::steady_clock::time_point shadowAnimDeadline_ {};

    void ensureUIView(const OmegaWTK::Composition::Rect & bounds){
        auto local = localBounds(bounds);
        if(uiView == nullptr){
            uiView = makeSubView<OmegaWTK::UIView>(local,"clamp_anim_child_view");
        }
        else {
            uiView->resize(local);
        }
    }

    void applyInitialStyle() {
        auto style = OmegaWTK::Style::Create();
        style = style->backgroundColor("clamp_anim_child_view",OmegaWTK::Composition::Color::Transparent);
        style = style->elementBrush("animated_rect",OmegaWTK::Composition::ColorBrush(
                OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Red8)),
                true,
                kAnimationDurationSec);
        style = style->elementDropShadow("animated_rect",makeShadow(0.f,6.f,3.f,10.f,0.60f),true,kAnimationDurationSec);
        uiView->setStyle(style);
    }

    void applyAnimatedTargetStyle() {
        auto style = OmegaWTK::Style::Create();
        style = style->backgroundColor("clamp_anim_child_view",OmegaWTK::Composition::Color::Transparent);
        style = style->elementBrush("animated_rect",OmegaWTK::Composition::ColorBrush(
                OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Blue8)),
                true,
                kAnimationDurationSec);
        style = style->elementDropShadow("animated_rect",makeShadow(0.f,2.f,2.f,6.f,0.40f),true,kAnimationDurationSec);
        uiView->setStyle(style);
    }

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onMount() override {
        ensureUIView(rect());
    }

    void onPaint(OmegaWTK::PaintReason reason) override {
        (void)reason;
        auto bounds = localBounds(rect());
        ensureUIView(bounds);

        OmegaWTK::UIViewLayout layout {};
        layout.shape("animated_rect",OmegaWTK::Shape::RoundedRect(
            OmegaWTK::Composition::RoundedRect{
                OmegaWTK::Composition::Point2D{0.f,0.f},
                bounds.w,
                bounds.h,
                18.f,
                18.f
            }));
        uiView->setLayout(layout);

        if(!firstClampRequestIssued){
            applyInitialStyle();
            uiView->update();

            auto req = rect();
            req.pos.x -= 220.f;
            req.pos.y -= 140.f;
            req.w = 280.f;
            req.h = 240.f;
            requestRect(req,OmegaWTK::GeometryChangeReason::ChildRequest);
            firstClampRequestIssued = true;
            invalidate(OmegaWTK::PaintReason::StateChanged);
            return;
        }

        if(!secondClampRequestIssued){
            applyInitialStyle();
            uiView->update();

            auto req = rect();
            req.pos.x += 420.f;
            req.pos.y += 340.f;
            req.w += 80.f;
            req.h += 40.f;
            requestRect(req,OmegaWTK::GeometryChangeReason::ChildRequest);
            secondClampRequestIssued = true;
            invalidate(OmegaWTK::PaintReason::StateChanged);
            return;
        }

        if(!animationTriggered){
            applyAnimatedTargetStyle();
            uiView->update();
            animationTriggered = true;
            invalidate(OmegaWTK::PaintReason::StateChanged);
        }
        else {
            applyAnimatedTargetStyle();
            uiView->update();
        }

        // Phase 4.4: while a menu-triggered shadow tween is in flight,
        // keep the frame loop alive so the scheduler ticks each turn
        // and Paint re-samples the updated side-table value.
        if(shadowAnimActive_){
            if(std::chrono::steady_clock::now() >= shadowAnimDeadline_){
                shadowAnimActive_ = false;
            }
            else {
                invalidate(OmegaWTK::PaintReason::StateChanged);
            }
        }
    }

public:
    explicit ClampAnimatedChildWidget(OmegaWTK::Composition::Rect rect):
            OmegaWTK::Widget(rect){}

    // Phase 4.4 visual-validation hook. Called from the menu delegate.
    // Registers a scheduler tween on the existing `animated_rect`'s
    // shadow Y-offset (the shadow drops away and back via EaseInOut),
    // arms the self-invalidate loop, and requests one repaint so the
    // pump starts immediately rather than waiting for the next event.
    void triggerShadowAnimation(){
        if(uiView == nullptr){
            return;
        }
        const auto durationSec = kShadowAnimDurationSec;
        uiView->animateElement(
            "animated_rect",
            OmegaWTK::UIView::AnimationChannel::ShadowOffsetY,
            kShadowAnimFromOffsetY,
            kShadowAnimToOffsetY,
            durationSec,
            OmegaWTK::Composition::AnimationCurve::EaseInOut());
        shadowAnimActive_ = true;
        shadowAnimDeadline_ = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(static_cast<long>(durationSec * 1000.f));
        invalidate(OmegaWTK::PaintReason::StateChanged);
    }
};

class ClampRootContainer final : public OmegaWTK::Container {
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onPaint(OmegaWTK::PaintReason reason) override {
        // Tier 3 Phase 3.9: white backdrop via the hosted UIView
        // (a full-bounds rect element) instead of CanvasView::clear.
        auto & uv = viewAs<OmegaWTK::UIView>();
        auto r = rect();
        OmegaWTK::UIViewLayout layout {};
        layout.shape("clamp_bg",OmegaWTK::Shape::Rect(
            OmegaWTK::Composition::Rect{OmegaWTK::Composition::Point2D{0.f,0.f},r.w,r.h}));
        uv.setLayout(layout);
        auto style = OmegaWTK::Style::Create();
        style = style->elementBrush("clamp_bg",OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::White8)),
            false,0.f);
        uv.setStyle(style);
        uv.update();
        OmegaWTK::Container::onPaint(reason);
    }
public:
    explicit ClampRootContainer(OmegaWTK::Composition::Rect rect):
            OmegaWTK::Container(OmegaWTK::ViewPtr(
                new OmegaWTK::UIView(rect,nullptr,"clamp_root_view"))){}
};

class MyWindowDelegate final : public OmegaWTK::AppWindowDelegate {
public:
    void windowWillClose(OmegaWTK::Native::NativeEventPtr event) override {
        (void)event;
        OmegaWTK::AppInst::terminate();
    }
};

// Phase 4.4: a Menu delegate routes "Animate Shadow" / "Quit" to the
// widget owning the scheduler-driven tween. The widget pointer is
// captured at app-startup time and stays alive for the duration of
// AppInst::start (the widget tree owns it via shared_ptr).
class AnimMenuDelegate final : public OmegaWTK::MenuDelegate {
    SharedHandle<ClampAnimatedChildWidget> target_;
public:
    explicit AnimMenuDelegate(SharedHandle<ClampAnimatedChildWidget> target):
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
    auto window = make<OmegaWTK::AppWindow>(
            OmegaWTK::Composition::Rect{{0,0},500,500},
            new MyWindowDelegate());

    auto container = make<ClampRootContainer>(
            OmegaWTK::Composition::Rect{{0,0},500,500});

    OmegaWTK::ContainerClampPolicy clampPolicy {};
    clampPolicy.contentInsets = {24.f,24.f,24.f,24.f};
    clampPolicy.minWidth = 64.f;
    clampPolicy.minHeight = 64.f;
    clampPolicy.clampPositionToBounds = true;
    clampPolicy.clampSizeToBounds = true;
    container->setClampPolicy(clampPolicy);

    auto child = make<ClampAnimatedChildWidget>(
            OmegaWTK::Composition::Rect{{190.f,160.f},120.f,120.f});
    container->addChild(child);

    window->setRootWidget(container);

    // Phase 4.4 visual-validation menu. The "Animate" menu's first item
    // triggers the scheduler-driven shadow tween on the animated_rect;
    // the separator + "Quit" item match the BasicAppTest convention.
    // Delegate is `static` so it outlives this function (the menu
    // captures it by raw pointer).
    static AnimMenuDelegate animMenuDelegate(child);
    auto menu = make<OmegaWTK::Menu>("MainMenu", std::initializer_list<SharedHandle<OmegaWTK::MenuItem>>{
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
