#include <OmegaWTK.h>
#include <algorithm>
#include <iostream>
#include <memory>

namespace {
OmegaWTK::Core::Rect localViewBounds(const OmegaWTK::Core::Rect & bounds){
    return OmegaWTK::Core::Rect{
        OmegaWTK::Core::Position{0.f,0.f},
        bounds.w,
        bounds.h
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

class RoundedFrameWidget final : public OmegaWTK::Widget {
    OmegaWTK::UIViewPtr uiView {};
    bool loggedLayout = false;

    void ensureUIView(const OmegaWTK::Core::Rect & bounds){
        auto localBounds = localViewBounds(bounds);
        if(uiView == nullptr){
            uiView = makeUIView(localBounds,rootView,"rounded_frame_view");
        }
        else {
            uiView->resize(localBounds);
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
        auto & bounds = context.bounds();
        ensureUIView(bounds);

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

        OmegaWTK::UIViewLayout layout {};
        layout.shape("rounded_outer",OmegaWTK::Shape::RoundedRect(outer));
        layout.shape("rounded_inner",OmegaWTK::Shape::RoundedRect(inner));
        uiView->setLayout(layout);

        auto style = OmegaWTK::StyleSheet::Create();
        style = style->backgroundColor("rounded_frame_view",OmegaWTK::Composition::Color::Transparent);
        style = style->elementBrush("rounded_outer",OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Red8)),
            true,
            0.28f);
        style = style->elementBrush("rounded_inner",OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::Transparent),
            true,
            0.28f);
        style = style->elementDropShadow("rounded_outer",makeShadow(0.f,4.f,2.f,8.f,0.55f),true,0.28f);
        uiView->setStyleSheet(style);
        uiView->update();

        if(!loggedLayout){
            std::cout << "[EllipsePathCompositorTest] RoundedFrameWidget rendered via UIView." << std::endl;
            loggedLayout = true;
        }
    }

    bool isLayoutResizable() const override {
        return false;
    }

public:
    explicit RoundedFrameWidget(const OmegaWTK::Core::Rect & rect,OmegaWTK::WidgetPtr parent):
        OmegaWTK::Widget(rect,parent){}
};

class EllipseOnlyWidget final : public OmegaWTK::Widget {
    OmegaWTK::UIViewPtr uiView {};
    bool loggedLayout = false;

    void ensureUIView(const OmegaWTK::Core::Rect & bounds){
        auto localBounds = localViewBounds(bounds);
        if(uiView == nullptr){
            uiView = makeUIView(localBounds,rootView,"ellipse_view");
        }
        else {
            uiView->resize(localBounds);
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
        auto & bounds = context.bounds();
        ensureUIView(bounds);
        OmegaWTK::Core::Ellipse ellipse{
            bounds.w * 0.5f,
            bounds.h * 0.5f,
            bounds.w * 0.30f,
            bounds.h * 0.22f};

        OmegaWTK::UIViewLayout layout {};
        layout.shape("ellipse_shape",OmegaWTK::Shape::Ellipse(ellipse));
        uiView->setLayout(layout);

        auto style = OmegaWTK::StyleSheet::Create();
        style = style->backgroundColor("ellipse_view",OmegaWTK::Composition::Color::Transparent);
        style = style->elementBrush("ellipse_shape",OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Green8)),
            true,
            0.30f);
        style = style->elementDropShadow("ellipse_shape",makeShadow(0.f,5.f,2.f,9.f,0.55f),true,0.30f);
        uiView->setStyleSheet(style);
        uiView->update();

        if(!loggedLayout){
            std::cout << "[EllipsePathCompositorTest] EllipseOnlyWidget rendered via UIView." << std::endl;
            loggedLayout = true;
        }
    }

    bool isLayoutResizable() const override {
        return false;
    }

public:
    explicit EllipseOnlyWidget(const OmegaWTK::Core::Rect & rect,OmegaWTK::WidgetPtr parent):
        OmegaWTK::Widget(rect,parent){}
};

class PathOnlyWidget final : public OmegaWTK::Widget {
    OmegaWTK::UIViewPtr uiView {};
    bool loggedLayout = false;

    void ensureUIView(const OmegaWTK::Core::Rect & bounds){
        auto localBounds = localViewBounds(bounds);
        if(uiView == nullptr){
            uiView = makeUIView(localBounds,rootView,"path_view");
        }
        else {
            uiView->resize(localBounds);
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
        auto & bounds = context.bounds();
        ensureUIView(bounds);

        const float x0 = bounds.w * 0.12f;
        const float x1 = bounds.w * 0.38f;
        const float x2 = bounds.w * 0.62f;
        const float x3 = bounds.w * 0.88f;
        const float yHigh = bounds.h * 0.36f;
        const float yLow = bounds.h * 0.64f;

        OmegaGTE::GVectorPath2D vectorPath({x0,yLow});
        vectorPath.append({x1,yHigh});
        vectorPath.append({x2,yLow});
        vectorPath.append({x3,yHigh});

        OmegaWTK::UIViewLayout layout {};
        layout.shape("path_shape",OmegaWTK::Shape::Path(vectorPath,6));
        uiView->setLayout(layout);

        auto style = OmegaWTK::StyleSheet::Create();
        style = style->backgroundColor("path_view",OmegaWTK::Composition::Color::Transparent);
        style = style->elementBrush("path_shape",OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Yellow8)),
            true,
            0.30f);
        style = style->elementDropShadow("path_shape",makeShadow(0.f,5.f,2.f,8.f,0.50f),true,0.30f);
        uiView->setStyleSheet(style);
        uiView->update();

        if(!loggedLayout){
            std::cout << "[EllipsePathCompositorTest] PathOnlyWidget rendered via UIView." << std::endl;
            loggedLayout = true;
        }
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
            OmegaWTK::Composition::Color::White8));
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
