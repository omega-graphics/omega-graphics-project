#include "omegaWTK/Native/NativeVisualTree.h"

namespace OmegaWTK::Native {

    Visual::Visual() = default;

    Visual::Visual(Composition::Rect rect, int z):
        rectPixels(rect),
        zOrderHint(z) {}

    Composition::Rect Visual::rect() const { return rectPixels; }
    int Visual::zOrder() const { return zOrderHint; }
    void Visual::setZOrder(int z) { zOrderHint = z; }

    void Visual::resize(const Composition::Rect & newRect){
        rectPixels = newRect;
        if(onResize_){
            onResize_(newRect);
        }
    }

    void Visual::setOnResize(std::function<void(const Composition::Rect &)> cb){
        onResize_ = std::move(cb);
    }

    void VisualTree::resize(const Composition::Rect & newRect){
        if(auto *r = rootVisual()){
            r->resize(newRect);
        }
    }

}
