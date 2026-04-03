#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include <iostream>

namespace OmegaWTK::Composition {

// --- Layer ---

Layer::Layer(const Core::Rect &rect)
    : parentTree(nullptr), surface_rect(rect), needsNativeResize(false), parent_ptr(nullptr){
};

void Layer::addSubLayer(SharedHandle<Layer> &layer) {
    layer->parent_ptr = this;
    children.push_back(layer);
};

void Layer::removeSubLayer(SharedHandle<Layer> &layer) {
    auto it = children.begin();
    while (it != children.end()) {
        auto l = *it;
        if (l == layer) {
            children.erase(it);
            layer->parent_ptr = nullptr;
            return;
        };
        ++it;
    };
    std::cout << "Error! Could not Remove Sublayer!" << std::endl;
};

void Layer::setEnabled(bool state){
    enabled = state;
}

void Layer::resize(Core::Rect &newRect){
    surface_rect = newRect;
};

LayerTree * Layer::getParentTree(){
    return parentTree;
};

Layer::~Layer() { };

LayerEffect::~LayerEffect(){
    if(type == DropShadow){
        dropShadow.color.~Color();
    }
}

// --- LayerTree ---

LayerTree::LayerTree():rootLayer(nullptr),enabled(true){};

LayerTree::LayerTree(const Core::Rect &rect):
rootLayer(std::make_shared<Layer>(rect)),
enabled(true){
    rootLayer->parentTree = this;
};

SharedHandle<Layer> & LayerTree::getRootLayer(){
    return rootLayer;
}

void LayerTree::addLayer(SharedHandle<Layer> layer){
    layer->parentTree = this;
    rootLayer->addSubLayer(layer);
};

LayerTree::iterator LayerTree::begin(){
    return rootLayer->children.begin();
};

LayerTree::iterator LayerTree::end(){
    return rootLayer->children.end();
};

void LayerTree::enable(){
    if(!enabled && rootLayer != nullptr){
        rootLayer->setEnabled(true);
        enabled = true;
    }
};

void LayerTree::disable(){
    if(enabled && rootLayer != nullptr){
        rootLayer->setEnabled(false);
        enabled = false;
    }
};

void LayerTree::collectAllLayers(OmegaCommon::Vector<Layer *> & out){
    if(rootLayer == nullptr){
        return;
    }
    out.push_back(rootLayer.get());
    for(auto & child : rootLayer->children){
        out.push_back(child.get());
    }
}

void LayerTree::notifyObserversOfResize(Layer *layer){
    for(auto & observer : observers){
        observer->layerHasResized(layer);
    };
};

void LayerTree::notifyObserversOfDisable(Layer *layer){
    for(auto & observer : observers){
        observer->layerHasDisabled(layer);
    };
};

void LayerTree::notifyObserversOfEnable(Layer *layer){
    for(auto & observer : observers){
        observer->layerHasEnabled(layer);
    };
};

void LayerTree::notifyObserversOfWidgetDetach(){
    for(auto & observer : observers){
        observer->hasDetached(this);
    };
};

void LayerTree::addObserver(LayerTreeObserver * observer){
    observers.push_back(observer);
};

void LayerTree::removeObserver(LayerTreeObserver * observer){
    for(auto it = observers.begin();it != observers.end();it++){
        if(observer == *it){
            observers.erase(it);
            break;
        }
    };
};

LayerTree::~LayerTree(){

};

// --- WindowLayer ---

WindowLayer::WindowLayer(Core::Rect & rect,Native::NWH native_window_ptr):native_window_ptr(native_window_ptr),rect(rect){
};

void WindowLayer::redraw(){

};

} // namespace OmegaWTK::Composition
