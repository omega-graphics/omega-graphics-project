#include <OmegaWTK.h>

class MyWidget final : public OmegaWTK::Widget {
    OmegaWTK::UIViewPtr uiView {};

    void ensureUIView(const OmegaWTK::Core::Rect & bounds){
        if(uiView == nullptr){
            uiView = makeUIView(bounds,rootView,"basic_view");
        }
        else {
            uiView->resize(bounds);
        }
    }

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {}

    void onMount() override {
        ensureUIView(rect());
    }

    void onPaint(OmegaWTK::PaintContext & context,OmegaWTK::PaintReason reason) override {
        (void)reason;
        auto bounds = context.bounds();
        ensureUIView(bounds);

        context.clear(OmegaWTK::Composition::Color::create8Bit(
            OmegaWTK::Composition::Color::Black8));

        constexpr float kRectSize = 48.0f;
        OmegaWTK::Core::Rect redRect{
            OmegaWTK::Core::Position{
                (bounds.w - kRectSize) * 0.5f,
                (bounds.h - kRectSize) * 0.5f},
            kRectSize,
            kRectSize};

        OmegaWTK::UIViewLayout layout {};
        layout.shape("center_rect",OmegaWTK::Shape::Rect(redRect));
        uiView->setLayout(layout);

        auto style = OmegaWTK::StyleSheet::Create();
        style = style->backgroundColor("basic_view",OmegaWTK::Composition::Color::Transparent);
        style = style->elementBrush("center_rect",OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Red8)));
        uiView->setStyleSheet(style);
        uiView->update();
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
