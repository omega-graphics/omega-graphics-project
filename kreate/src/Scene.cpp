#include <kreate/Scene.h>
#include <kreate/Object.h>
#include <kreate/App.h>
#include <kreate/Pipeline.h>
#include <kreate/Mesh.h>
#include "renderer/Renderer.h"
#include "ObjectAccess.h"
#include "MathConvert.h"
#include <omegaGTE/GESpace.h>
#include <vector>
#include <algorithm>
#include <iostream>

namespace Kreate {

namespace {
struct Node {
    std::shared_ptr<Object> object;
    std::shared_ptr<Object> parent;
    // The object's slot in the scene's GESpace — where its transform actually
    // lives once placed, and the handle objectTransform() composes the MVP from.
    OmegaGTE::GESpaceObjectID id = OmegaGTE::GESpaceInvalidObject;
};
} // namespace

struct Scene::Impl {
    std::vector<Node> nodes;
    // GESpace is the transform + projection authority (GESpace-Implementation-
    // Plan Phase 5): it owns the view/projection and composes projection·view·
    // model per object. Built with a placeholder viewport that render() re-anchors
    // to the window each frame — for a perspective scene the projection override
    // replaces spaceToNDC, so the viewport only matters to a future 2D scene.
    UniqueHandle<OmegaGTE::GESpace> space;
    Color clearColor{0.f, 0.f, 0.f, 1.f};
    bool warnedHierarchy = false;

    Impl() {
        OmegaGTE::GEViewport vp;
        vp.x = 0.f; vp.y = 0.f; vp.width = 1.f; vp.height = 1.f;
        vp.nearDepth = 0.f; vp.farDepth = 1.f;
        space = std::make_unique<OmegaGTE::GESpace>(vp);
    }

    std::vector<Node>::iterator find(const std::shared_ptr<Object> &o) {
        return std::find_if(nodes.begin(), nodes.end(),
            [&](const Node &n) { return n.object == o; });
    }
};

Scene::Scene() : impl(std::make_unique<Impl>()) {}
Scene::~Scene() = default;

std::shared_ptr<Scene> Scene::create() {
    struct Ctor : Scene { Ctor() : Scene() {} };
    return std::shared_ptr<Scene>(new Ctor());
}

bool Scene::add(std::shared_ptr<Object> object, std::shared_ptr<Object> parent) {
    if (!object) return false;
    if (impl->find(object) != impl->nodes.end()) return false;
    if (parent && impl->find(parent) == impl->nodes.end()) return false;
    // Register the object in the space, seeding its slot with the object's
    // current transform. The returned handle is how render() addresses it.
    const auto id = impl->space->addObject(ObjectAccess::spaceTransform(*object));
    impl->nodes.push_back({std::move(object), std::move(parent), id});
    return true;
}

bool Scene::remove(const std::shared_ptr<Object> &object) {
    auto it = impl->find(object);
    if (it == impl->nodes.end()) return false;
    impl->space->remove(it->id);
    // Detach children — they become roots.
    for (auto &n : impl->nodes) {
        if (n.parent == object) n.parent.reset();
    }
    impl->nodes.erase(it);
    return true;
}

void Scene::setViewMatrix(const Mat4 &m) { impl->space->setViewMatrix(toFMatrix(m)); }
void Scene::setProjectionMatrix(const Mat4 &m) { impl->space->setProjectionMatrix(toFMatrix(m)); }
void Scene::setClearColor(Color c) { impl->clearColor = c; }

void Scene::render(App &app) {
    auto &space = *impl->space;

    // Re-anchor the space's viewport to the current window each frame (handles
    // resize). For a perspective scene this only feeds spaceToNDC, which the
    // projection override replaces, so it is harmless there and correct for a
    // future 2D scene.
    OmegaGTE::GEViewport vp;
    vp.x = 0.f; vp.y = 0.f;
    vp.width = static_cast<float>(app.window().width());
    vp.height = static_cast<float>(app.window().height());
    vp.nearDepth = 0.f; vp.farDepth = 1.f;
    space.setViewport(vp);

    // Push each object's current transform into its space slot. Parent-child
    // world composition is not applied yet (GESpace objects are flat); it is
    // deferred to the Scene-model phase (Engine-Roadmap Phase 6). Warn once if a
    // caller parents an object so this gap is never silent.
    for (auto &n : impl->nodes) {
        if (n.parent && !impl->warnedHierarchy) {
            std::cerr << "[Kreate::Scene] note: object hierarchy (parent transforms) is not yet "
                         "composed; parented objects render at their own transform for now."
                      << std::endl;
            impl->warnedHierarchy = true;
        }
        space.setTransform(n.id, ObjectAccess::spaceTransform(*n.object));
    }

    Renderer &r = app.internalRenderer();
    r.beginFrame(impl->clearColor);

    // Per-object draws. An object renders only if visible AND has both a
    // pipeline and a mesh — a missing piece is a half-constructed Object, not a
    // runtime error, so it is silently skipped. The MVP is the space's
    // objectTransform (projection·view·model), composed correctly inside GESpace.
    for (auto &n : impl->nodes) {
        if (!n.object->isVisible()) continue;
        auto pipe = n.object->pipeline();
        auto mesh = n.object->mesh();
        if (!pipe || !mesh) continue;
        r.draw(*pipe, *mesh, space.objectTransform(n.id));
    }

    r.endFrameAndPresent();
}

} // namespace Kreate
