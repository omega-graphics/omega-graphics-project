#include "AQUABase.h"

#ifndef AQUA_CORE_AQUAOBJECT_H
#define AQUA_CORE_AQUAOBJECT_H


AQUA_NAMESPACE_BEGIN

/**
 @brief An object (physical or programmable) located either in Global or Scene Scope.
*/
class AQUA_PUBLIC Object {
public:
    struct Attribute {

    };
    template<class T,class ...Args>
    static SharedHandle<T> Construct(Args && ...args){
        return std::make_shared<T,Args...>(args...);
    };
protected:
    READWRITE_I_PROPERTY Vector<Attribute> programmableAttrs;    
};

struct AQUA_PUBLIC Transform {
    float x,y,z,pitch,yaw,roll;
public:
    Transform() = delete;
    Transform(float x,float y,float z,float pitch,float yaw,float roll);
    void Translate(float x,float y,float z);
    void Rotate(float pitch,float yaw,float roll);
};


/**
 @brief A phyiscal object with physical and programmable attributes located anywhere in a Scene.
*/
class AQUA_PUBLIC PhysObject : public Object {
public:
    struct PhysicalAttribute {

    };
    READWRITE_I_PROPERTY Transform transform;
protected:
    READWRITE_I_PROPERTY Vector<PhysicalAttribute> physicalAttrs; 
};

AQUA_NAMESPACE_END

#endif
