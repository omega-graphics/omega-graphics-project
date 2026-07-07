#ifndef AQUA_SRC_AQCOMPUTEUTIL_H
#define AQUA_SRC_AQCOMPUTEUTIL_H

// AQUA compute backend — shared host-side buffer marshalling helpers. These
// started life as anonymous-namespace helpers inside AQComputeBackend.cpp;
// the Phase 7f / Phase 6 GPU sub-phases split the backend across translation
// units (AQComputeXPBD.cpp, AQComputeParticles.cpp — the AQSpaceParticles.cpp
// second-TU pattern), so the helpers live here once instead of three times.
// Internal (src-only): OmegaGTE types are fine here, never in include/aqua/.

#include <omegaGTE/GE.h>          // GEBuffer
#include <omegaGTE/GTEShader.h>   // GEBufferWriter/Reader, omegaSLStructStride
#include <omegaGTE/GTEMath.h>     // FVec / UVec
#include <omegasl.h>              // OMEGASL_FLOAT4 / OMEGASL_UINT

#include <cstddef>
#include <cstdint>

// One float4 per record — the stride of every struct-of-float4 component
// group buffer (body pool, XPBD particle SoA, particle pool).
inline std::size_t aqF4Stride() {
    static const std::size_t stride = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4});
    return stride;
}

inline std::size_t aqU1Stride() {
    static const std::size_t stride = OmegaGTE::omegaSLStructStride({OMEGASL_UINT});
    return stride;
}

// Write `count` float4 records (x/y/z/w lane arrays) into `buffer`.
// A null lane pointer writes 0 in that lane.
inline bool aqWriteF4Buffer(SharedHandle<OmegaGTE::GEBuffer>& buffer,
                            const float* x, const float* y, const float* z, const float* w,
                            std::size_t count) {
    auto writer = OmegaGTE::GEBufferWriter::Create();
    writer->setOutputBuffer(buffer);
    for (std::size_t i = 0; i < count; ++i) {
        auto v = OmegaGTE::FVec<4>::Create();
        v[0][0] = x ? x[i] : 0.f;
        v[1][0] = y ? y[i] : 0.f;
        v[2][0] = z ? z[i] : 0.f;
        v[3][0] = w ? w[i] : 0.f;
        writer->structBegin();
        writer->writeFloat4(v);
        writer->structEnd();
        writer->sendToBuffer();
    }
    writer->flush();
    return true;
}

// Read `count` float4 records out of `buffer` into lane arrays (null lane
// pointers discard that lane).
inline bool aqReadF4Buffer(SharedHandle<OmegaGTE::GEBuffer>& buffer,
                           float* x, float* y, float* z, float* w,
                           std::size_t count) {
    auto reader = OmegaGTE::GEBufferReader::Create();
    reader->setInputBuffer(buffer);
    reader->setStructLayout({OMEGASL_FLOAT4});
    for (std::size_t i = 0; i < count; ++i) {
        reader->structBegin();
        auto v = OmegaGTE::FVec<4>::Create();
        reader->getFloat4(v);
        reader->structEnd();
        if (x) { x[i] = v[0][0]; }
        if (y) { y[i] = v[1][0]; }
        if (z) { z[i] = v[2][0]; }
        if (w) { w[i] = v[3][0]; }
    }
    return true;
}

// Write `count` plain uints into a buffer of AQU1 records.
inline bool aqWriteU1Buffer(SharedHandle<OmegaGTE::GEBuffer>& buffer,
                            const std::uint32_t* values, std::size_t count) {
    auto writer = OmegaGTE::GEBufferWriter::Create();
    writer->setOutputBuffer(buffer);
    for (std::size_t i = 0; i < count; ++i) {
        unsigned v = values ? values[i] : 0u;
        writer->structBegin();
        writer->writeUint(v);
        writer->structEnd();
        writer->sendToBuffer();
    }
    writer->flush();
    return true;
}

// Read `count` plain uints out of a buffer of AQU1 records.
inline bool aqReadU1Buffer(SharedHandle<OmegaGTE::GEBuffer>& buffer,
                           std::uint32_t* values, std::size_t count) {
    auto reader = OmegaGTE::GEBufferReader::Create();
    reader->setInputBuffer(buffer);
    reader->setStructLayout({OMEGASL_UINT});
    for (std::size_t i = 0; i < count; ++i) {
        reader->structBegin();
        reader->getUint(values[i]);
        reader->structEnd();
    }
    return true;
}

#endif // AQUA_SRC_AQCOMPUTEUTIL_H
