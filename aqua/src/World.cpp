#include <aqua/World.h>
#include <vector>
#include <algorithm>

namespace Aqua {

struct RigidBody::Impl {
    BodyType type = BodyType::Dynamic;
    Vec3 position{};
    Vec3 velocity{};
    /// 0 means infinite mass (static / immovable); otherwise 1/mass.
    float invMass = 1.f;
};

RigidBody::RigidBody() : impl(std::make_unique<Impl>()) {}
RigidBody::~RigidBody() = default;

Vec3 RigidBody::position() const { return impl->position; }
void RigidBody::setPosition(const Vec3 &p) { impl->position = p; }
Vec3 RigidBody::velocity() const { return impl->velocity; }
void RigidBody::setVelocity(const Vec3 &v) { impl->velocity = v; }
BodyType RigidBody::type() const { return impl->type; }

struct World::Impl {
    Vec3 gravity{0.f, -9.81f, 0.f};
    std::vector<std::shared_ptr<RigidBody>> bodies;
};

World::World() : impl(std::make_unique<Impl>()) {}
World::~World() = default;

std::shared_ptr<World> World::create() {
    // Private-constructor + make_shared idiom, matching kREATE's Scene/Object.
    struct Ctor : World { Ctor() : World() {} };
    return std::shared_ptr<World>(new Ctor());
}

void World::setGravity(const Vec3 &g) { impl->gravity = g; }
Vec3 World::gravity() const { return impl->gravity; }

std::shared_ptr<RigidBody> World::addBody(const BodyDesc &desc) {
    struct Ctor : RigidBody { Ctor() : RigidBody() {} };
    auto body = std::shared_ptr<RigidBody>(new Ctor());
    body->impl->type = desc.type;
    body->impl->position = desc.position;
    body->impl->invMass =
        (desc.type == BodyType::Static || desc.mass <= 0.f) ? 0.f : 1.f / desc.mass;
    impl->bodies.push_back(body);
    return body;
}

bool World::removeBody(const std::shared_ptr<RigidBody> &body) {
    auto &v = impl->bodies;
    auto it = std::find(v.begin(), v.end(), body);
    if (it == v.end()) return false;
    v.erase(it);
    return true;
}

std::size_t World::bodyCount() const { return impl->bodies.size(); }

void World::step(float dt) {
    // Placeholder integrator: semi-implicit Euler under global gravity, no
    // collision yet. This is the thinnest slice that makes the module do
    // something observable — dynamic bodies fall. The real solver (a custom
    // engine or a wrapped SDK) replaces the body of this loop without touching
    // the public API. See kreate/docs/Engine-Roadmap.md, Phase 8.
    for (auto &body : impl->bodies) {
        auto *b = body->impl.get();
        if (b->invMass == 0.f) continue;     // static / immovable
        b->velocity += impl->gravity * dt;   // acceleration == gravity
        b->position += b->velocity * dt;
    }
}

} // namespace Aqua
