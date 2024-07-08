#include "aqua/Core/AQUAObject.h"

AQUA_NAMESPACE_BEGIN

void Transform::Translate(float x, float y, float z){
    this->x += x;
    this->y += y;
    this->z += z;
};

void Transform::Rotate(float pitch, float yaw, float roll){
    this->pitch += pitch;
    this->yaw += yaw;
    this->roll += roll;
};

AQUA_NAMESPACE_END