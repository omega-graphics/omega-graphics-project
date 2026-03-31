#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/Composition/Canvas.h>
#include <omegaWTK/Composition/FontEngine.h>
#include <omegaWTK/Main.h>

using namespace OmegaWTK;


static OmegaWTK::Core::SharedPtr<Composition::Brush> brush;
static SharedHandle<Composition::Font> font;

class RectWidget : public Widget {
public:
    RectWidget(ViewPtr view,WidgetPtr parent):Widget(std::move(view),parent){

    };

    void render() override {
        auto & surface = view->getLayerTree()->getRootLayer()->getSurface();
        surface->drawRect(rect(),brush);
        view->commitRender();
    };

};

int omegaWTKMain(OmegaWTK::AppInst *app){
    Composition::FontDescriptor desc ("Arial",20);

    std::cout << "Hello World" << std::endl;

    brush = Composition::ColorBrush(Composition::Color::Green);

    font = Composition::FontEngine::instance->CreateFont(desc);

    AppWindow window (Core::Rect {Core::Position {0,0},500,500});

    
    app->windowManager->setRootWindow(&window);
    app->windowManager->displayRootWindow();
     
    return AppInst::start();
};
