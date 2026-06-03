#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/UIView.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include "omegaWTK/Composition/DisplayList.h"
#include <omegaWTK/Composition/FontEngine.h>
#include <omegaWTK/Main.h>
#include <iostream>

class TextCompositorWidget final : public OmegaWTK::Widget {
    OmegaWTK::Core::SharedPtr<OmegaWTK::Composition::Font> font;

    void ensureFontLoaded(){
        if(font != nullptr){
            return;
        }
        auto *fontEngine = OmegaWTK::Composition::FontEngine::inst();
        if(fontEngine == nullptr){
            return;
        }
        // Arial resolves on all three platforms: bundled on Windows
        // and macOS; FontConfig substitutes it to Liberation Sans /
        // DejaVu Sans on Linux. Helvetica doesn't ship with Windows
        // — DWrite's `FindFamilyName` returns false and the font is
        // routed to BitmapFallback (the MSDF path then renders nothing).
        OmegaWTK::Composition::FontDescriptor descriptor(
            "Arial",
            28,
            OmegaWTK::Composition::FontDescriptor::Bold);
        font = fontEngine->CreateFont(descriptor);
    }

    void rebuildContent(){
        ensureFontLoaded();
        if(font == nullptr){
            return;
        }

        auto & uv = viewAs<OmegaWTK::UIView>();
        auto & r = rect();
        OmegaWTK::Composition::Rect bounds{
            OmegaWTK::Composition::Point2D{0.f,0.f}, r.w, r.h};

        OmegaWTK::Composition::Rect titleRect{
            OmegaWTK::Composition::Point2D{24.0f,24.0f},
            bounds.w - 48.0f,
            54.0f};
        OmegaWTK::Composition::Rect bodyRect{
            OmegaWTK::Composition::Point2D{
                bounds.w * 0.17f,
                bounds.h * 0.64f},
            bounds.w * 0.66f,
            bounds.h * 0.22f};

        OmegaWTK::UIViewLayout layout {};
        layout.shape("text_bg",OmegaWTK::Shape::Rect(bounds));
        layout.text("title",
            OmegaCommon::UString(U"OmegaWTK Text Compositor"),titleRect);
        layout.text("body",
            OmegaCommon::UString(U"Centered, wrapped text rendered through the compositor."),
            bodyRect);
        uv.setLayout(layout);

        auto black = OmegaWTK::Composition::Color::create8Bit(
            OmegaWTK::Composition::Color::Black8);
        auto style = OmegaWTK::Style::Create();
        style = style->elementBrush("text_bg",OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::White8)),
            false,0.f);
        style = style->textFont("title",font);
        style = style->textColor("title",black);
        style = style->textFont("body",font);
        style = style->textColor("body",black);
        style = style->textAlignment("body",
            OmegaWTK::Composition::TextLayoutDescriptor::MiddleCenter);
        style = style->textWrapping("body",
            OmegaWTK::Composition::TextLayoutDescriptor::WrapByWord);
        uv.setStyle(style);
    }

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onMount() override {
        rebuildContent();
    }

    void resize(OmegaWTK::Composition::Rect & newRect) override {
        viewAs<OmegaWTK::UIView>().resize(newRect);
        rebuildContent();
        invalidate(OmegaWTK::PaintReason::Resize);
    }

public:
    explicit TextCompositorWidget(OmegaWTK::Composition::Rect rect):
        OmegaWTK::Widget(OmegaWTK::ViewPtr(
            new OmegaWTK::UIView(rect, nullptr, "text_root_view"))){}
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
        OmegaWTK::Composition::Rect{{0,0},500,500},
        new MyWindowDelegate());

    auto widget = make<TextCompositorWidget>(
        OmegaWTK::Composition::Rect{{0,0},500,500});
    window->setRootWidget(widget);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return OmegaWTK::AppInst::start();
}
