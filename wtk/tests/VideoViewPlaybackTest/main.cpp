#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/View.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/Composition/Canvas.h>
#include <omegaWTK/Media/MediaIO.h>
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
        OmegaWTK::Core::Rect localBounds{OmegaWTK::Core::Position{0.f, 0.f}, bounds.w, bounds.h};
        videoView = OmegaWTK::VideoViewPtr(new OmegaWTK::VideoView(localBounds, view));
        videoView->setDelegate(&playbackDelegate);
        videoView->setScaleMode(OmegaWTK::VideoScaleMode::AspectFit);

        if(filePath.empty()){
            std::cerr << "[VideoViewPlaybackTest] No file path provided." << std::endl;
            return;
        }

        auto input = OmegaWTK::Media::MediaInputStream::fromFile(filePath);
        if(!videoView->bindPlaybackSource(input)){
            std::cerr << "[VideoViewPlaybackTest] Failed to bind playback source." << std::endl;
            return;
        }

        videoView->play();
    }

    void onPaint(OmegaWTK::PaintContext & context, OmegaWTK::PaintReason reason) override {
        (void)reason;
        context.clear(OmegaWTK::Composition::Color::create8Bit(
            OmegaWTK::Composition::Color::Black8));
    }

    bool isLayoutResizable() const override { return false; }

public:
    explicit VideoWidget(OmegaWTK::ViewPtr view,
                         OmegaWTK::WidgetPtr parent,
                         const OmegaCommon::String & path)
        : OmegaWTK::Widget(std::move(view), parent), filePath(path) {}
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

    const OmegaWTK::Core::Rect windowRect{{0, 0}, 500, 400};

    auto window = make<OmegaWTK::AppWindow>(
        windowRect,
        new MyWindowDelegate());

    auto widget = make<VideoWidget>(
        OmegaWTK::View::Create(windowRect),
        OmegaWTK::WidgetPtr{},
        videoPath);

    window->setRootWidget(widget);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return OmegaWTK::AppInst::start();
}
