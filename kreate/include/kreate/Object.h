#ifndef KREATE_OBJECT_H
#define KREATE_OBJECT_H

#include "Base.h"
#include "Math.h"
#include <memory>
#include <string>

namespace Kreate {

class Pipeline;
class Mesh;
struct ObjectAccess; // internal — src/ObjectAccess.h

/// Renderable scene node. Holds a transform, a pipeline, and a mesh.
/// `Scene::render` draws an object iff it is visible AND has both a
/// pipeline and a mesh; any of the three missing skips it.
///
/// The transform is authored as translate / rotate / scale — the same TRS
/// components GTE's `GESpace` stores, which is where the transform actually
/// lives once the object is in a `Scene`. Rotation replaces (not accumulates)
/// on each `setRotation*` call; compose your own delta if you want to spin.
class KREATE_EXPORT Object : public std::enable_shared_from_this<Object> {
public:
    static std::shared_ptr<Object> create(std::shared_ptr<Pipeline> pipeline = nullptr,
                                          std::shared_ptr<Mesh> mesh = nullptr);
    ~Object();

    /// Position in space units.
    void setPosition(Vec3 position);
    Vec3 position() const;

    /// Replace orientation with the Euler angles (radians), applied X→Y→Z.
    void setRotationEuler(float pitch, float yaw, float roll);
    /// Replace orientation with a rotation of `radians` about `axis`
    /// (normalized internally; a zero-length axis clears rotation to identity).
    void setRotationAxis(Vec3 axis, float radians);

    /// Per-axis scale (1,1,1 = unscaled).
    void setScale(Vec3 scale);
    Vec3 scale() const;

    void setPipeline(std::shared_ptr<Pipeline> p);
    std::shared_ptr<Pipeline> pipeline() const;

    void setMesh(std::shared_ptr<Mesh> m);
    std::shared_ptr<Mesh> mesh() const;

    void setVisible(bool v);
    bool isVisible() const;

    void setName(std::string n);
    const std::string &name() const;

private:
    Object();
    struct Impl;
    std::unique_ptr<Impl> impl;

    friend struct ObjectAccess;
};

} // namespace Kreate

#endif // KREATE_OBJECT_H
