#include <kreate/Object.h>
#include <kreate/Pipeline.h>
#include <kreate/Mesh.h>
#include "ObjectAccess.h"
#include <cmath>

namespace Kreate {

struct Object::Impl {
    // The transform lives as GTE's TRS components — the same type GESpace
    // stores — so the Scene can push it straight into the space with no
    // conversion or matrix decomposition.
    OmegaGTE::GESpaceTransform transform;
    std::shared_ptr<Pipeline> pipeline;
    std::shared_ptr<Mesh> mesh;
    bool visible = true;
    std::string name;
};

Object::Object() : impl(std::make_unique<Impl>()) {}
Object::~Object() = default;

std::shared_ptr<Object> Object::create(std::shared_ptr<Pipeline> pipeline,
                                       std::shared_ptr<Mesh> mesh) {
    struct Ctor : Object { Ctor() : Object() {} };
    auto o = std::shared_ptr<Object>(new Ctor());
    o->impl->pipeline = std::move(pipeline);
    o->impl->mesh = std::move(mesh);
    return o;
}

void Object::setPosition(Vec3 position) {
    impl->transform.translation = OmegaGTE::GPoint3D{position.x, position.y, position.z};
}

Vec3 Object::position() const {
    const auto &t = impl->transform.translation;
    return Vec3{t.x, t.y, t.z};
}

void Object::setRotationEuler(float pitch, float yaw, float roll) {
    impl->transform.rotation = OmegaGTE::FQuaternion::fromEuler(pitch, yaw, roll);
}

void Object::setRotationAxis(Vec3 axis, float radians) {
    const float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (len <= 0.f) {
        impl->transform.rotation = OmegaGTE::FQuaternion::Identity();
        return;
    }
    impl->transform.rotation =
        OmegaGTE::FQuaternion::fromAxisAngle(axis.x / len, axis.y / len, axis.z / len, radians);
}

void Object::setScale(Vec3 scale) {
    impl->transform.scale = OmegaGTE::GPoint3D{scale.x, scale.y, scale.z};
}

Vec3 Object::scale() const {
    const auto &s = impl->transform.scale;
    return Vec3{s.x, s.y, s.z};
}

void Object::setPipeline(std::shared_ptr<Pipeline> p) { impl->pipeline = std::move(p); }
std::shared_ptr<Pipeline> Object::pipeline() const { return impl->pipeline; }

void Object::setMesh(std::shared_ptr<Mesh> m) { impl->mesh = std::move(m); }
std::shared_ptr<Mesh> Object::mesh() const { return impl->mesh; }

void Object::setVisible(bool v) { impl->visible = v; }
bool Object::isVisible() const { return impl->visible; }

void Object::setName(std::string n) { impl->name = std::move(n); }
const std::string &Object::name() const { return impl->name; }

const OmegaGTE::GESpaceTransform &ObjectAccess::spaceTransform(const Object &o) {
    return o.impl->transform;
}

} // namespace Kreate
