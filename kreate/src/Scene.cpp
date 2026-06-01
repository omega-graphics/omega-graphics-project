#include <kreate/Scene.h>
#include <kreate/Object.h>
#include <kreate/App.h>
#include <kreate/Pipeline.h>
#include <kreate/Mesh.h>
#include "renderer/Renderer.h"
#include <vector>
#include <algorithm>
#include <iterator>

namespace Kreate {

namespace {
struct Node {
    std::shared_ptr<Object> object;
    std::shared_ptr<Object> parent;
    Mat4 cachedWorld = Mat4::identity();
};
} // namespace

struct Scene::Impl {
    std::vector<Node> nodes;
    Mat4 view = Mat4::identity();
    Mat4 projection = Mat4::identity();
    Color clearColor{0.f, 0.f, 0.f, 1.f};

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
    impl->nodes.push_back({std::move(object), std::move(parent), Mat4::identity()});
    return true;
}

bool Scene::remove(const std::shared_ptr<Object> &object) {
    auto it = impl->find(object);
    if (it == impl->nodes.end()) return false;
    // Detach children — they become roots.
    for (auto &n : impl->nodes) {
        if (n.parent == object) n.parent.reset();
    }
    impl->nodes.erase(it);
    return true;
}

void Scene::setViewMatrix(const Mat4 &m) { impl->view = m; }
void Scene::setProjectionMatrix(const Mat4 &m) { impl->projection = m; }
void Scene::setClearColor(Color c) { impl->clearColor = c; }

void Scene::render(App &app) {
    // 1. Compute world transforms. With a flat vector, a parent may follow
    //    its children in iteration order. Resolve in passes until stable;
    //    bounded by graph depth.
    auto &nodes = impl->nodes;
    std::vector<bool> resolved(nodes.size(), false);

    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (resolved[i]) continue;
            auto &n = nodes[i];
            if (!n.parent) {
                n.cachedWorld = n.object->transform();
                resolved[i] = true;
                progress = true;
                continue;
            }
            // Find parent index.
            auto pit = std::find_if(nodes.begin(), nodes.end(),
                [&](const Node &p) { return p.object == n.parent; });
            if (pit == nodes.end()) {
                // Orphan — treat as root.
                n.cachedWorld = n.object->transform();
                resolved[i] = true;
                progress = true;
                continue;
            }
            size_t pidx = static_cast<size_t>(std::distance(nodes.begin(), pit));
            if (!resolved[pidx]) continue;
            n.cachedWorld = pit->cachedWorld * n.object->transform();
            resolved[i] = true;
            progress = true;
        }
    }

    // 2. Encode through the app's renderer.
    Renderer &r = app.internalRenderer();
    r.beginFrame(impl->clearColor);

    // 3. Per-object draws (Engine-Roadmap Phase 1). An object renders
    //    only if it is visible AND has both a pipeline and a mesh — a
    //    missing piece is silently skipped because it represents a
    //    half-constructed Object, not a runtime error. Pipeline-sort to
    //    amortize state changes is a later phase; one pipeline per draw
    //    is fine while there is one pipeline in the scene.
    for (auto &n : nodes) {
        if (!n.object->isVisible()) continue;
        auto pipe = n.object->pipeline();
        auto mesh = n.object->mesh();
        if (!pipe || !mesh) continue;
        Mat4 mvp = impl->projection * impl->view * n.cachedWorld;
        r.draw(*pipe, *mesh, mvp);
    }

    r.endFrameAndPresent();
}

} // namespace Kreate
