#ifndef AQUA_WORLD_H
#define AQUA_WORLD_H

#include "Base.h"
#include "Math.h"
#include <memory>

namespace Aqua {

/// How a body participates in the simulation.
enum class BodyType {
    Static,   ///< Never moves; treated as infinite mass (ground, level geometry).
    Dynamic,  ///< Integrated each step; affected by gravity and forces.
};

/// Parameters for creating a RigidBody.
struct AQUA_EXPORT BodyDesc {
    BodyType type = BodyType::Dynamic;
    Vec3 position{};
    float mass = 1.f;  ///< Ignored for Static bodies.
};

/// Handle to a body living inside a World. Owned by the World; obtained from
/// `World::addBody` and valid until removed or the World is destroyed.
class AQUA_EXPORT RigidBody {
public:
    ~RigidBody();

    [[nodiscard]] Vec3 position() const;
    void setPosition(const Vec3 &p);

    [[nodiscard]] Vec3 velocity() const;
    void setVelocity(const Vec3 &v);

    [[nodiscard]] BodyType type() const;

private:
    RigidBody();
    friend class World;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// The simulation world: holds bodies and advances them with `step`.
///
/// The simulation backend is intentionally hidden behind this pimpl, the same
/// way Omega kREATE hides OmegaGTE behind its own surface. Whether AQUA grows
/// its own solver or wraps a vendored physics SDK is an implementation decision
/// that must not change this public API.
class AQUA_EXPORT World {
public:
    static std::shared_ptr<World> create();
    ~World();

    void setGravity(const Vec3 &g);
    [[nodiscard]] Vec3 gravity() const;

    /// Adds a body and returns a handle owned by this world.
    std::shared_ptr<RigidBody> addBody(const BodyDesc &desc);

    /// Removes a body. Returns false if it was not in this world.
    bool removeBody(const std::shared_ptr<RigidBody> &body);

    /// Number of bodies currently in the world.
    [[nodiscard]] std::size_t bodyCount() const;

    /// Advances the simulation by `dt` seconds.
    void step(float dt);

private:
    World();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Aqua

#endif // AQUA_WORLD_H
