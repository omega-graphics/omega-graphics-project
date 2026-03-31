#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/UIView.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/Composition/Canvas.h>
#include <fstream>
#include <chrono>
#include <sstream>

// #region agent log
static void dbg_log(const char* location, const char* message, const char* hypothesisId, float v0, float v1, float v2, float v3, int i0) {
    std::ostringstream os;
    os << "{\"sessionId\":\"6af4be\",\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
       << ",\"location\":\"" << location << "\",\"message\":\"" << message << "\",\"hypothesisId\":\"" << hypothesisId
       << "\",\"data\":{\"v0\":" << v0 << ",\"v1\":" << v1 << ",\"v2\":" << v2 << ",\"v3\":" << v3 << ",\"i0\":" << i0 << "}}\n";
    std::ofstream f("/home/alex/Documents/Git/omega-graphics-project/.cursor/debug-6af4be.log", std::ios::app);
    if (f) f << os.str();
}
// #endregion

class MyWidget final : public OmegaWTK::Widget {

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {}
    void onMount() override {}

    void onPaint(OmegaWTK::PaintContext & context,OmegaWTK::PaintReason reason) override {
        (void)reason;
        auto bounds = context.bounds();

        // Render directly to the rootView's CanvasView — no UIView.
        context.clear(OmegaWTK::Composition::Color::create8Bit(
            OmegaWTK::Composition::Color::Black8));

        constexpr float kRectSize = 48.0f;
        OmegaWTK::Core::Rect redRect{
            OmegaWTK::Core::Position{
                (bounds.w - kRectSize) * 0.5f,
                (bounds.h - kRectSize) * 0.5f},
            kRectSize,
            kRectSize};

        context.drawRect(redRect, OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Red8)));
    }

public:
    explicit MyWidget(OmegaWTK::ViewPtr view, OmegaWTK::WidgetPtr parent)
        : OmegaWTK::Widget(std::move(view), parent) {}
};

class MyWindowDelegate final : public OmegaWTK::AppWindowDelegate {
public:
    void windowWillClose(OmegaWTK::Native::NativeEventPtr event) override {
        OmegaWTK::AppInst::terminate();
    }
};

int RunBasicAppTest(OmegaWTK::AppInst *app) {
    auto window = make<OmegaWTK::AppWindow>(
        OmegaWTK::Core::Rect{{0, 0}, 500, 500},
        new MyWindowDelegate());

    auto widget = make<MyWidget>(
        OmegaWTK::View::Create(OmegaWTK::Core::Rect{{0, 0}, 500, 500}),
        OmegaWTK::WidgetPtr{});
    window->setRootWidget(widget);
    // #region agent log
    dbg_log("BasicAppTestRun.cpp:RunBasicAppTest", "widget added", "E", 500, 500, 0, 0, 1);
    // #endregion

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return OmegaWTK::AppInst::start();
}
