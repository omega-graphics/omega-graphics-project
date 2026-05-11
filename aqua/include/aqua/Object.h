#ifndef AQUA_OBJECT_H
#define AQUA_OBJECT_H

#include "Base.h"
#include "Math.h"
#include <memory>
#include <string>

namespace Aqua {

class Pipeline;

/// Renderable scene node. Pure data; rendering activates once GEMesh lands
/// (see gte/docs/GEMesh-TextureAssets-Implementation-Plan.md). A mesh
/// handle will be added then; today an Object is transform + pipeline.
class AQUA_EXPORT Object : public std::enable_shared_from_this<Object> {
public:
    static std::shared_ptr<Object> create(std::shared_ptr<Pipeline> pipeline = nullptr);
    ~Object();

    void setTransform(const Mat4 &t);
    const Mat4 &transform() const;

    void setPipeline(std::shared_ptr<Pipeline> p);
    std::shared_ptr<Pipeline> pipeline() const;

    void setVisible(bool v);
    bool isVisible() const;

    void setName(std::string n);
    const std::string &name() const;

private:
    Object();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Aqua

#endif // AQUA_OBJECT_H
