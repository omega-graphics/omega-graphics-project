#include <OmegaWTK.h>
#include <algorithm>

class RoundedFrameWidget final : public OmegaWTK::Widget {
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onPaint(OmegaWTK::PaintContext & context,OmegaWTK::PaintReason reason) override {
        (void)reason;
        auto & bounds = context.bounds();

        const float outerSize = std::min(bounds.w,bounds.h) * 0.70f;
        const float thickness = 8.0f;
        const float outerRadius = 14.0f;
        const float innerSize = std::max(1.0f,outerSize - (thickness * 2.0f));
        const float innerRadius = std::max(1.0f,outerRadius - (thickness * 0.75f));

        OmegaWTK::Core::RoundedRect outer{
            OmegaWTK::Core::Position{
                (bounds.w - outerSize) * 0.5f,
                (bounds.h - outerSize) * 0.5f},
            outerSize,
            outerSize,
            outerRadius,
            outerRadius};
        OmegaWTK::Core::RoundedRect inner{
            OmegaWTK::Core::Position{
                outer.pos.x + thickness,
                outer.pos.y + thickness},
            innerSize,
            innerSize,
            innerRadius,
            innerRadius};

        auto frameBrush = OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(
                OmegaWTK::Composition::Color::Red8));
        auto cutoutBrush = OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(
                OmegaWTK::Composition::Color::Black8));

        context.drawRoundedRect(outer,frameBrush);
        context.drawRoundedRect(inner,cutoutBrush);
    }

    bool isLayoutResizable() const override {
        return false;
    }

public:
    explicit RoundedFrameWidget(const OmegaWTK::Core::Rect & rect,OmegaWTK::WidgetPtr parent):
        OmegaWTK::Widget(rect,parent){}
};

class EllipseOnlyWidget final : public OmegaWTK::Widget {
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onPaint(OmegaWTK::PaintContext & context,OmegaWTK::PaintReason reason) override {
        (void)reason;
        auto & bounds = context.bounds();
        OmegaWTK::Core::Ellipse ellipse{
            bounds.w * 0.5f,
            bounds.h * 0.5f,
            bounds.w * 0.30f,
            bounds.h * 0.22f};

        auto greenBrush = OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(
                OmegaWTK::Composition::Color::Green8));

        context.rootCanvas().drawEllipse(ellipse,greenBrush);
    }

    bool isLayoutResizable() const override {
        return false;
    }

public:
    explicit EllipseOnlyWidget(const OmegaWTK::Core::Rect & rect,OmegaWTK::WidgetPtr parent):
        OmegaWTK::Widget(rect,parent){}
};

class PathOnlyWidget final : public OmegaWTK::Widget {
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onPaint(OmegaWTK::PaintContext & context,OmegaWTK::PaintReason reason) override {
        (void)reason;
        auto & bounds = context.bounds();

        const float x0 = bounds.w * 0.12f;
        const float x1 = bounds.w * 0.38f;
        const float x2 = bounds.w * 0.62f;
        const float x3 = bounds.w * 0.88f;
        const float yHigh = bounds.h * 0.36f;
        const float yLow = bounds.h * 0.64f;

        OmegaWTK::Composition::Path path({x0,yLow},6);
        path.addLine({x1,yHigh});
        path.addLine({x2,yLow});
        path.addLine({x3,yHigh});

        auto yellowBrush = OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(
                OmegaWTK::Composition::Color::Yellow8));
        path.setPathBrush(yellowBrush);
        context.rootCanvas().drawPath(path);
    }

    bool isLayoutResizable() const override {
        return false;
    }

public:
    explicit PathOnlyWidget(const OmegaWTK::Core::Rect & rect,OmegaWTK::WidgetPtr parent):
        OmegaWTK::Widget(rect,parent){}
};

class GeometryHStack final : public OmegaWTK::HStack {
protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onPaint(OmegaWTK::PaintContext & context,OmegaWTK::PaintReason reason) override {
        context.clear(OmegaWTK::Composition::Color::create8Bit(
            OmegaWTK::Composition::Color::Black8));
        OmegaWTK::HStack::onPaint(context,reason);
    }

public:
    explicit GeometryHStack(const OmegaWTK::Core::Rect & rect,
                            OmegaWTK::WidgetPtr parent,
                            const OmegaWTK::StackOptions & options):
        OmegaWTK::HStack(rect,parent,options){}
};

class MyWindowDelegate final : public OmegaWTK::AppWindowDelegate {
public:
    void windowWillClose(OmegaWTK::Native::NativeEventPtr event) override {
        (void)event;
        OmegaWTK::AppInst::terminate();
    }
};

int omegaWTKMain(OmegaWTK::AppInst *app) {
    const OmegaWTK::Core::Rect windowRect{{0,0},500,500};

    auto window = make<OmegaWTK::AppWindow>(
        windowRect,
        new MyWindowDelegate());

    OmegaWTK::StackOptions options {};
    options.spacing = 18.0f;
    options.padding = {20.0f,20.0f,20.0f,20.0f};
    options.mainAlign = OmegaWTK::StackMainAlign::Center;
    options.crossAlign = OmegaWTK::StackCrossAlign::Center;

    auto stack = make<GeometryHStack>(
        windowRect,
        OmegaWTK::WidgetPtr{},
        options);

    const OmegaWTK::Core::Rect childRect{{0,0},130,220};

    auto pathWidget = make<PathOnlyWidget>(
        childRect,
        OmegaWTK::WidgetPtr{});
    auto roundedFrameWidget = make<RoundedFrameWidget>(
        childRect,
        OmegaWTK::WidgetPtr{});
    auto ellipseWidget = make<EllipseOnlyWidget>(
        childRect,
        OmegaWTK::WidgetPtr{});

    OmegaWTK::StackSlot slot {};
    slot.flexGrow = 0.0f;
    slot.flexShrink = 0.0f;
    slot.margin = {0.0f,0.0f,0.0f,0.0f};
    slot.alignSelf = OmegaWTK::StackCrossAlign::Center;

    stack->addChild(pathWidget,slot);
    stack->addChild(roundedFrameWidget,slot);
    stack->addChild(ellipseWidget,slot);

    window->add(stack);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return OmegaWTK::AppInst::start();
}
