#include <omegasl.h>
#include <omega-common/common.h>
#include <cstddef>
#include <cstring>
#include <utility>

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

    /// std430 column stride for an OmegaSL `floatCxR` matrix. OmegaSL
    /// matrices are stored as C column vectors, each of which gets its
    /// base alignment rounded up to vec4 when the column is vec3 (the
    /// std430 vec3 quirk). vec1/vec2/vec4 columns pack tightly.
    /// See OmegaSL-Feature-Gap-Survey §12.2.
    inline constexpr std::size_t std430MatrixColumnStride(unsigned rows) noexcept {
        if (rows == 1) return 4;
        if (rows == 2) return 8;
        return 16; // 3 (padded) or 4
    }

    /// Total std430 size of an OmegaSL `floatCxR` matrix.
    inline constexpr std::size_t std430MatrixSize(unsigned cols, unsigned rows) noexcept {
        return static_cast<std::size_t>(cols) * std430MatrixColumnStride(rows);
    }

    /// std430 base alignment for a matrix is the column alignment — same
    /// rule as the column stride.
    inline constexpr std::size_t std430MatrixAlignment(unsigned rows) noexcept {
        return std430MatrixColumnStride(rows);
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

    /// Pack an `FMatrix<C, R>` into a freshly heap-allocated std430 byte
    /// block (column-major, with per-column padding for `Cx3`). Caller
    /// owns the returned pointer and must `delete[]` it after the
    /// `sendToBuffer` memcpy. Backends share this so the std430 column
    /// padding rule lives in one place.
    template<unsigned C, unsigned R>
    unsigned char *encodeFMatrixToStd430(FMatrix<C, R> &m) {
        const std::size_t sz = std430MatrixSize(C, R);
        auto *bytes = new unsigned char[sz]{};
        const std::size_t colStride = std430MatrixColumnStride(R);
        for (unsigned c = 0; c < C; ++c) {
            auto *col = bytes + c * colStride;
            for (unsigned r = 0; r < R; ++r) {
                float f = m[c][r];
                std::memcpy(col + r * sizeof(float), &f, sizeof(float));
            }
        }
        return bytes;
    }

    /// Inverse — read `std430MatrixSize(C, R)` bytes from `src` and copy
    /// back into the host's tightly-packed `FMatrix`, dropping the per-
    /// column std430 padding when present.
    template<unsigned C, unsigned R>
    void decodeFMatrixFromStd430(const unsigned char *src, FMatrix<C, R> &m) {
        const std::size_t colStride = std430MatrixColumnStride(R);
        for (unsigned c = 0; c < C; ++c) {
            const auto *col = src + c * colStride;
            for (unsigned r = 0; r < R; ++r) {
                float f;
                std::memcpy(&f, col + r * sizeof(float), sizeof(float));
                m[c][r] = f;
            }
        }
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
