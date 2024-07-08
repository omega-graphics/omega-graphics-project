#include "AQUAObject.h"

#ifndef AQUA_CORE_AQUASCENE_H
#define AQUA_CORE_AQUASCENE_H

AQUA_NAMESPACE_BEGIN

class AQUA_PUBLIC Scene {

    READWRITE_I_PROPERTY Vector<SharedHandle<PhysObject>> objectContainer;

public:
    void addObject(SharedHandle<PhysObject> object);
};

AQUA_NAMESPACE_END

#endif