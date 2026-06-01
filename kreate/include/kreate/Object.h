#ifndef KREATE_OBJECT_H
#define KREATE_OBJECT_H

#include "Base.h"
#include "Math.h"
#include <memory>
#include <string>

namespace Kreate {

class Pipeline;
class Mesh;

/// Renderable scene node. Holds a transform, a pipeline, and a mesh.
/// `Scene::render` draws an object iff it is visible AND has both a
/// pipeline and a mesh; any of the three missing skips it.
class KREATE_EXPORT Object : public std::enable_shared_from_this<Object> {
public:
    static std::shared_ptr<Object> create(std::shared_ptr<Pipeline> pipeline = nullptr,
                                          std::shared_ptr<Mesh> mesh = nullptr);
    ~Object();

    void setTransform(const Mat4 &t);
    const Mat4 &transform() const;

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
};

} // namespace Kreate

#endif // KREATE_OBJECT_H
