# OmegaGTE Quaternion Class Plan

## Why

`rotationEuler(pitch, yaw, roll)` composes three axis rotations via matrix multiplication. This works but has two problems:

1. **Gimbal lock**: When pitch approaches +/-90 degrees, yaw and roll become indistinguishable. The rotation loses a degree of freedom.
2. **Interpolation**: Interpolating between two Euler rotations (e.g. for animation) requires decomposing, interpolating, and recomposing — producing jerky, non-uniform motion along the shortest arc.

Quaternions solve both. They represent rotations as a 4D unit vector `(x, y, z, w)` where `(x, y, z)` is the rotation axis scaled by `sin(angle/2)` and `w = cos(angle/2)`. They compose via quaternion multiplication (no gimbal lock), and interpolate smoothly via `slerp`.

## Proposed API

All additions go in `GTEMath.h`, header-only. The quaternion is a lightweight value type.

```cpp
template<class Ty>
struct Quaternion {
    Ty x, y, z, w;

    // --- Construction ---

    static Quaternion Identity();
    // Returns (0, 0, 0, 1).

    static Quaternion fromAxisAngle(Ty ax, Ty ay, Ty az, Ty radians);
    // Rotation of `radians` around the unit axis (ax, ay, az).

    static Quaternion fromEuler(Ty pitch, Ty yaw, Ty roll);
    // Equivalent to rotationEuler() but as a quaternion.
    // Applies X (pitch) → Y (yaw) → Z (roll), matching the existing convention.

    static Quaternion fromMatrix(const Matrix<Ty,4,4>& m);
    // Extracts the rotation from a 4x4 matrix (upper-left 3x3).

    // --- Arithmetic ---

    Quaternion operator*(const Quaternion& other) const;
    // Hamilton product. Composes rotations: (q1 * q2) applies q2 first, then q1.

    Quaternion operator*(Ty scalar) const;
    friend Quaternion operator*(Ty scalar, const Quaternion& q);

    Quaternion operator+(const Quaternion& other) const;
    Quaternion operator-(const Quaternion& other) const;
    Quaternion operator-() const;  // negation (same rotation, opposite path)

    // --- Operations ---

    Ty length() const;
    Ty lengthSquared() const;
    Quaternion normalized() const;
    Quaternion conjugate() const;   // (-x, -y, -z, w)
    Quaternion inverse() const;     // conjugate / lengthSquared

    Ty dot(const Quaternion& other) const;

    // --- Conversion ---

    Matrix<Ty,4,4> toMatrix() const;
    // Produces a 4x4 rotation matrix (no translation/scale).
    // This is the bridge between quaternion rotations and the existing
    // matrix pipeline: `auto mvp = proj * view * q.toMatrix() * translate;`

    // --- Interpolation ---

    static Quaternion slerp(const Quaternion& a, const Quaternion& b, Ty t);
    // Spherical linear interpolation. t=0 returns a, t=1 returns b.
    // Follows the shortest arc (flips b if dot(a,b) < 0).

    static Quaternion nlerp(const Quaternion& a, const Quaternion& b, Ty t);
    // Normalized linear interpolation. Cheaper than slerp, nearly identical
    // for small angular differences. Good enough for most frame-to-frame animation.
};

using FQuaternion = Quaternion<float>;
```

### Free functions

```cpp
// Rotate a point by a quaternion (q * p * q_inverse, optimized).
template<class Ty>
GPoint3D rotatePoint(const Quaternion<Ty>& q, const GPoint3D& pt);

// Build a lookAt-style quaternion (camera orientation).
FQuaternion lookAtQuaternion(const GPoint3D& forward, const GPoint3D& up);
```

## Implementation Notes

### Hamilton product

```
(a1 + b1*i + c1*j + d1*k) * (a2 + b2*i + c2*j + d2*k) =
  w = a1*a2 - b1*b2 - c1*c2 - d1*d2
  x = a1*b2 + b1*a2 + c1*d2 - d1*c2
  y = a1*c2 - b1*d2 + c1*a2 + d1*b2
  z = a1*d2 + b1*c2 - c1*b2 + d1*a2
```

16 multiplies, 12 adds. No trig.

### `toMatrix()`

```
| 1-2(y²+z²)    2(xy-wz)    2(xz+wy)   0 |
| 2(xy+wz)    1-2(x²+z²)    2(yz-wx)   0 |
| 2(xz-wy)     2(yz+wx)   1-2(x²+y²)  0 |
|     0            0            0       1 |
```

12 multiplies, 12 adds. No trig, no square roots.

### `fromMatrix()`

Shepperd's method: select the largest diagonal element to avoid numerical instability, then extract components. ~20 operations.

### `slerp(a, b, t)`

```
cos_theta = dot(a, b)
if cos_theta < 0: b = -b, cos_theta = -cos_theta   // shortest path
if cos_theta > 0.9995: return nlerp(a, b, t)        // near-identical, avoid div-by-zero
theta = acos(cos_theta)
return (sin((1-t)*theta) * a + sin(t*theta) * b) / sin(theta)
```

One `acos`, two `sin` calls. Falls back to `nlerp` for nearly-identical orientations.

### `fromEuler(pitch, yaw, roll)`

Construct three axis-angle quaternions and multiply:
```
qx = fromAxisAngle(1,0,0, pitch)
qy = fromAxisAngle(0,1,0, yaw)
qz = fromAxisAngle(0,0,1, roll)
return qz * qy * qx
```

Or the fused single-expression form (6 trig calls, ~20 arithmetic ops):
```
cx = cos(pitch/2), sx = sin(pitch/2)
cy = cos(yaw/2),   sy = sin(yaw/2)
cz = cos(roll/2),  sz = sin(roll/2)
w = cx*cy*cz + sx*sy*sz
x = sx*cy*cz - cx*sy*sz
y = cx*sy*cz + sx*cy*sz
z = cx*cy*sz - sx*sy*cz
```

## Integration with Existing Code

### Replacing `rotationEuler` usage

The `TEMesh::rotate()` method currently calls `rotationEuler(pitch, yaw, roll)` which builds a 4x4 matrix. With quaternions:

```cpp
void TEMesh::rotate(float pitch, float yaw, float roll) {
    auto q = FQuaternion::fromEuler(pitch, yaw, roll);
    for(auto & polygon : vertexPolygons){
        polygon.a.pt = rotatePoint(q, polygon.a.pt);
        polygon.b.pt = rotatePoint(q, polygon.b.pt);
        polygon.c.pt = rotatePoint(q, polygon.c.pt);
    }
}
```

When composing with translation/scale (needs a full 4x4 matrix):

```cpp
auto mvp = projection * view * q.toMatrix() * translationMatrix(x, y, z);
```

### Animation example

```cpp
FQuaternion startRot = FQuaternion::fromEuler(0, 0, 0);
FQuaternion endRot = FQuaternion::fromAxisAngle(0, 1, 0, 3.14159f);

for(float t = 0; t <= 1.0f; t += 0.01f){
    auto current = FQuaternion::slerp(startRot, endRot, t);
    auto matrix = current.toMatrix();
    // apply matrix to mesh or upload to GPU
}
```

## File Changes

| File | Change |
|------|--------|
| `GTEMath.h` | Add `Quaternion<Ty>` struct, `FQuaternion` alias, `rotatePoint`, `lookAtQuaternion` |

No other files change. Header-only, no .cpp needed. The existing `rotationEuler` function stays — quaternions are an addition, not a replacement.

## Implementation Order

| Step | Description |
|------|-------------|
| 1 | `Quaternion` struct with `Identity`, `fromAxisAngle`, `fromEuler` |
| 2 | Hamilton product, scalar multiply, add, subtract, negate |
| 3 | `length`, `normalized`, `conjugate`, `inverse`, `dot` |
| 4 | `toMatrix` and `fromMatrix` |
| 5 | `slerp` and `nlerp` |
| 6 | `rotatePoint` free function |
| 7 | Optional: `lookAtQuaternion`, refactor `TEMesh::rotate` |
