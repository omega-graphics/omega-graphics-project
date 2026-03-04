#include <OmegaWTK.h>
#include <iostream>

class SVGDelegate final : public OmegaWTK::SVGViewDelegate {
public:
    void onSVGLoaded() override {
        std::cerr << "[SVGViewRenderTest] SVG loaded successfully." << std::endl;
    }
    void onSVGParseError(const OmegaCommon::String & message) override {
        std::cerr << "[SVGViewRenderTest] SVG parse error: " << message << std::endl;
    }
};

class SVGWidget final : public OmegaWTK::Widget {
    OmegaWTK::SVGViewPtr svgView;
    SVGDelegate svgDelegate;

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onMount() override {
        auto bounds = rect();
        OmegaWTK::Core::Rect localBounds{OmegaWTK::Core::Position{0.f, 0.f}, bounds.w, bounds.h};
        svgView = makeSVGView(localBounds, rootView);
        svgView->setDelegate(&svgDelegate);

        const OmegaCommon::String svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"500\" height=\"500\">"

            "<rect x=\"30\" y=\"30\" width=\"120\" height=\"80\" "
            "fill=\"#3366CC\" stroke=\"#001133\" stroke-width=\"2\"/>"

            "<rect x=\"200\" y=\"30\" width=\"100\" height=\"80\" rx=\"12\" ry=\"12\" "
            "fill=\"#CC6633\" stroke=\"#331100\" stroke-width=\"2\"/>"

            "<circle cx=\"100\" cy=\"220\" r=\"60\" "
            "fill=\"#33AA33\" stroke=\"#003300\" stroke-width=\"3\"/>"

            "<ellipse cx=\"300\" cy=\"220\" rx=\"90\" ry=\"50\" "
            "fill=\"#AA33AA\" stroke=\"#330033\" stroke-width=\"2\"/>"

            "<line x1=\"30\" y1=\"340\" x2=\"470\" y2=\"340\" "
            "stroke=\"#888888\" stroke-width=\"2\"/>"

            "<polygon points=\"250,360 300,460 200,460\" "
            "fill=\"#CCCC00\" stroke=\"#333300\" stroke-width=\"2\"/>"

            "<path d=\"M 30 400 L 80 370 L 130 420 Z\" "
            "fill=\"#FF6600\" stroke=\"#663300\" stroke-width=\"2\"/>"

            "</svg>";

        svgView->setSourceString(svg);
        svgView->renderNow();
    }

    void onPaint(OmegaWTK::PaintContext & context, OmegaWTK::PaintReason reason) override {
        (void)reason;
        context.clear(OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::White8));
    }

    bool isLayoutResizable() const override { return false; }

public:
    explicit SVGWidget(const OmegaWTK::Core::Rect & rect, OmegaWTK::WidgetPtr parent)
        : OmegaWTK::Widget(rect, parent) {}
};

class MyWindowDelegate final : public OmegaWTK::AppWindowDelegate {
public:
    void windowWillClose(OmegaWTK::Native::NativeEventPtr event) override {
        (void)event;
        OmegaWTK::AppInst::terminate();
    }
};

int omegaWTKMain(OmegaWTK::AppInst * app) {
    const OmegaWTK::Core::Rect windowRect{{0, 0}, 500, 500};

    auto window = make<OmegaWTK::AppWindow>(
        windowRect,
        new MyWindowDelegate());

    auto widget = make<SVGWidget>(
        windowRect,
        OmegaWTK::WidgetPtr{});

    window->add(widget);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return OmegaWTK::AppInst::start();
}
