#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/UIView.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/UI/VideoView.h>
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include <omegaVA/MediaIO.h>
#include <omegaWTK/Main.h>
#include <iostream>

class PlaybackDelegate final : public OmegaWTK::VideoViewDelegate {
public:
    void onVideoReady() override {
        std::cerr << "[VideoViewPlaybackTest] Video ready." << std::endl;
    }
    void onVideoEndOfStream() override {
        std::cerr << "[VideoViewPlaybackTest] End of stream." << std::endl;
    }
    void onVideoError(const OmegaCommon::String & message) override {
        std::cerr << "[VideoViewPlaybackTest] Error: " << message << std::endl;
    }
};

class VideoWidget final : public OmegaWTK::Widget {
    OmegaWTK::VideoViewPtr videoView;
    PlaybackDelegate playbackDelegate;
    OmegaCommon::String filePath;

protected:
    void onThemeSet(OmegaWTK::Native::ThemeDesc & desc) override {
        (void)desc;
    }

    void onMount() override {
        auto bounds = rect();
        OmegaWTK::Composition::Rect localBounds{OmegaWTK::Composition::Point2D{0.f, 0.f}, bounds.w, bounds.h};
        videoView = makeSubView<OmegaWTK::VideoView>(localBounds);
        videoView->setDelegate(&playbackDelegate);
        videoView->setScaleMode(OmegaWTK::VideoScaleMode::AspectFit);

        // Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
        // pre-D8 the black backdrop setup ran inside `onPaint`. With
        // `Widget::onPaint` retired, the backdrop runs once here at
        // mount — this widget is non-resizable
        // (`isLayoutResizable() == false`) so a single setup at mount
        // covers the lifetime. The central walker drives every
        // subsequent paint of the resulting cells via
        // `View::paint(PaintContext&)`.
        // Tier 3 Phase 3.9: black backdrop via the hosted UIView
        // (a full-bounds rect element) instead of CanvasView::clear.
        // The VideoView subview composites on top.
        {
            auto & uv = viewAs<OmegaWTK::UIView>();
            OmegaWTK::UIViewLayout layout {};
            layout.shape("video_bg",OmegaWTK::Shape::Rect(
                OmegaWTK::Composition::Rect{OmegaWTK::Composition::Point2D{0.f,0.f},
                                            bounds.w, bounds.h}));
            uv.setLayout(layout);
            auto style = OmegaWTK::Style::Create();
            style = style->elementBrush("video_bg",OmegaWTK::Composition::ColorBrush(
                OmegaWTK::Composition::Color::create8Bit(OmegaWTK::Composition::Color::Black8)),
                false,0.f);
            uv.setStyle(style);
        }

        if(filePath.empty()){
            std::cerr << "[VideoViewPlaybackTest] No file path provided." << std::endl;
            return;
        }

        auto input = OmegaVA::MediaInputStream::fromFile(filePath);
        if(!videoView->bindPlaybackSource(input)){
            std::cerr << "[VideoViewPlaybackTest] Failed to bind playback source." << std::endl;
            return;
        }

        videoView->play();
    }

    bool isLayoutResizable() const override { return false; }

public:
    explicit VideoWidget(OmegaWTK::Composition::Rect rect,
                         const OmegaCommon::String & path)
        : OmegaWTK::Widget(OmegaWTK::ViewPtr(
              new OmegaWTK::UIView(rect, nullptr, "video_root_view"))),
          filePath(path) {}
};

class MyWindowDelegate final : public OmegaWTK::AppWindowDelegate {
public:
    void windowWillClose(OmegaWTK::Native::NativeEventPtr event) override {
        (void)event;
        OmegaWTK::AppInst::terminate();
    }
};

int omegaWTKMain(OmegaWTK::AppInst * app) {
    OmegaCommon::String videoPath = "test_video.mp4";

    const OmegaWTK::Composition::Rect windowRect{{0, 0}, 500, 400};

    auto window = make<OmegaWTK::AppWindow>(
        windowRect,
        new MyWindowDelegate());

    auto widget = make<VideoWidget>(
        windowRect,
        videoPath);

    window->setRootWidget(widget);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return OmegaWTK::AppInst::start();
}
