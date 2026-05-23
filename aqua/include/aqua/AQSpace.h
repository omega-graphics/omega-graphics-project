#ifndef AQUA_AQSPACE_H
#define AQUA_AQSPACE_H

#include "AQBase.h"
#include "AQMath.h"
#include <memory>

/// How a body participates in the simulation.
enum class AQBodyType {
    Static,   ///< Never moves; treated as infinite mass (ground, level geometry).
    Dynamic,  ///< Integrated each step; affected by gravity and forces.
};

/// Parameters for creating an AQRigidBody.
struct AQUA_EXPORT AQBodyDesc {
    AQBodyType type = AQBodyType::Dynamic;
    Vec3 position{};
    float mass = 1.f;  ///< Ignored for Static bodies.
};

/// Handle to a body living inside an AQSpace. Owned by the AQSpace; obtained from
/// `AQSpace::addBody` and valid until removed or the AQSpace is destroyed.
class AQUA_EXPORT AQRigidBody {
public:
    ~AQRigidBody();

    AQUA_NODISCARD Vec3 position() const;
    void setPosition(const Vec3 &p);

    AQUA_NODISCARD Vec3 velocity() const;
    void setVelocity(const Vec3 &v);

    AQUA_NODISCARD AQBodyType type() const;

private:
    AQRigidBody();
    friend class AQSpace;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// The simulation space: holds bodies that AQContext advances each sub-step.
///
/// Spaces are created by `AQContext::createSpace` and owned by that context,
/// which also drives their stepping — see the timekeeping note on `AQContext`.
///
/// The simulation backend is intentionally hidden behind this pimpl, the same
/// way Omega kREATE hides OmegaGTE behind its own surface. Whether AQUA grows
/// its own solver or wraps a vendored physics SDK is an implementation decision
/// that must not change this public API.
class AQUA_EXPORT AQSpace {
public:
    ~AQSpace();

    void setGravity(const Vec3 &g);
    AQUA_NODISCARD Vec3 gravity() const;

    /// Adds a body and returns a handle owned by this space.
    std::shared_ptr<AQRigidBody> addBody(const AQBodyDesc &desc);

    /// Removes a body. Returns false if it was not in this space.
    bool removeBody(const std::shared_ptr<AQRigidBody> &body);

    /// Number of bodies currently in the space.
    AQUA_NODISCARD std::size_t bodyCount() const;

private:
    AQSpace();

    /// Advances this space by one fixed sub-step of `dt` seconds. Driven by
    /// AQContext::advance; deliberately not public so all timekeeping lives in
    /// the context rather than being duplicated per call site.
    void stepInternal(float dt);

    friend class AQContext;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // AQUA_AQSPACE_H
