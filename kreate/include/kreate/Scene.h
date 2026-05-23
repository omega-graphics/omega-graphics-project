#ifndef KREATE_SCENE_H
#define KREATE_SCENE_H

#include "Base.h"
#include "Math.h"
#include <memory>

namespace Kreate {

class App;
class Object;

class KREATE_EXPORT Scene {
public:
    static std::shared_ptr<Scene> create();
    ~Scene();

    /// Adds `object` to the scene under `parent` (root if null). Returns
    /// true on success; false if `object` is already in the scene or
    /// `parent` is not.
    bool add(std::shared_ptr<Object> object, std::shared_ptr<Object> parent = nullptr);

    /// Removes `object` and detaches any children (children become roots).
    bool remove(const std::shared_ptr<Object> &object);

    void setViewMatrix(const Mat4 &m);
    void setProjectionMatrix(const Mat4 &m);
    void setClearColor(Color c);

    /// Encodes the frame for `app`'s internal renderer.
    void render(App &app);

private:
    Scene();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Kreate

#endif // KREATE_SCENE_H
