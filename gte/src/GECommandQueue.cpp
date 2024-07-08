#include "omegaGTE/GECommandQueue.h"

_NAMESPACE_BEGIN_

    GECommandQueue::GECommandQueue(unsigned size):size(size){};

    unsigned GECommandQueue::getSize(){
        return size;
    };
_NAMESPACE_END_
