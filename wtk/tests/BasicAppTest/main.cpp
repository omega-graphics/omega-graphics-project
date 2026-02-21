#include <OmegaWTK.h>

class MyWidget final : public OmegaWTK::Widget {
    void renderCenteredRedRect(OmegaWTK::PaintContext & context) {
        constexpr float kRectSize = 48.0f;
        auto & bounds = context.bounds();
        OmegaWTK::Core::Rect redRect{
            OmegaWTK::Core::Position{
                (bounds.w - kRectSize) * 0.5f,
                (bounds.h - kRectSize) * 0.5f},
            kRectSize,
            kRectSize};

        auto redBrush = OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(
                OmegaWTK::Composition::Color::Red8));

        context.drawRect(redRect,redBrush);
    }

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {}

    void onPaint(OmegaWTK::PaintContext & context,OmegaWTK::PaintReason reason) override {
        (void)reason;
        renderCenteredRedRect(context);
    }

public:
    explicit MyWidget(const OmegaWTK::Core::Rect & rect, OmegaWTK::WidgetPtr parent)
        : OmegaWTK::Widget(rect, parent) {}
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

    return OmegaWTK::AppInst::start();
}
