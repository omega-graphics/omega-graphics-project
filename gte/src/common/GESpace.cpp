#include "omegaGTE/GESpace.h"

#include <cmath>
#include <iostream>

_NAMESPACE_BEGIN_

namespace {

    /// GTE's `Matrix::operator*` composes in REVERSE relative to the column-major
    /// convention these matrices are built and uploaded in: `A * B` evaluates to
    /// the GPU product `B·A`. (Verified, not assumed: `translationMatrix(1,0,0) *
    /// rotationZ(90°)` comes back with its translation column at (0,1,0) — the
    /// signature of Rz·T, not T·Rz.) Read that way, `operator*` means "apply A
    /// first, then B".
    ///
    /// Spelling that out at every call site invites exactly one silent
    /// transposition bug, so say it once, here. `applyThen(first, second)` returns
    /// the matrix that applies `first` and then `second` — the GPU product
    /// `second · first`.
    inline FMatrix<4,4> applyThen(const FMatrix<4,4> & first, const FMatrix<4,4> & second){
        return first * second;
    }

    bool viewportIsDegenerate(const GEViewport & vp){
        const float depthRange = vp.farDepth - vp.nearDepth;
        return vp.width == 0.f || vp.height == 0.f || depthRange == 0.f;
    }

    /// Compose the origin-aware space→NDC map for `vp`.
    ///
    /// X and Y come straight from `orthographicProjection`, which already emits
    /// exactly the mapping GESpace wants: passing the viewport's bottom edge as
    /// `bottom` and its top edge as `top` (i.e. swapped, since GEViewport is
    /// Y-down) encodes the Y-flip, and its `m[3][0]` / `m[3][1]` terms carry the
    /// viewport origin. Depth is the one row it cannot supply: its Z maps
    /// near→0 but far→**-1**, so this overwrites column 2 / column 3 row 2 with
    /// the [0,1] range the backends clip against.
    FMatrix<4,4> composeSpaceToNDC(const GEViewport & vp){
        auto m = orthographicProjection(vp.x, vp.x + vp.width,      // left,  right
                                        vp.y + vp.height, vp.y,     // bottom, top (Y-flip)
                                        vp.nearDepth, vp.farDepth);

        // z_ndc = (z - near) / (far - near), written column-major: the row-2
        // terms of a column-major matrix are m[2][2] (the z coefficient) and
        // m[3][2] (the constant), so z_ndc = m[2][2]*z + m[3][2].
        const float depthRange = vp.farDepth - vp.nearDepth;
        m[2][2] = 1.f / depthRange;
        m[3][2] = -vp.nearDepth / depthRange;
        return m;
    }

}  // namespace

FMatrix<4,4> GESpaceTransform::modelMatrix() const {
    // Scale first, then rotate, then translate — so the object scales about its
    // own origin, spins in place, and only then moves. The reverse order would
    // scale the translation and make an object's position depend on its size.
    const auto S = scalingMatrix(scale.x, scale.y, scale.z);
    const auto R = rotation.toMatrix();
    const auto T = translationMatrix(translation.x, translation.y, translation.z);
    return applyThen(applyThen(S, R), T);
}

struct GESpace::Impl {
    /// A placed object: a transform, plus the geometry it transforms. The mesh
    /// is a SharedHandle and stays one — GESpace never copies or re-bakes
    /// vertices, so placing one GEMesh in two spaces (or twice in one space)
    /// shares a single GPU buffer between the instances.
    struct Object {
        GESpaceTransform transform;
        /// Null for a transform-only object (`addObject`).
        SharedHandle<GEMesh> mesh;
    };

    GEViewport viewport;
    FMatrix<4,4> spaceToNDC = FMatrix<4,4>::Identity();

    /// Insertion-ordered (IDs are monotonic and Map is ordered), so `objects()`
    /// enumerates deterministically — a renderer walking it draws in a stable
    /// order frame to frame.
    OmegaCommon::Map<GESpaceObjectID, Object> objects;
    GESpaceObjectID nextID = 1;   // 0 is GESpaceInvalidObject

    explicit Impl(const GEViewport & vp):viewport(vp){
        recompose();
    }

    /// Null for an unknown handle. Callers log and degrade; nothing here throws.
    GESpaceTransform * find(GESpaceObjectID id){
        auto it = objects.find(id);
        return it == objects.end() ? nullptr : &it->second.transform;
    }
    const GESpaceTransform * find(GESpaceObjectID id) const {
        auto it = objects.find(id);
        return it == objects.end() ? nullptr : &it->second.transform;
    }

    const Object * findObject(GESpaceObjectID id) const {
        auto it = objects.find(id);
        return it == objects.end() ? nullptr : &it->second;
    }

    /// Every mutator resolves its handle through here. An unknown handle is a
    /// caller bug (a stale or foreign ID), so it is reported at the point of use
    /// — loudly, naming the operation and the handle — rather than silently
    /// mutating nothing and leaving the caller to wonder why the object never
    /// moves.
    GESpaceTransform * findForWrite(GESpaceObjectID id, const char * op){
        auto * t = find(id);
        if(t == nullptr){
            std::cerr << "[GESpace] error: " << op << "() on unknown object " << id
                      << "; ignoring." << std::endl;
        }
        return t;
    }

    void recompose(){
        if(viewportIsDegenerate(viewport)){
            std::cerr << "[GESpace] error: degenerate viewport (width=" << viewport.width
                      << ", height=" << viewport.height
                      << ", depth range=" << (viewport.farDepth - viewport.nearDepth)
                      << "); space->NDC is identity until a valid viewport is set."
                      << std::endl;
            spaceToNDC = FMatrix<4,4>::Identity();
            return;
        }
        spaceToNDC = composeSpaceToNDC(viewport);
    }
};

GESpace::GESpace(const GEViewport & viewport):impl(std::make_unique<Impl>(viewport)){

}

GESpace::~GESpace() = default;

void GESpace::setViewport(const GEViewport & viewport){
    impl->viewport = viewport;
    impl->recompose();
}

const GEViewport & GESpace::viewport() const {
    return impl->viewport;
}

FMatrix<4,4> GESpace::spaceToNDC() const {
    return impl->spaceToNDC;
}

// -------------------------------------------------------------------------
// Objects and transforms
// -------------------------------------------------------------------------

GESpaceObjectID GESpace::addObject(const GESpaceTransform & transform){
    const GESpaceObjectID id = impl->nextID++;
    Impl::Object obj;
    obj.transform = transform;
    impl->objects[id] = obj;
    return id;
}

GESpaceObjectID GESpace::addMesh(const SharedHandle<GEMesh> & mesh,
                                 const GESpaceTransform & transform){
    if(mesh == nullptr){
        // Handing back a live handle for a null mesh would let the caller
        // transform and draw nothing, and only find out at the (blank) frame.
        // Refuse it here, where the stack still says who did it.
        std::cerr << "[GESpace] error: addMesh() called with a null mesh; "
                     "returning GESpaceInvalidObject." << std::endl;
        return GESpaceInvalidObject;
    }
    const GESpaceObjectID id = impl->nextID++;
    Impl::Object obj;
    obj.transform = transform;
    obj.mesh = mesh;
    impl->objects[id] = obj;
    return id;
}

SharedHandle<GEMesh> GESpace::meshOf(GESpaceObjectID id) const {
    const auto * obj = impl->findObject(id);
    // Deliberately quiet: a transform-only object having no mesh is normal, and
    // so is probing a handle you are about to discard. The mutators are the loud
    // ones, because there a bad handle means work silently going nowhere.
    return obj == nullptr ? nullptr : obj->mesh;
}

void GESpace::remove(GESpaceObjectID id){
    auto it = impl->objects.find(id);
    if(it == impl->objects.end()){
        std::cerr << "[GESpace] error: remove() on unknown object " << id
                  << "; ignoring." << std::endl;
        return;
    }
    // nextID is never rewound, so this handle is retired for the life of the
    // space: a caller still holding it gets the loud unknown-handle path, not a
    // silent hit on whatever object is added next.
    impl->objects.erase(it);
}

OmegaCommon::Vector<GESpaceObjectID> GESpace::objects() const {
    OmegaCommon::Vector<GESpaceObjectID> ids;
    ids.reserve(impl->objects.size());
    for(const auto & entry : impl->objects){
        ids.push_back(entry.first);
    }
    // OmegaCommon::Map is ordered and IDs are monotonic, so this comes out in
    // insertion order for free.
    return ids;
}

bool GESpace::contains(GESpaceObjectID id) const {
    return impl->find(id) != nullptr;
}

void GESpace::setTranslation(GESpaceObjectID id, const GPoint3D & translation){
    if(auto * t = impl->findForWrite(id, "setTranslation")){
        t->translation = translation;
    }
}

void GESpace::translate(GESpaceObjectID id, float dx, float dy, float dz){
    if(auto * t = impl->findForWrite(id, "translate")){
        t->translation.x += dx;
        t->translation.y += dy;
        t->translation.z += dz;
    }
}

void GESpace::setRotation(GESpaceObjectID id, const FQuaternion & rotation){
    if(auto * t = impl->findForWrite(id, "setRotation")){
        t->rotation = rotation;
    }
}

void GESpace::rotate(GESpaceObjectID id, float pitch, float yaw, float roll){
    if(auto * t = impl->findForWrite(id, "rotate")){
        // `q1 * q2` applies q2 first, then q1 (see Quaternion::operator*), so the
        // delta goes on the LEFT: the new rotation is applied on top of whatever
        // the object already had, about the space's axes. Normalize as we go —
        // a long chain of products otherwise drifts off the unit sphere and the
        // orientation slowly acquires a scale.
        t->rotation = (FQuaternion::fromEuler(pitch, yaw, roll) * t->rotation).normalized();
    }
}

void GESpace::rotateAxis(GESpaceObjectID id, float ax, float ay, float az, float radians){
    auto * t = impl->findForWrite(id, "rotateAxis");
    if(t == nullptr){
        return;
    }
    const float len = std::sqrt(ax*ax + ay*ay + az*az);
    if(len == 0.f){
        std::cerr << "[GESpace] warning: rotateAxis called with a zero-length axis on object "
                  << id << "; ignoring." << std::endl;
        return;
    }
    const auto delta = FQuaternion::fromAxisAngle(ax/len, ay/len, az/len, radians);
    t->rotation = (delta * t->rotation).normalized();
}

void GESpace::setScale(GESpaceObjectID id, const GPoint3D & scale){
    if(auto * t = impl->findForWrite(id, "setScale")){
        t->scale = scale;
    }
}

void GESpace::scale(GESpaceObjectID id, float sx, float sy, float sz){
    if(auto * t = impl->findForWrite(id, "scale")){
        // Multiplicative, matching translate()/rotate(): scale(2,2,2) twice is a
        // 4x object, not a 2x one.
        t->scale.x *= sx;
        t->scale.y *= sy;
        t->scale.z *= sz;
    }
}

const GESpaceTransform & GESpace::transformOf(GESpaceObjectID id) const {
    static const GESpaceTransform identity;
    const auto * t = impl->find(id);
    if(t == nullptr){
        std::cerr << "[GESpace] error: transformOf() on unknown object " << id
                  << "; returning an identity transform." << std::endl;
        return identity;
    }
    return *t;
}

FMatrix<4,4> GESpace::objectTransform(GESpaceObjectID id) const {
    const auto * t = impl->find(id);
    if(t == nullptr){
        std::cerr << "[GESpace] error: objectTransform() on unknown object " << id
                  << "; returning the bare space->NDC matrix." << std::endl;
        return impl->spaceToNDC;
    }
    // Model takes the object from local units into space units; spaceToNDC takes
    // it the rest of the way. Apply model, THEN spaceToNDC.
    return applyThen(t->modelMatrix(), impl->spaceToNDC);
}

_NAMESPACE_END_
