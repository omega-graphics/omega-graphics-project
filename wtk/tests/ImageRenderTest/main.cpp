// ImageRenderTest — minimal sanity check that the bitmap-image path
// (Canvas::drawImage → Bitmap visual command → sampled quad) renders
// correctly on every backend. Used as a control while diagnosing the
// DWrite text-bitmap path on Windows: if a vanilla PNG draws here but
// the rasterized-text bitmap does not, the regression sits between
// DWriteTextRect and the bitmap render path; if neither draws, the
// regression is in the bitmap path itself.

#include <omegaWTK/Main.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/Widgets/Primatives.h>
#include <omegaWTK/Widgets/Containers.h>
#include <omegaWTK/Widgets/BasicWidgets.h>
#include <omega-common/img.h>
#include <omega-common/fs.h>

#include <iostream>

// Source-tree path to the test PNG. Baked in by CMake via
// target_compile_definitions so the binary works regardless of the
// current working directory at launch.
#ifndef IMAGE_RENDER_TEST_PNG_PATH
#define IMAGE_RENDER_TEST_PNG_PATH "test.png"
#endif

using namespace OmegaWTK;

class TestWindowDelegate final : public AppWindowDelegate {
public:
    void windowWillClose(Native::NativeEventPtr event) override {
        (void)event;
        AppInst::terminate();
    }
};

int omegaWTKMain(AppInst *app) {
    Composition::Rect windowRect{{0, 0}, 640, 540};

    auto window = make<AppWindow>(windowRect, new TestWindowDelegate());
    window->setTitle("ImageRenderTest");

    // Load the PNG. Failure is a fatal diagnostic — bail with a clear
    // message so the user notices it in the console instead of seeing
    // an empty window and assuming the bitmap path is broken when it's
    // really just an unresolved asset path.
    OmegaCommon::FS::Path imgPath(IMAGE_RENDER_TEST_PNG_PATH);
    auto loadResult = OmegaCommon::Img::loadFromFile(imgPath);
    if(!loadResult.isOk()){
        std::cerr << "ImageRenderTest: failed to load "
                  << IMAGE_RENDER_TEST_PNG_PATH << ": "
                  << loadResult.error() << std::endl;
        return 1;
    }
    auto bitmap = std::make_shared<OmegaCommon::Img::BitmapImage>(
        std::move(loadResult.value()));
    std::cout << "ImageRenderTest: loaded " << IMAGE_RENDER_TEST_PNG_PATH
              << " (" << bitmap->header.width << "x" << bitmap->header.height
              << ", channels=" << bitmap->header.channels << ")" << std::endl;

    // Widget tree: a title Label on top, the Image filling the rest.
    // Two flex slots so the Image gets every available pixel after the
    // fixed-height title; Contain fit keeps the PNG's aspect ratio so
    // we can eyeball orientation without aspect-distortion confusing
    // the diagnostic.
    float contentW = windowRect.w - 32.f;

    auto root = make<VStack>(windowRect, StackOptions{
        .spacing = 8.f,
        .padding = {16.f, 16.f, 16.f, 16.f},
        .mainAlign = StackMainAlign::Start,
        .crossAlign = StackCrossAlign::Stretch
    });

    LabelProps titleProps;
    titleProps.text = U"ImageRenderTest — drawImage / bitmap quad path";
    titleProps.textColor = Composition::Color::create8Bit(Composition::Color::White8);
    titleProps.alignment = Composition::TextLayoutDescriptor::MiddleCenter;
    titleProps.wrapping = Composition::TextLayoutDescriptor::None;
    auto titleLabel = make<Label>(
        Composition::Rect{{0, 0}, contentW, 30.f}, titleProps);
    root->addChild(titleLabel, StackSlot{.flexGrow = 0.f});

    ImageProps imgProps;
    imgProps.source = bitmap;
    imgProps.fitMode = ImageFitMode::Contain;
    auto img = make<Image>(
        Composition::Rect{{0, 0}, contentW, 400.f}, imgProps);
    root->addChild(img, StackSlot{.flexGrow = 1.f});

    window->setRootWidget(root);

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return AppInst::start();
}
