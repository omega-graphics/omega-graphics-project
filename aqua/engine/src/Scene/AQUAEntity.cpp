#include "aqua/Scene/AQUAEntity.h"

#include <algorithm>

AQUA_NAMESPACE_BEGIN

AQUAEntity::AQUAEntity(const std::string & name)
    : id_(NextId()), name_(name.empty() ? "Entity" : name), transform_() {}

std::uint64_t AQUAEntity::id() const {
    return id_;
}

const std::string & AQUAEntity::name() const {
    return name_;
}

void AQUAEntity::setName(const std::string & name) {
    name_ = name;
}

Transform & AQUAEntity::transform() {
    return transform_;
}

const Transform & AQUAEntity::transform() const {
    return transform_;
}

void AQUAEntity::addComponent(const SharedHandle<AQUAComponent> & component) {
    if (!component) {
        return;
    }

    auto existing = std::find(components_.begin(), components_.end(), component);
    if (existing != components_.end()) {
        return;
    }

    component->setOwner(this);
    components_.push_back(component);
}

bool AQUAEntity::removeComponent(const SharedHandle<AQUAComponent> & component) {
    auto existing = std::find(components_.begin(), components_.end(), component);
    if (existing == components_.end()) {
        return false;
    }

    (*existing)->setOwner(nullptr);
    components_.erase(existing);
    return true;
}

const Vector<SharedHandle<AQUAComponent>> & AQUAEntity::components() const {
    return components_;
}

std::uint64_t AQUAEntity::NextId() {
    static std::uint64_t nextId = 1;
    return nextId++;
}

AQUA_NAMESPACE_END
