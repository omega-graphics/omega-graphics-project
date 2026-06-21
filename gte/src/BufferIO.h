#include <omegasl.h>
#include <omega-common/utils.h>
#include <cstddef>
#include <cstring>
#include <utility>
#include <initializer_list>

#ifndef OMEGAGTE_BUFFERIO_H_PRIV
#define OMEGAGTE_BUFFERIO_H_PRIV

namespace OmegaGTE {

    /// Forward decl: `FMatrix<C, R>` is defined in `omegaGTE/GTEBase.h`,
    /// which BufferIO.h cannot pull in directly without a circular include
    /// for the omegasl header. Backends that include both headers see the
    /// definition.
    template<class, unsigned, unsigned> class Matrix;
    template<unsigned c, unsigned r>
    using FMatrix = Matrix<float, c, r>;
    /// §12.2 follow-up — integer matrix aliases, mirroring GTEMath.h. Their
    /// byte layout is identical to the same-shape `FMatrix` (all three scalar
    /// families are 4 bytes), so they share the encode/decode helpers below.
    template<unsigned c, unsigned r>
    using IMatrix = Matrix<int, c, r>;
    template<unsigned c, unsigned r>
    using UMatrix = Matrix<unsigned int, c, r>;

    struct DataBlock {
        omegasl_data_type type;
        void *data;
    };

    /// §2.4 — buffer memory-layout standard. `Std430` is the storage-buffer
    /// (StructuredBuffer / SSBO / Metal-natural) layout used everywhere
    /// before constant buffers existed. `Std140` is the stricter uniform-
    /// buffer layout used by GLSL `uniform` blocks and HLSL `cbuffer`
    /// (column-major); its only divergence from std430 *for the flat
    /// GEBufferWriter API* (no arrays / nested structs) is that every matrix
    /// column is rounded up to a 16-byte stride and the struct is padded to
    /// a 16-byte multiple. Metal reads constant buffers with its natural
    /// (std430-equivalent) layout, so the Metal backend always passes
    /// `Std430` regardless of buffer role.
    ///
    /// `DXStructured` is native Direct3D `StructuredBuffer<T>` layout, used by
    /// the D3D12 backend for *storage* buffers. It is scalar-aligned: every
    /// scalar / vector / matrix aligns to its 4-byte component, NOT to its
    /// vector size — so a `float3`/`float4` aligns to 4 (size 12 / 16), and a
    /// `column_major floatCxR` matrix packs its columns tightly at `R*4` stride
    /// (e.g. `float3x3` = 36 bytes, not std430's 48). It deliberately does NOT
    /// apply the legacy cbuffer 16-byte column/struct padding — that rule is
    /// `Std140` (D3D12 `cbuffer` / Uniform buffers still use `Std140`). This
    /// matches what DXIL produces for `StructuredBuffer<T>` element access, so
    /// the host bytes line up with the shader's reads. (Metal/Vulkan keep
    /// `Std430`, which matches their GPU storage-buffer layouts.)
    enum class BufferLayoutStd { Std430, Std140, DXStructured };

    /// Column stride for an OmegaSL `floatCxR` matrix under the given
    /// standard. OmegaSL matrices are stored as C column vectors.
    ///   std430: the column's base alignment is rounded up to vec4 only when
    ///           the column is vec3 (the vec3 quirk); vec1/vec2/vec4 columns
    ///           pack tightly (4 / 8 / 16).
    ///   std140: every column is rounded up to 16 bytes.
    /// See OmegaSL-Feature-Gap-Survey §2.4 / §12.2.
    inline constexpr std::size_t matrixColumnStride(unsigned rows, BufferLayoutStd std) noexcept {
        if (std == BufferLayoutStd::Std140) return 16;
        // DX StructuredBuffer: columns pack tightly at R*4 — no vec3→16 pad
        // (float3x3 column stride is 12, not 16). See the enum doc.
        if (std == BufferLayoutStd::DXStructured) return static_cast<std::size_t>(rows) * 4u;
        if (rows == 1) return 4;
        if (rows == 2) return 8;
        return 16; // 3 (padded) or 4
    }

    /// Total size of an OmegaSL `floatCxR` matrix under the given standard.
    inline constexpr std::size_t matrixSize(unsigned cols, unsigned rows, BufferLayoutStd std) noexcept {
        return static_cast<std::size_t>(cols) * matrixColumnStride(rows, std);
    }

    /// A matrix's base alignment. For std430 / std140 it is the column
    /// alignment (same rule as the column stride). For DX StructuredBuffer it is
    /// the 4-byte component alignment — DX never aligns a matrix to its column
    /// size — so a matrix member only needs 4-byte placement.
    inline constexpr std::size_t matrixAlignment(unsigned rows, BufferLayoutStd std) noexcept {
        if (std == BufferLayoutStd::DXStructured) return 4;
        return matrixColumnStride(rows, std);
    }

    /// std430 wrappers — preserve the pre-§2.4 call sites verbatim.
    inline constexpr std::size_t std430MatrixColumnStride(unsigned rows) noexcept {
        return matrixColumnStride(rows, BufferLayoutStd::Std430);
    }
    inline constexpr std::size_t std430MatrixSize(unsigned cols, unsigned rows) noexcept {
        return matrixSize(cols, rows, BufferLayoutStd::Std430);
    }
    inline constexpr std::size_t std430MatrixAlignment(unsigned rows) noexcept {
        return matrixAlignment(rows, BufferLayoutStd::Std430);
    }

    /// Decompose an `omegasl_data_type` into (cols, rows). Returns
    /// {0, 0} for non-matrix types — callers can use that as a "this isn't
    /// a matrix" predicate.
    inline std::pair<unsigned, unsigned> matrixDims(omegasl_data_type t) noexcept {
        switch (t) {
            case OMEGASL_FLOAT2x1: return {2u, 1u};
            case OMEGASL_FLOAT2x2: return {2u, 2u};
            case OMEGASL_FLOAT2x3: return {2u, 3u};
            case OMEGASL_FLOAT2x4: return {2u, 4u};
            case OMEGASL_FLOAT3x1: return {3u, 1u};
            case OMEGASL_FLOAT3x2: return {3u, 2u};
            case OMEGASL_FLOAT3x3: return {3u, 3u};
            case OMEGASL_FLOAT3x4: return {3u, 4u};
            case OMEGASL_FLOAT4x1: return {4u, 1u};
            case OMEGASL_FLOAT4x2: return {4u, 2u};
            case OMEGASL_FLOAT4x3: return {4u, 3u};
            case OMEGASL_FLOAT4x4: return {4u, 4u};
            /// §12.2 follow-up — integer matrices share the float layout.
            case OMEGASL_INT2x2: case OMEGASL_UINT2x2: return {2u, 2u};
            case OMEGASL_INT2x3: case OMEGASL_UINT2x3: return {2u, 3u};
            case OMEGASL_INT2x4: case OMEGASL_UINT2x4: return {2u, 4u};
            case OMEGASL_INT3x2: case OMEGASL_UINT3x2: return {3u, 2u};
            case OMEGASL_INT3x3: case OMEGASL_UINT3x3: return {3u, 3u};
            case OMEGASL_INT3x4: case OMEGASL_UINT3x4: return {3u, 4u};
            case OMEGASL_INT4x2: case OMEGASL_UINT4x2: return {4u, 2u};
            case OMEGASL_INT4x3: case OMEGASL_UINT4x3: return {4u, 3u};
            case OMEGASL_INT4x4: case OMEGASL_UINT4x4: return {4u, 4u};
            default: return {0u, 0u};
        }
    }

    inline bool isMatrixDataType(omegasl_data_type t) noexcept {
        return matrixDims(t).first != 0u;
    }

    /// Copy a host-side column-major `Matrix<float, C, R>` (C * R
    /// contiguous floats — `_data[c][r]` at offset `c * R + r`) into a
    /// std430-laid-out destination, inserting per-column padding when
    /// the column stride is wider than the raw column bytes (the Cx3
    /// case). `dst` must have room for `std430MatrixSize(cols, rows)`
    /// bytes.
    inline void hostMatrixToStd430(const float *host, void *dst,
                                   unsigned cols, unsigned rows) noexcept {
        auto *out = static_cast<unsigned char *>(dst);
        const std::size_t colStride = std430MatrixColumnStride(rows);
        const std::size_t rawColBytes = static_cast<std::size_t>(rows) * sizeof(float);
        for (unsigned c = 0; c < cols; ++c) {
            std::memcpy(out + c * colStride, host + c * rows, rawColBytes);
            if (colStride > rawColBytes) {
                std::memset(out + c * colStride + rawColBytes, 0,
                            colStride - rawColBytes);
            }
        }
    }

    /// Inverse of `hostMatrixToStd430`. Reads `cols * std430MatrixColumnStride(rows)`
    /// bytes from `src` and writes `cols * rows` contiguous floats into
    /// `host`.
    inline void std430ToHostMatrix(const void *src, float *host,
                                   unsigned cols, unsigned rows) noexcept {
        const auto *in = static_cast<const unsigned char *>(src);
        const std::size_t colStride = std430MatrixColumnStride(rows);
        const std::size_t rawColBytes = static_cast<std::size_t>(rows) * sizeof(float);
        for (unsigned c = 0; c < cols; ++c) {
            std::memcpy(host + c * rows, in + c * colStride, rawColBytes);
        }
    }

    /// Pack a `Matrix<T, C, R>` into a freshly heap-allocated byte block
    /// (column-major) for the given standard. `T` must be a 4-byte scalar
    /// (`float` / `int` / `unsigned`) — the column stride / size helpers all
    /// assume a 4-byte element, which is what makes the int/uint matrix bytes
    /// identical to the same-shape float matrix (§12.2 follow-up). Caller owns
    /// the returned pointer and must `delete[]` it after the `sendToBuffer`
    /// memcpy. Backends share this so the column-padding rule lives in one
    /// place.
    template<class T, unsigned C, unsigned R>
    unsigned char *encodeMatrix(Matrix<T, C, R> &m, BufferLayoutStd std) {
        static_assert(sizeof(T) == 4, "matrix element must be a 4-byte scalar");
        const std::size_t sz = matrixSize(C, R, std);
        auto *bytes = new unsigned char[sz]{};
        const std::size_t colStride = matrixColumnStride(R, std);
        for (unsigned c = 0; c < C; ++c) {
            auto *col = bytes + c * colStride;
            for (unsigned r = 0; r < R; ++r) {
                T v = m[c][r];
                std::memcpy(col + r * sizeof(T), &v, sizeof(T));
            }
        }
        return bytes;
    }

    /// Inverse — read `matrixSize(C, R, std)` bytes from `src` and copy back
    /// into the host's tightly-packed `Matrix<T, C, R>`, dropping the
    /// per-column padding when present.
    template<class T, unsigned C, unsigned R>
    void decodeMatrix(const unsigned char *src, Matrix<T, C, R> &m, BufferLayoutStd std) {
        static_assert(sizeof(T) == 4, "matrix element must be a 4-byte scalar");
        const std::size_t colStride = matrixColumnStride(R, std);
        for (unsigned c = 0; c < C; ++c) {
            const auto *col = src + c * colStride;
            for (unsigned r = 0; r < R; ++r) {
                T v;
                std::memcpy(&v, col + r * sizeof(T), sizeof(T));
                m[c][r] = v;
            }
        }
    }

    /// Float-named wrappers — preserve the pre-§12.2 float call sites verbatim.
    template<unsigned C, unsigned R>
    unsigned char *encodeFMatrix(FMatrix<C, R> &m, BufferLayoutStd std) {
        return encodeMatrix<float, C, R>(m, std);
    }
    template<unsigned C, unsigned R>
    void decodeFMatrix(const unsigned char *src, FMatrix<C, R> &m, BufferLayoutStd std) {
        decodeMatrix<float, C, R>(src, m, std);
    }

    /// std430 wrappers — preserve the pre-§2.4 call sites verbatim.
    template<unsigned C, unsigned R>
    unsigned char *encodeFMatrixToStd430(FMatrix<C, R> &m) {
        return encodeMatrix<float, C, R>(m, BufferLayoutStd::Std430);
    }
    template<unsigned C, unsigned R>
    void decodeFMatrixFromStd430(const unsigned char *src, FMatrix<C, R> &m) {
        decodeMatrix<float, C, R>(src, m, BufferLayoutStd::Std430);
    }

    /// Non-matrix std140 base alignment / size for a scalar or vector type.
    /// std140 shares std430's scalar/vector rules: float/int/uint → (4,4),
    /// vec2 → (8,8), vec3 → (16,12), vec4 → (16,16). Returns {align, size}.
    inline std::pair<std::size_t, std::size_t> std140ScalarVec(omegasl_data_type d) noexcept {
        switch (d) {
            case OMEGASL_FLOAT: case OMEGASL_INT: case OMEGASL_UINT:   return {4, 4};
            case OMEGASL_FLOAT2: case OMEGASL_INT2: case OMEGASL_UINT2: return {8, 8};
            case OMEGASL_FLOAT3: case OMEGASL_INT3: case OMEGASL_UINT3: return {16, 12};
            case OMEGASL_FLOAT4: case OMEGASL_INT4: case OMEGASL_UINT4: return {16, 16};
            default: return {4, 4};
        }
    }

    /// §2.4-1 — per-member base alignment for the align-then-place buffer
    /// writers/readers. Alignment is *standard-driven and backend-agnostic*:
    /// scalar 4, vec2 8, vec3/vec4 16, matrix = its column alignment (std430:
    /// vecR alignment of a column; std140: always 16). The per-member *size*
    /// is NOT shared here — Metal packs vec3 as a 16-byte `simd_float3` while
    /// std430/std140 pack it as 12 — so each backend keeps its own size
    /// convention and only borrows this alignment rule.
    inline std::size_t memberBaseAlignment(omegasl_data_type d, BufferLayoutStd std) noexcept {
        // DX StructuredBuffer aligns *everything* to its 4-byte component — a
        // float2/float3/float4 or any matrix all need only 4-byte placement.
        if (std == BufferLayoutStd::DXStructured) {
            return 4;
        }
        if (isMatrixDataType(d)) {
            return matrixAlignment(matrixDims(d).second, std);
        }
        return std140ScalarVec(d).first; // align is identical in std430 / std140
    }

    /// Round `off` up to a multiple of `align` (a power of two in practice,
    /// but the modulo form is robust to any positive alignment).
    inline std::size_t alignOffset(std::size_t off, std::size_t align) noexcept {
        if (align != 0 && off % align != 0) {
            off += align - (off % align);
        }
        return off;
    }

    /// §2.4-1 — byte stride of a flat struct (the GEBufferWriter API exposes
    /// no arrays / nested structs) under the given standard. Standard
    /// align-then-place: each member is aligned to its base alignment, then the
    /// whole struct rounds up to its struct alignment — 16 for std140 (struct
    /// base alignment is always >= 16), the largest member alignment for
    /// std430. This is the *logical* layout (vec3 = 12 bytes), shared by the
    /// Vulkan / D3D12 backends and the unit test; Metal's `omegaSLStructStride`
    /// keeps its `simd_*`-native sizing (vec3 = 16) separately. `Iterable` is
    /// any range of `omegasl_data_type` (an `OmegaCommon::Vector` from the
    /// engine or a brace list from a test). Header-side and backend-independent
    /// so it is unit-testable on any platform.
    template<class Iterable>
    inline std::size_t structStride(const Iterable &fields, BufferLayoutStd std) noexcept {
        std::size_t off = 0, maxAlign = 1;
        for (auto d : fields) {
            // Alignment is standard-aware via memberBaseAlignment (DXStructured
            // collapses every member to 4-byte alignment); only the *size*
            // varies by type and is the same across std430 / DXStructured for
            // scalars/vectors (vec3 = 12, vec4 = 16) and follows the column
            // stride for matrices.
            const std::size_t align = memberBaseAlignment(d, std);
            std::size_t size;
            if (isMatrixDataType(d)) {
                auto dims = matrixDims(d);
                size = matrixSize(dims.first, dims.second, std);
            } else {
                size = std140ScalarVec(d).second; // scalar/vec size identical across stds
            }
            if (align > maxAlign) maxAlign = align;
            off = alignOffset(off, align);
            off += size;
        }
        const std::size_t structAlign = (std == BufferLayoutStd::Std140) ? 16 : maxAlign;
        return alignOffset(off, structAlign);
    }

    /// std140 / std430 flat-struct stride. `std140StructStride` preserves the
    /// pre-§2.4-1 call sites verbatim; both delegate to `structStride`.
    template<class Iterable>
    inline std::size_t std140StructStride(const Iterable &fields) noexcept {
        return structStride(fields, BufferLayoutStd::Std140);
    }
    template<class Iterable>
    inline std::size_t std430StructStride(const Iterable &fields) noexcept {
        return structStride(fields, BufferLayoutStd::Std430);
    }

    /// Brace-list overloads: `std140StructStride({OMEGASL_FLOAT4, ...})`.
    /// (Template argument deduction can't bind a braced literal to `Iterable`.)
    inline std::size_t std140StructStride(std::initializer_list<omegasl_data_type> fields) noexcept {
        return structStride<std::initializer_list<omegasl_data_type>>(fields, BufferLayoutStd::Std140);
    }
    inline std::size_t std430StructStride(std::initializer_list<omegasl_data_type> fields) noexcept {
        return structStride<std::initializer_list<omegasl_data_type>>(fields, BufferLayoutStd::Std430);
    }

    /// Native DirectX `StructuredBuffer<T>` flat-struct stride (scalar-aligned,
    /// no std430 column / struct 16-byte padding). Used by the D3D12 storage
    /// path; see the `BufferLayoutStd::DXStructured` enum doc.
    template<class Iterable>
    inline std::size_t dxStructuredStructStride(const Iterable &fields) noexcept {
        return structStride(fields, BufferLayoutStd::DXStructured);
    }
    inline std::size_t dxStructuredStructStride(std::initializer_list<omegasl_data_type> fields) noexcept {
        return structStride<std::initializer_list<omegasl_data_type>>(fields, BufferLayoutStd::DXStructured);
    }

    /// Map a (C, R) shape to the matching `omegasl_data_type`
    /// enumerator. Used by the per-backend matrix writers to tag the
    /// queued byte block so `sendToBuffer` can look up its size.
    template<unsigned C, unsigned R>
    constexpr omegasl_data_type matrixDataTypeFor() {
        if constexpr (C == 2 && R == 2) return OMEGASL_FLOAT2x2;
        else if constexpr (C == 2 && R == 3) return OMEGASL_FLOAT2x3;
        else if constexpr (C == 2 && R == 4) return OMEGASL_FLOAT2x4;
        else if constexpr (C == 3 && R == 2) return OMEGASL_FLOAT3x2;
        else if constexpr (C == 3 && R == 3) return OMEGASL_FLOAT3x3;
        else if constexpr (C == 3 && R == 4) return OMEGASL_FLOAT3x4;
        else if constexpr (C == 4 && R == 2) return OMEGASL_FLOAT4x2;
        else if constexpr (C == 4 && R == 3) return OMEGASL_FLOAT4x3;
        else if constexpr (C == 4 && R == 4) return OMEGASL_FLOAT4x4;
        else return OMEGASL_FLOAT;
    }

    /// §12.2 follow-up — (C, R) → integer matrix enumerator, mirroring the
    /// float helper above for the int/uint buffer writers.
    template<unsigned C, unsigned R>
    constexpr omegasl_data_type intMatrixDataTypeFor() {
        if constexpr (C == 2 && R == 2) return OMEGASL_INT2x2;
        else if constexpr (C == 2 && R == 3) return OMEGASL_INT2x3;
        else if constexpr (C == 2 && R == 4) return OMEGASL_INT2x4;
        else if constexpr (C == 3 && R == 2) return OMEGASL_INT3x2;
        else if constexpr (C == 3 && R == 3) return OMEGASL_INT3x3;
        else if constexpr (C == 3 && R == 4) return OMEGASL_INT3x4;
        else if constexpr (C == 4 && R == 2) return OMEGASL_INT4x2;
        else if constexpr (C == 4 && R == 3) return OMEGASL_INT4x3;
        else if constexpr (C == 4 && R == 4) return OMEGASL_INT4x4;
        else return OMEGASL_INT;
    }

    template<unsigned C, unsigned R>
    constexpr omegasl_data_type uintMatrixDataTypeFor() {
        if constexpr (C == 2 && R == 2) return OMEGASL_UINT2x2;
        else if constexpr (C == 2 && R == 3) return OMEGASL_UINT2x3;
        else if constexpr (C == 2 && R == 4) return OMEGASL_UINT2x4;
        else if constexpr (C == 3 && R == 2) return OMEGASL_UINT3x2;
        else if constexpr (C == 3 && R == 3) return OMEGASL_UINT3x3;
        else if constexpr (C == 3 && R == 4) return OMEGASL_UINT3x4;
        else if constexpr (C == 4 && R == 2) return OMEGASL_UINT4x2;
        else if constexpr (C == 4 && R == 3) return OMEGASL_UINT4x3;
        else if constexpr (C == 4 && R == 4) return OMEGASL_UINT4x4;
        else return OMEGASL_UINT;
    }
}

#endif
