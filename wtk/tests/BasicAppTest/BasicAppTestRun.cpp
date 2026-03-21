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
        // #region agent log
        auto r = rect();
        dbg_log("BasicAppTestRun.cpp:onMount", "onMount", "D", r.w, r.h, r.pos.x, r.pos.y, rootView ? 1 : 0);
        // #endregion
        ensureUIView(rect());
    }

    void onPaint(OmegaWTK::PaintContext & context,OmegaWTK::PaintReason reason) override {
        (void)reason;
        auto bounds = context.bounds();
        // #region agent log
        dbg_log("BasicAppTestRun.cpp:onPaint:entry", "onPaint entry", "A", bounds.w, bounds.h, bounds.pos.x, bounds.pos.y, uiView ? 1 : 0);
        // #endregion
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

        // #region agent log
        dbg_log("BasicAppTestRun.cpp:onPaint:redRect", "redRect", "B", redRect.pos.x, redRect.pos.y, redRect.w, redRect.h, 0);
        // #endregion

        OmegaWTK::UIViewLayout layout {};
        layout.shape("center_rect",OmegaWTK::Shape::Rect(redRect));
        uiView->setLayout(layout);

        auto style = OmegaWTK::StyleSheet::Create();
        style = style->backgroundColor("basic_view",OmegaWTK::Composition::Color::Transparent);
        style = style->elementBrush("center_rect",OmegaWTK::Composition::ColorBrush(
            OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Red8)));
        uiView->setStyleSheet(style);
        uiView->update();
        // #region agent log
        dbg_log("BasicAppTestRun.cpp:onPaint:afterUpdate", "after uiView->update()", "C", 0, 0, 0, 0, 1);
        // #endregion
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

int RunBasicAppTest(OmegaWTK::AppInst *app) {
    auto window = make<OmegaWTK::AppWindow>(
        OmegaWTK::Core::Rect{{0, 0}, 500, 500},
        new MyWindowDelegate());

    auto widget = make<MyWidget>(
        OmegaWTK::Core::Rect{{0, 0}, 500, 500},
        OmegaWTK::WidgetPtr{});
    window->add(widget);
    // #region agent log
    dbg_log("BasicAppTestRun.cpp:RunBasicAppTest", "widget added", "E", 500, 500, 0, 0, 1);
    // #endregion

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return OmegaWTK::AppInst::start();
}
