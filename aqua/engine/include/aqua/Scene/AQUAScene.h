#include "AQUAObject.h"

#ifndef AQUA_SCENE_AQUASCENE_H
#define AQUA_SCENE_AQUASCENE_H

AQUA_NAMESPACE_BEGIN

class AQUA_PUBLIC Scene {

    READWRITE_I_PROPERTY Vector<SharedHandle<PhysObject>> objectContainer;

public:
    void addObject(SharedHandle<PhysObject> object);
};

AQUA_NAMESPACE_END

#endif //AQUA_SCENE_AQUASCENE_H