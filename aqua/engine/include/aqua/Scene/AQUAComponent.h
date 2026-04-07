#ifndef AQUA_SCENE_AQUACOMPONENT_H
#define AQUA_SCENE_AQUACOMPONENT_H

#include "aqua/Core/AQUABase.h"

AQUA_NAMESPACE_BEGIN

class AQUAEntity;

class AQUA_PUBLIC AQUAComponent {
public:
    virtual ~AQUAComponent() = default;

    AQUAEntity * owner() const {
        return owner_;
    }

private:
    friend class AQUAEntity;

    void setOwner(AQUAEntity * owner) {
        owner_ = owner;
    }

    AQUAEntity * owner_ = nullptr;
};

AQUA_NAMESPACE_END

#endif
