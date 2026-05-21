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
    enum class BufferLayoutStd { Std430, Std140 };

    /// Column stride for an OmegaSL `floatCxR` matrix under the given
    /// standard. OmegaSL matrices are stored as C column vectors.
    ///   std430: the column's base alignment is rounded up to vec4 only when
    ///           the column is vec3 (the vec3 quirk); vec1/vec2/vec4 columns
    ///           pack tightly (4 / 8 / 16).
    ///   std140: every column is rounded up to 16 bytes.
    /// See OmegaSL-Feature-Gap-Survey §2.4 / §12.2.
    inline constexpr std::size_t matrixColumnStride(unsigned rows, BufferLayoutStd std) noexcept {
        if (std == BufferLayoutStd::Std140) return 16;
        if (rows == 1) return 4;
        if (rows == 2) return 8;
        return 16; // 3 (padded) or 4
    }

    /// Total size of an OmegaSL `floatCxR` matrix under the given standard.
    inline constexpr std::size_t matrixSize(unsigned cols, unsigned rows, BufferLayoutStd std) noexcept {
        return static_cast<std::size_t>(cols) * matrixColumnStride(rows, std);
    }

    /// A matrix's base alignment is its column alignment — same rule as the
    /// column stride.
    inline constexpr std::size_t matrixAlignment(unsigned rows, BufferLayoutStd std) noexcept {
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

    /// Pack an `FMatrix<C, R>` into a freshly heap-allocated byte block
    /// (column-major) for the given standard. Caller owns the returned
    /// pointer and must `delete[]` it after the `sendToBuffer` memcpy.
    /// Backends share this so the column-padding rule lives in one place.
    template<unsigned C, unsigned R>
    unsigned char *encodeFMatrix(FMatrix<C, R> &m, BufferLayoutStd std) {
        const std::size_t sz = matrixSize(C, R, std);
        auto *bytes = new unsigned char[sz]{};
        const std::size_t colStride = matrixColumnStride(R, std);
        for (unsigned c = 0; c < C; ++c) {
            auto *col = bytes + c * colStride;
            for (unsigned r = 0; r < R; ++r) {
                float f = m[c][r];
                std::memcpy(col + r * sizeof(float), &f, sizeof(float));
            }
        }
        return bytes;
    }

    /// Inverse — read `matrixSize(C, R, std)` bytes from `src` and copy back
    /// into the host's tightly-packed `FMatrix`, dropping the per-column
    /// padding when present.
    template<unsigned C, unsigned R>
    void decodeFMatrix(const unsigned char *src, FMatrix<C, R> &m, BufferLayoutStd std) {
        const std::size_t colStride = matrixColumnStride(R, std);
        for (unsigned c = 0; c < C; ++c) {
            const auto *col = src + c * colStride;
            for (unsigned r = 0; r < R; ++r) {
                float f;
                std::memcpy(&f, col + r * sizeof(float), sizeof(float));
                m[c][r] = f;
            }
        }
    }

    /// std430 wrappers — preserve the pre-§2.4 call sites verbatim.
    template<unsigned C, unsigned R>
    unsigned char *encodeFMatrixToStd430(FMatrix<C, R> &m) {
        return encodeFMatrix<C, R>(m, BufferLayoutStd::Std430);
    }
    template<unsigned C, unsigned R>
    void decodeFMatrixFromStd430(const unsigned char *src, FMatrix<C, R> &m) {
        decodeFMatrix<C, R>(src, m, BufferLayoutStd::Std430);
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

    /// std140 byte stride of a flat struct (the GEBufferWriter API exposes no
    /// arrays / nested structs). Standard align-then-place: each member is
    /// aligned to its std140 base alignment, then the whole struct rounds up
    /// to a 16-byte multiple (std140 struct base alignment is always >= 16).
    /// Matrices are treated as C columns of 16-byte stride. `Iterable` is any
    /// range of `omegasl_data_type` (an `OmegaCommon::Vector` from the engine
    /// or a brace list from a test). Header-side and backend-independent so it
    /// is unit-testable on any platform — including Metal builds, where
    /// `omegaSLStructStride` itself never takes the std140 path.
    template<class Iterable>
    inline std::size_t std140StructStride(const Iterable &fields) noexcept {
        std::size_t off = 0;
        for (auto d : fields) {
            std::size_t align, size;
            if (isMatrixDataType(d)) {
                auto dims = matrixDims(d);
                align = matrixAlignment(dims.second, BufferLayoutStd::Std140); // 16
                size  = matrixSize(dims.first, dims.second, BufferLayoutStd::Std140); // C * 16
            } else {
                auto av = std140ScalarVec(d);
                align = av.first;
                size  = av.second;
            }
            if (off % align != 0) {
                off += align - (off % align);
            }
            off += size;
        }
        if (off % 16 != 0) {
            off += 16 - (off % 16);
        }
        return off;
    }

    /// Brace-list overload: `std140StructStride({OMEGASL_FLOAT4, ...})`.
    /// (Template argument deduction can't bind a braced literal to `Iterable`.)
    inline std::size_t std140StructStride(std::initializer_list<omegasl_data_type> fields) noexcept {
        return std140StructStride<std::initializer_list<omegasl_data_type>>(fields);
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
}

#endif
