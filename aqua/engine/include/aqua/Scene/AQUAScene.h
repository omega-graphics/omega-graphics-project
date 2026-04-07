#ifndef AQUA_SCENE_AQUASCENE_H
#define AQUA_SCENE_AQUASCENE_H

#include "aqua/Scene/AQUAEntity.h"

#include <string>

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
public:
    using EntityHandle = SharedHandle<AQUAEntity>;

    struct Dimensions {
        unsigned x = 0;
        unsigned y = 0;
        unsigned z = 0;
    } dimensions;

    EntityHandle createEntity(const std::string & name = {});
    void addEntity(const EntityHandle & entity);
    void addObject(const EntityHandle & entity);
    bool removeEntity(const EntityHandle & entity);
    const Vector<EntityHandle> & entities() const;

private:
    Vector<EntityHandle> entityContainer_;
};

AQUA_NAMESPACE_END

#endif //AQUA_SCENE_AQUASCENE_H
