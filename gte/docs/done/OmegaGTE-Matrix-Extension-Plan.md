# OmegaGTE Matrix Math Extension Plan

## Current State

`GTEBase.h` defines a template `Matrix<Ty, column, row>` with `std::array`-based storage and basic accessors (`operator[]`, `at`, `begin/end`, `Create`, `Identity`). Aliases exist: `FMatrix<c,r>`, `IMatrix<c,r>`, `FVec<n>`, etc.

**What's missing**: The Matrix class has no arithmetic operations at all — no multiplication, no addition, no scalar multiply, no transpose, no determinant, no inverse. Transform operations (`translate`, `rotate`, `scale`) are implemented as manual per-vertex loops in `TE.cpp` using raw trig rather than matrices.

This means:
- Transforms can't be composed (`projection * view * model`)
- Each operation re-iterates all vertices
- The rotation code applies Euler angles sequentially (gimbal lock susceptible)
- No way to build a transform pipeline on the CPU side

## Goal

Make `Matrix` a complete linear algebra type so that OmegaGTE code can express transforms as composable matrices, matching what shaders do on the GPU side.

## Proposed API

All additions are to the existing `Matrix<Ty, column, row>` template in `GTEBase.h` plus a new `GTEMath.h` header for free functions.

### Arithmetic operators (on `Matrix`)

```cpp
// Matrix + Matrix (same dimensions)
Matrix operator+(const Matrix& other) const;
Matrix operator-(const Matrix& other) const;

// Scalar * Matrix, Matrix * Scalar
Matrix operator*(Ty scalar) const;
friend Matrix operator*(Ty scalar, const Matrix& m);

// Matrix * Matrix (NxM * MxP = NxP)
template<unsigned P>
Matrix<Ty, column, P> operator*(const Matrix<Ty, row, P>& other) const;

// Matrix * Vector (NxM * Mx1 = Nx1, i.e. FMatrix<4,4> * FVec<4> = FVec<4>)
// Covered by the above template when P=1.

// Compound assignment
Matrix& operator+=(const Matrix& other);
Matrix& operator-=(const Matrix& other);
Matrix& operator*=(Ty scalar);

// Unary negation
Matrix operator-() const;

// Equality
bool operator==(const Matrix& other) const;
bool operator!=(const Matrix& other) const;
```

These are all header-only template implementations — no .cpp file needed.

### Member functions (on `Matrix`)

```cpp
// Transpose: Matrix<Ty,C,R> → Matrix<Ty,R,C>
Matrix<Ty, row, column> transposed() const;

// Data pointer for uploading to GPU buffers
const Ty* data() const;  // returns &_data[0][0]
```

### Free functions (`GTEMath.h`)

```cpp
namespace OmegaGTE {

    // ---- General matrix functions ----

    template<class Ty, unsigned N>
    Ty determinant(const Matrix<Ty,N,N>& m);
    // Specializations for N=2, N=3, N=4.

    template<class Ty, unsigned N>
    Matrix<Ty,N,N> inverse(const Matrix<Ty,N,N>& m);
    // Requires determinant(). Specializations for N=2, N=3, N=4.

    // ---- Transform matrix builders (float, 4x4) ----

    FMatrix<4,4> translationMatrix(float x, float y, float z);
    FMatrix<4,4> scalingMatrix(float x, float y, float z);

    FMatrix<4,4> rotationX(float radians);
    FMatrix<4,4> rotationY(float radians);
    FMatrix<4,4> rotationZ(float radians);
    FMatrix<4,4> rotationEuler(float pitch, float yaw, float roll);

    // ---- Projection / view matrices ----

    FMatrix<4,4> perspectiveProjection(float fovY, float aspect, float nearZ, float farZ);
    FMatrix<4,4> orthographicProjection(float left, float right, float bottom, float top, float nearZ, float farZ);
    FMatrix<4,4> lookAt(const GPoint3D& eye, const GPoint3D& target, const GPoint3D& up);

    // ---- Viewport mapping ----

    FMatrix<4,4> viewportMatrix(const GEViewport& viewport);
    // Replaces the scattered translateCoordsDefaultImpl / per-vertex NDC conversion.

    // ---- Vector helpers ----

    template<class Ty, unsigned N>
    Ty dot(const Matrix<Ty,N,1>& a, const Matrix<Ty,N,1>& b);

    template<class Ty>
    Matrix<Ty,3,1> cross(const Matrix<Ty,3,1>& a, const Matrix<Ty,3,1>& b);

    template<class Ty, unsigned N>
    Ty length(const Matrix<Ty,N,1>& v);

    template<class Ty, unsigned N>
    Matrix<Ty,N,1> normalize(const Matrix<Ty,N,1>& v);
}
```

## Implementation Details

### `determinant` specializations

```
det(2x2): a*d - b*c                                    (4 ops)
det(3x3): cofactor expansion along first row            (12 ops)
det(4x4): Laplace expansion via 3x3 cofactors           (~40 ops)
```

### `inverse` specializations

```
inv(2x2): (1/det) * adjugate                            (6 ops)
inv(3x3): (1/det) * cofactor matrix transposed          (~30 ops)
inv(4x4): (1/det) * adjugate via 16 cofactors           (~100 ops)
```

All constexpr-friendly. No heap allocation.

### Matrix multiplication

Standard NxM * MxP triple loop:

```cpp
template<class Ty, unsigned C, unsigned R>
template<unsigned P>
Matrix<Ty, C, P> Matrix<Ty, C, R>::operator*(const Matrix<Ty, R, P>& other) const {
    auto result = Matrix<Ty, C, P>::Create();
    for(unsigned i = 0; i < C; i++){
        for(unsigned j = 0; j < P; j++){
            Ty sum = 0;
            for(unsigned k = 0; k < R; k++){
                sum += _data[i][k] * other._data[k][j];
            }
            result._data[i][j] = sum;
        }
    }
    return result;
}
```

Requires `Matrix<Ty,R,P>` to be a friend of `Matrix<Ty,C,R>` for `_data` access. Add:

```cpp
template<class, unsigned, unsigned> friend class Matrix;
```

### Transform matrix builders

Standard 4x4 affine matrices. Example for translation:

```cpp
FMatrix<4,4> translationMatrix(float x, float y, float z) {
    auto m = FMatrix<4,4>::Identity();
    m[3][0] = x;
    m[3][1] = y;
    m[3][2] = z;
    return m;
}
```

Column-major convention matching the GPU side. A point `(x,y,z,1)` is transformed by `M * p`.

### Replacing TE.cpp manual transforms

After this extension, the manual vertex-loop transforms in `TETriangulationResult::TEMesh` can be replaced:

**Before** (40 lines of manual trig in `rotate`):
```cpp
void TEMesh::rotate(float pitch, float yaw, float roll) {
    auto cos_pitch = cosf(pitch), sin_pitch = sinf(pitch);
    // ... 20 more lines of manual rotation per vertex
}
```

**After** (3 lines):
```cpp
void TEMesh::rotate(float pitch, float yaw, float roll) {
    auto rot = rotationEuler(pitch, yaw, roll);
    for(auto & p : vertexPolygons){
        p.a.pt = transformPoint(rot, p.a.pt);
        p.b.pt = transformPoint(rot, p.b.pt);
        p.c.pt = transformPoint(rot, p.c.pt);
    }
}
```

And transforms become composable:
```cpp
auto mvp = projection * view * rotationY(angle) * translationMatrix(x, y, z);
```

## File Plan

| File | Change |
|------|--------|
| `GTEBase.h` | Add operators, `transposed()`, `data()`, friend declaration to `Matrix` |
| **`GTEMath.h`** (new) | Free functions: `determinant`, `inverse`, transform builders, projection/view matrices, vector helpers |
| `TE.cpp` | Refactor `translate`/`rotate`/`scale` to use matrix operations (optional, non-breaking) |
| `CMakeLists.txt` | No change needed — header-only |

## Implementation Order

| Step | Description | Scope |
|------|-------------|-------|
| 1 | Add arithmetic operators and friend decl to `Matrix` | `GTEBase.h` |
| 2 | Add `transposed()` and `data()` | `GTEBase.h` |
| 3 | Create `GTEMath.h` with `determinant` (2x2, 3x3, 4x4) | New header |
| 4 | Add `inverse` (2x2, 3x3, 4x4) | `GTEMath.h` |
| 5 | Add transform builders (translation, scale, rotationX/Y/Z, euler) | `GTEMath.h` |
| 6 | Add projection/view matrices (perspective, ortho, lookAt) | `GTEMath.h` |
| 7 | Add vector helpers (dot, cross, length, normalize on FVec) | `GTEMath.h` |
| 8 | Refactor TE.cpp transforms to use matrices (optional) | `TE.cpp` |

Steps 1-2 are prerequisites. Steps 3-7 are independent. Step 8 is optional cleanup.

## Relationship to OmegaSL Matrix Extension

The OmegaSL matrix extension plan (`OmegaSL-Matrix-Extension-Plan.md`) covers the **shader-side** matrix types and operations. This plan covers the **CPU-side** `Matrix` class in `GTEBase.h`. Both use the same column-major convention and naming (`float4x4`), so data can be uploaded directly from `FMatrix<4,4>::data()` to GPU buffers without transposition.

## Not Included

- **Quaternions**: Useful for smooth rotations but not essential for the initial matrix extension. Can be added later as a `Quaternion<Ty>` class in `GTEMath.h`.
- **SIMD optimization**: The initial implementation uses scalar loops. SIMD intrinsics (`<arm_neon.h>` / `<immintrin.h>`) can optimize hot paths later without changing the API.
- **GLM replacement**: OmegaGTE already depends on GLM for the Vulkan backend. The `GTEMath.h` functions could eventually replace GLM usage, but this is not a goal of the initial extension.
