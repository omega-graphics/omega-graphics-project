#include "aqua/Scene/AQUAScene.h"

#include <algorithm>

AQUA_NAMESPACE_BEGIN

Scene::EntityHandle Scene::createEntity(const std::string & name) {
    auto entity = Object::Construct<AQUAEntity>(name);
    addEntity(entity);
    return entity;
}

void Scene::addEntity(const EntityHandle & entity) {
    if (!entity) {
        return;
    }

    auto existing = std::find(entityContainer_.begin(), entityContainer_.end(), entity);
    if (existing == entityContainer_.end()) {
        entityContainer_.push_back(entity);
    }
}

void Scene::addObject(const EntityHandle & entity) {
    addEntity(entity);
}

bool Scene::removeEntity(const EntityHandle & entity) {
    auto existing = std::find(entityContainer_.begin(), entityContainer_.end(), entity);
    if (existing == entityContainer_.end()) {
        return false;
    }

    entityContainer_.erase(existing);
    return true;
}

const Vector<Scene::EntityHandle> & Scene::entities() const {
    return entityContainer_;
}

AQUA_NAMESPACE_END
