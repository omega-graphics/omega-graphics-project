#include "omegaGTE/GECommandQueue.h"

_NAMESPACE_BEGIN_

    GECommandQueue::GECommandQueue(unsigned size):size(size){};

    GERenderPassDescriptor::ColorAttachment::ClearColor::ClearColor(float r,float g,float b,float a):r(r),g(g),b(b),a(a){

    }

    GERenderPassDescriptor::ColorAttachment::ColorAttachment(ClearColor clearColor,LoadAction loadAction):
    clearColor(clearColor),loadAction(loadAction){

    };
    GERenderPassDescriptor::ColorAttachment::ColorAttachment(ClearColor clearColor,LoadAction loadAction,SharedHandle<GETexture> texture):
    clearColor(clearColor),loadAction(loadAction),texture(texture){

    };

    unsigned GECommandQueue::getSize(){
        return size;
    };
_NAMESPACE_END_
