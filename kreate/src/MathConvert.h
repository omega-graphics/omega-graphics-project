#ifndef KREATE_INTERNAL_MATHCONVERT_H
#define KREATE_INTERNAL_MATHCONVERT_H

#include <kreate/Math.h>
#include <omegaGTE/GTEMath.h>

namespace Kreate {

/// Convert a Kreate `Mat4` (row-major) to a GTE `FMatrix<4,4>` (column-major).
/// The two stores are transposes of each other, so this is the same logical
/// matrix in the layout GTE and the shaders expect. Internal — not part of the
/// public KREATE surface (keeps GTE types out of `<kreate/Math.h>`).
OmegaGTE::FMatrix<4,4> toFMatrix(const Mat4 &m);

} // namespace Kreate

#endif // KREATE_INTERNAL_MATHCONVERT_H
