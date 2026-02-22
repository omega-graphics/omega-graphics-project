#include "omegaWTK/UI/WidgetTreeHost.h"
#include "../Composition/Compositor.h"

#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/AppWindow.h"
#include <atomic>

namespace OmegaWTK {
    namespace {
        std::atomic<uint64_t> g_widgetTreeSyncLaneSeed {1};

        Composition::Compositor *globalCompositor(){
            static Composition::Compositor compositor;
            return &compositor;
        }
    }

    WidgetTreeHost::WidgetTreeHost():
    compositor(globalCompositor()),
    syncLaneId(g_widgetTreeSyncLaneSeed.fetch_add(1)),
    attachedToWindow(false)
    {

    };

    WidgetTreeHost::~WidgetTreeHost(){
        compositor = nullptr;
    };

    SharedHandle<WidgetTreeHost> WidgetTreeHost::Create(){
        return SharedHandle<WidgetTreeHost>(new WidgetTreeHost());
    };

    void WidgetTreeHost::initWidgetRecurse(Widget *parent){
        parent->init();
        for(auto & child : parent->children){
            initWidgetRecurse(child);
        }
    }

    void WidgetTreeHost::initWidgetTree(){
        root->setTreeHostRecurse(this);
        initWidgetRecurse(root.get());
    }

    void WidgetTreeHost::notifyWindowResize(const Core::Rect &rect){
        if(root != nullptr){
            root->handleHostResize(rect);
        }
    }

    void WidgetTreeHost::attachToWindow(AppWindow * window){
        if(!attachedToWindow) {
            attachedToWindow = true;
            window->_add_widget(root.get());
            window->proxy.setFrontendPtr(compositor);
        }
    };

    void WidgetTreeHost::setRoot(WidgetPtr widget){
        root = widget;
    };
};
