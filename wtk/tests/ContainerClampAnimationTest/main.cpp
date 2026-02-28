#include <OmegaWTK.h>
#include <algorithm>

namespace {

OmegaWTK::Core::Rect localBounds(const OmegaWTK::Core::Rect & rect){
    return OmegaWTK::Core::Rect{
        OmegaWTK::Core::Position{0.f,0.f},
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

}

class ClampAnimatedChildWidget final : public OmegaWTK::Widget {
    OmegaWTK::UIViewPtr uiView {};
    bool firstClampRequestIssued = false;
    bool secondClampRequestIssued = false;

    void ensureUIView(const OmegaWTK::Core::Rect & bounds){
        auto local = localBounds(bounds);
        if(uiView == nullptr){
            uiView = makeUIView(local,rootView,"clamp_anim_child_view");
        }
        else {
            uiView->resize(local);
        }
    }

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onMount() override {
        ensureUIView(rect());
    }

    void onPaint(OmegaWTK::PaintContext & context,OmegaWTK::PaintReason reason) override {
        (void)reason;
        auto bounds = context.bounds();
        ensureUIView(bounds);

        OmegaWTK::UIViewLayout layout {};
        layout.shape("animated_rect",OmegaWTK::Shape::RoundedRect(
            OmegaWTK::Core::RoundedRect{
                OmegaWTK::Core::Position{0.f,0.f},
                bounds.w,
                bounds.h,
                18.f,
                18.f
            }));
        uiView->setLayout(layout);

        auto style = OmegaWTK::StyleSheet::Create();
        style = style->backgroundColor("clamp_anim_child_view",OmegaWTK::Composition::Color::Transparent);
        style = style->elementBrush("animated_rect",OmegaWTK::Composition::ColorBrush(
                OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Red8)),
                true,
                0.35f);
        style = style->elementDropShadow("animated_rect",makeShadow(0.f,6.f,3.f,10.f,0.60f),true,0.35f);
        uiView->setStyleSheet(style);
        uiView->update();

        if(!firstClampRequestIssued){
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
            auto req = rect();
            req.pos.x += 420.f;
            req.pos.y += 340.f;
            req.w += 80.f;
            req.h += 40.f;
            requestRect(req,OmegaWTK::GeometryChangeReason::ChildRequest);
            secondClampRequestIssued = true;
        }
    }

public:
    explicit ClampAnimatedChildWidget(const OmegaWTK::Core::Rect & rect,OmegaWTK::WidgetPtr parent):
            OmegaWTK::Widget(rect,parent){}
};

class ClampRootContainer final : public OmegaWTK::Container {
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onPaint(OmegaWTK::PaintContext & context,OmegaWTK::PaintReason reason) override {
        context.clear(OmegaWTK::Composition::Color::create8Bit(
            OmegaWTK::Composition::Color::White8));
        OmegaWTK::Container::onPaint(context,reason);
    }
public:
    explicit ClampRootContainer(const OmegaWTK::Core::Rect & rect,OmegaWTK::WidgetPtr parent):
            OmegaWTK::Container(rect,parent){}
};

class MyWindowDelegate final : public OmegaWTK::AppWindowDelegate {
public:
    void windowWillClose(OmegaWTK::Native::NativeEventPtr event) override {
        (void)event;
        OmegaWTK::AppInst::terminate();
    }
};

int omegaWTKMain(OmegaWTK::AppInst *app) {
    auto window = make<OmegaWTK::AppWindow>(
            OmegaWTK::Core::Rect{{0,0},500,500},
            new MyWindowDelegate());

    auto container = make<ClampRootContainer>(
            OmegaWTK::Core::Rect{{0,0},500,500},
            OmegaWTK::WidgetPtr{});

    OmegaWTK::ContainerClampPolicy clampPolicy {};
    clampPolicy.contentInsets = {24.f,24.f,24.f,24.f};
    clampPolicy.minWidth = 64.f;
    clampPolicy.minHeight = 64.f;
    clampPolicy.clampPositionToBounds = true;
    clampPolicy.clampSizeToBounds = true;
    container->setClampPolicy(clampPolicy);

    auto child = make<ClampAnimatedChildWidget>(
            OmegaWTK::Core::Rect{{190.f,160.f},120.f,120.f},
            OmegaWTK::WidgetPtr{});
    container->addChild(child);

    window->add(container);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return OmegaWTK::AppInst::start();
}
