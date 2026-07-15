#ifndef KREATE_INTERNAL_OBJECTACCESS_H
#define KREATE_INTERNAL_OBJECTACCESS_H

#include <kreate/Object.h>
#include <omegaGTE/GESpace.h>

namespace Kreate {

/// Internal bridge letting the Scene read an Object's transform as the GTE
/// `GESpaceTransform` it stores, without exposing GTE types on the public
/// `Object` surface. Mirrors the `MeshFactory::geMesh` accessor pattern.
struct ObjectAccess {
    static const OmegaGTE::GESpaceTransform &spaceTransform(const Object &o);
};

} // namespace Kreate

#endif // KREATE_INTERNAL_OBJECTACCESS_H
