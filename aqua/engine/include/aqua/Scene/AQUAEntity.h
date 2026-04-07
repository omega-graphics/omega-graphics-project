#ifndef AQUA_SCENE_AQUAENTITY_H
#define AQUA_SCENE_AQUAENTITY_H

#include "AQUAComponent.h"
#include "aqua/Core/AQUAObject.h"

#include <string>

AQUA_NAMESPACE_BEGIN

class AQUA_PUBLIC AQUAEntity : public Object {
public:
    explicit AQUAEntity(const std::string & name = {});

    std::uint64_t id() const;
    const std::string & name() const;
    void setName(const std::string & name);

    Transform & transform();
    const Transform & transform() const;

    void addComponent(const SharedHandle<AQUAComponent> & component);
    bool removeComponent(const SharedHandle<AQUAComponent> & component);
    const Vector<SharedHandle<AQUAComponent>> & components() const;

private:
    static std::uint64_t NextId();

    std::uint64_t id_ = 0;
    std::string name_;
    Transform transform_;
    Vector<SharedHandle<AQUAComponent>> components_;
};

AQUA_NAMESPACE_END

#endif
