#include <aqua/Object.h>
#include <aqua/Pipeline.h>

namespace Aqua {

struct Object::Impl {
    Mat4 transform = Mat4::identity();
    std::shared_ptr<Pipeline> pipeline;
    bool visible = true;
    std::string name;
};

Object::Object() : impl(std::make_unique<Impl>()) {}
Object::~Object() = default;

std::shared_ptr<Object> Object::create(std::shared_ptr<Pipeline> pipeline) {
    struct Ctor : Object { Ctor() : Object() {} };
    auto o = std::shared_ptr<Object>(new Ctor());
    o->impl->pipeline = std::move(pipeline);
    return o;
}

void Object::setTransform(const Mat4 &t) { impl->transform = t; }
const Mat4 &Object::transform() const { return impl->transform; }

void Object::setPipeline(std::shared_ptr<Pipeline> p) { impl->pipeline = std::move(p); }
std::shared_ptr<Pipeline> Object::pipeline() const { return impl->pipeline; }

void Object::setVisible(bool v) { impl->visible = v; }
bool Object::isVisible() const { return impl->visible; }

void Object::setName(std::string n) { impl->name = std::move(n); }
const std::string &Object::name() const { return impl->name; }

} // namespace Aqua
