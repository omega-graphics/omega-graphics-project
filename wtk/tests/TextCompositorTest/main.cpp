#include <OmegaWTK.h>
#include <iostream>
#include <memory>

class TextCompositorWidget final : public OmegaWTK::Widget {
    OmegaWTK::Core::SharedPtr<OmegaWTK::Composition::Font> font;
    OmegaWTK::UIViewPtr accentView {};
    bool loggedUIViewValidation = false;

    static OmegaWTK::Core::Rect accentRectForBounds(const OmegaWTK::Core::Rect & bounds){
        constexpr float kLayerSize = 120.0f;
        return OmegaWTK::Core::Rect{
            OmegaWTK::Core::Position{
                (bounds.w - kLayerSize) * 0.5f,
                (bounds.h - kLayerSize) * 0.5f},
            kLayerSize,
            kLayerSize};
    }

    void ensureAccentView(const OmegaWTK::Core::Rect & bounds){
        auto targetRect = accentRectForBounds(bounds);
        if(accentView == nullptr){
            accentView = makeUIView(targetRect,rootView,"text_accent_view");
        }
        else {
            accentView->resize(targetRect);
        }
    }

    void ensureFontLoaded(){
        if(font != nullptr){
            return;
        }
        auto *fontEngine = OmegaWTK::Composition::FontEngine::inst();
        if(fontEngine == nullptr){
            return;
        }
        OmegaWTK::Composition::FontDescriptor descriptor(
            "Helvetica",
            28,
            OmegaWTK::Composition::FontDescriptor::Bold);
        font = fontEngine->CreateFont(descriptor);
    }

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onMount() override {
        ensureAccentView(rect());
    }

    void resize(OmegaWTK::Core::Rect & newRect) override {
        ensureAccentView(newRect);
    }

    void onPaint(OmegaWTK::PaintContext & context,OmegaWTK::PaintReason reason) override {
        (void)reason;
        ensureFontLoaded();

        context.clear(OmegaWTK::Composition::Color::create8Bit(
            OmegaWTK::Composition::Color::White8));

        auto & bounds = context.bounds();
        ensureAccentView(bounds);

        if(accentView != nullptr){
            constexpr float kRectSize = 56.0f;
            constexpr float kLayerSize = 120.0f;
            OmegaWTK::Core::Rect redRect{
                OmegaWTK::Core::Position{
                    (kLayerSize - kRectSize) * 0.5f,
                    (kLayerSize - kRectSize) * 0.5f},
                kRectSize,
                kRectSize};

            OmegaWTK::UIViewLayout layout {};
            layout.shape("accent_rect",OmegaWTK::Shape::Rect(redRect));
            accentView->setLayout(layout);

            auto style = OmegaWTK::StyleSheet::Create();
            style = style->backgroundColor("text_accent_view",OmegaWTK::Composition::Color::Transparent);
            style = style->elementBrush("accent_rect",OmegaWTK::Composition::ColorBrush(
                OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Red8)));
            accentView->setStyleSheet(style);
            accentView->update();

            if(!loggedUIViewValidation){
                std::cout << "[TextCompositorTest] Accent layer rendered through UIView." << std::endl;
                loggedUIViewValidation = true;
            }
        }

        if(font == nullptr){
            return;
        }

        OmegaWTK::Core::Rect titleRect{
            OmegaWTK::Core::Position{24.0f,24.0f},
            bounds.w - 48.0f,
            54.0f};
        context.drawText(
            OmegaWTK::UniString::fromUTF8("OmegaWTK Text Compositor"),
            font,
            titleRect,
            OmegaWTK::Composition::Color::create8Bit(
                OmegaWTK::Composition::Color::Black8));

        OmegaWTK::Core::Rect bodyRect{
            OmegaWTK::Core::Position{
                bounds.w * 0.17f,
                bounds.h * 0.64f},
            bounds.w * 0.66f,
            bounds.h * 0.22f};
        OmegaWTK::Composition::TextLayoutDescriptor centeredWrap{
            OmegaWTK::Composition::TextLayoutDescriptor::MiddleCenter,
            OmegaWTK::Composition::TextLayoutDescriptor::WrapByWord};

        context.drawText(
            OmegaWTK::UniString::fromUTF8("Centered, wrapped text rendered through the compositor."),
            font,
            bodyRect,
            OmegaWTK::Composition::Color::create8Bit(
                OmegaWTK::Composition::Color::Black8),
            centeredWrap);
    }

public:
    explicit TextCompositorWidget(const OmegaWTK::Core::Rect & rect,OmegaWTK::WidgetPtr parent):
        OmegaWTK::Widget(rect,parent){}
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

    auto widget = make<TextCompositorWidget>(
        OmegaWTK::Core::Rect{{0,0},500,500},
        OmegaWTK::WidgetPtr{});
    window->add(widget);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return OmegaWTK::AppInst::start();
}
