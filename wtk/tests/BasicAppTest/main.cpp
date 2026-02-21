#include <OmegaWTK.h>

class MyWidget final : public OmegaWTK::Widget {
    SharedHandle<OmegaWTK::Composition::Canvas> canvas;

    void renderCenteredRedRect() {
        if(!canvas){
            return;
        }

        constexpr float kRectSize = 48.0f;
        auto & bounds = rect();
        OmegaWTK::Core::Rect redRect{
            OmegaWTK::Core::Position{
                (bounds.w - kRectSize) * 0.5f,
                (bounds.h - kRectSize) * 0.5f},
            kRectSize,
            kRectSize};

        auto redBrush = OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(
                OmegaWTK::Composition::Color::Red8));

        rootView->startCompositionSession();
        canvas->drawRect(redRect, redBrush);
        canvas->sendFrame();
        rootView->endCompositionSession();
    }

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {}

    void init() override {
        auto rootLayer = rootView->getLayerTreeLimb()->getRootLayer();
        canvas = rootView->makeCanvas(rootLayer);
        rootView->enable();
        renderCenteredRedRect();
    }

public:
    explicit MyWidget(const OmegaWTK::Core::Rect & rect, OmegaWTK::WidgetPtr parent)
        : OmegaWTK::Widget(rect, parent) {}

    void redraw() {
        renderCenteredRedRect();
    }
};

class MyWindowDelegate final : public OmegaWTK::AppWindowDelegate {
public:
    void windowWillClose(OmegaWTK::Native::NativeEventPtr event) override {
        OmegaWTK::AppInst::terminate();
    }
};

int omegaWTKMain(OmegaWTK::AppInst *app) {
    auto window = make<OmegaWTK::AppWindow>(
        OmegaWTK::Core::Rect{{0, 0}, 500, 500},
        new MyWindowDelegate());

    auto widget = make<MyWidget>(
        OmegaWTK::Core::Rect{{0, 0}, 500, 500},
        OmegaWTK::WidgetPtr{});
    window->add(widget);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();
    widget->redraw();

    return OmegaWTK::AppInst::start();
}
