#include "aqua/Scene/AQUAScene.h"

AQUA_NAMESPACE_BEGIN

void Scene::addObject(SharedHandle<PhysObject> object){
    objectContainer.push_back(object);
};

AQUA_NAMESPACE_END