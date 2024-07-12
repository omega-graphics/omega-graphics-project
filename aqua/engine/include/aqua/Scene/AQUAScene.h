#include "aqua/Core/AQUAObject.h"

#ifndef AQUA_SCENE_AQUASCENE_H
#define AQUA_SCENE_AQUASCENE_H

AQUA_NAMESPACE_BEGIN

/**
 * @brief A multiphased renderer in charge of rendering a 3D scene.
 *  Unlike the `Scene` object, this takes care of calculating and rendering all of the GPU intensive tasks.
 */
class AQUA_PUBLIC SceneRenderer {

    void startVideoCycle();
    
    void endVideoCycle();
public:
    void loadScene();
    void stopScene();
};

/**
 * @brief A 3D space where a game can take place.
 This class is mostly just a database containing the configs and positions for objects before they go into play.
 * 
 */
class AQUA_PUBLIC Scene {

    READWRITE_I_PROPERTY Vector<SharedHandle<Object>> objectContainer;

public:

    struct Dimensions {
        unsigned x,y,z;
    } dimensions;

    void addObject(SharedHandle<Object> object);
};

AQUA_NAMESPACE_END

#endif //AQUA_SCENE_AQUASCENE_H