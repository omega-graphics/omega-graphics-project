#include <aqua/AQSpace.h>
#include <vector>
#include <algorithm>

struct AQRigidBody::Impl {
    AQBodyType type = AQBodyType::Dynamic;
    Vec3 position{};
    Vec3 velocity{};
    /// 0 means infinite mass (static / immovable); otherwise 1/mass.
    float invMass = 1.f;
};

AQRigidBody::AQRigidBody() : impl(std::make_unique<Impl>()) {}
AQRigidBody::~AQRigidBody() = default;

Vec3 AQRigidBody::position() const { return impl->position; }
void AQRigidBody::setPosition(const Vec3 &p) { impl->position = p; }
Vec3 AQRigidBody::velocity() const { return impl->velocity; }
void AQRigidBody::setVelocity(const Vec3 &v) { impl->velocity = v; }
AQBodyType AQRigidBody::type() const { return impl->type; }

struct AQSpace::Impl {
    Vec3 gravity{0.f, -9.81f, 0.f};
    std::vector<std::shared_ptr<AQRigidBody>> bodies;
};

AQSpace::AQSpace() : impl(std::make_unique<Impl>()) {}
AQSpace::~AQSpace() = default;

void AQSpace::setGravity(const Vec3 &g) { impl->gravity = g; }
Vec3 AQSpace::gravity() const { return impl->gravity; }

std::shared_ptr<AQRigidBody> AQSpace::addBody(const AQBodyDesc &desc) {
    struct Ctor : AQRigidBody { Ctor() : AQRigidBody() {} };
    auto body = std::shared_ptr<AQRigidBody>(new Ctor());
    body->impl->type = desc.type;
    body->impl->position = desc.position;
    body->impl->invMass =
        (desc.type == AQBodyType::Static || desc.mass <= 0.f) ? 0.f : 1.f / desc.mass;
    impl->bodies.push_back(body);
    return body;
}

bool AQSpace::removeBody(const std::shared_ptr<AQRigidBody> &body) {
    auto &v = impl->bodies;
    auto it = std::find(v.begin(), v.end(), body);
    if (it == v.end()) return false;
    v.erase(it);
    return true;
}

std::size_t AQSpace::bodyCount() const { return impl->bodies.size(); }

void AQSpace::stepInternal(float dt) {
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
