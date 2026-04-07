#include "aqua/Core/AQUAObject.h"

AQUA_NAMESPACE_BEGIN

Transform::Transform(float x, float y, float z, float pitch, float yaw, float roll)
    : x(x), y(y), z(z), pitch(pitch), yaw(yaw), roll(roll) {}

void Transform::Translate(float x, float y, float z){
    this->x += x;
    this->y += y;
    this->z += z;
}

void Transform::Rotate(float pitch, float yaw, float roll){
    this->pitch += pitch;
    this->yaw += yaw;
    this->roll += roll;
}

AQUA_NAMESPACE_END
