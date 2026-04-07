#ifndef AQUA_CORE_AQUAOBJECT_H
#define AQUA_CORE_AQUAOBJECT_H

#include "AQUABase.h"

#include <utility>

AQUA_NAMESPACE_BEGIN

/**
 @brief An object (physical or programmable) located either in Global or Scene Scope.
*/
class AQUA_PUBLIC Object {
public:
    virtual ~Object() = default;

    struct Attribute {

    };

    template<class T, class... Args>
    static SharedHandle<T> Construct(Args &&...args) {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }

protected:
    READWRITE_I_PROPERTY Vector<Attribute> programmableAttrs;
};

struct AQUA_PUBLIC Transform {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;

public:
    Transform() = default;
    Transform(float x, float y, float z, float pitch, float yaw, float roll);
    void Translate(float x, float y, float z);
    void Rotate(float pitch, float yaw, float roll);
};

AQUA_NAMESPACE_END

#endif
