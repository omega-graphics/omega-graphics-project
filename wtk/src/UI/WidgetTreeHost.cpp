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
        if(compositor != nullptr && root != nullptr){
            unobserveWidgetLayerTreesRecurse(root.get());
        }
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

    void WidgetTreeHost::observeWidgetLayerTreesRecurse(Widget *parent){
        if(parent == nullptr || compositor == nullptr){
            return;
        }
        if(parent->layerTree != nullptr){
            compositor->observeLayerTree(parent->layerTree.get(),syncLaneId);
        }
        for(auto & child : parent->children){
            observeWidgetLayerTreesRecurse(child);
        }
    }

    void WidgetTreeHost::unobserveWidgetLayerTreesRecurse(Widget *parent){
        if(parent == nullptr || compositor == nullptr){
            return;
        }
        if(parent->layerTree != nullptr){
            compositor->unobserveLayerTree(parent->layerTree.get());
        }
        for(auto & child : parent->children){
            unobserveWidgetLayerTreesRecurse(child);
        }
    }

    void WidgetTreeHost::initWidgetTree(){
        observeWidgetLayerTreesRecurse(root.get());
        root->setTreeHostRecurse(this);
        initWidgetRecurse(root.get());
        auto repaintRecurse = [&](auto &&self,Widget *widget) -> void {
            if(widget == nullptr){
                return;
            }
            if(widget->paintMode() == PaintMode::Automatic){
                widget->invalidateNow(PaintReason::Initial);
            }
            for(auto & child : widget->children){
                self(self,child);
            }
        };
        repaintRecurse(repaintRecurse,root.get());
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
        if(root == widget){
            return;
        }
        if(root != nullptr && compositor != nullptr){
            unobserveWidgetLayerTreesRecurse(root.get());
        }
        root = widget;
        if(root != nullptr && compositor != nullptr){
            observeWidgetLayerTreesRecurse(root.get());
        }
    };
};
