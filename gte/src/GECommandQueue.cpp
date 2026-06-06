#include "omegaGTE/GECommandQueue.h"

_NAMESPACE_BEGIN_

    GECommandQueue::GECommandQueue(const GECommandQueueDesc & desc,
                                   GECommandQueueDesc::Type achievedType):
        size(desc.maxBufferCount), desc_(desc), requestedType_(desc.type) {
        // Record what the backend actually allocated (post-fallback) on the
        // queue's `desc_.type`, but keep the user's original request in
        // `requestedType_` so isDedicated() can compare.
        desc_.type = achievedType;
    };

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
