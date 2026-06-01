#include <kreate/Mesh.h>
#include "MeshFactory.h"
#include <omegaGTE/GE.h>
#include <omegaGTE/GEMesh.h>
#include <omegaGTE/GTEShader.h>
#include <omegasl.h>
#include <cstring>
#include <iostream>
#include <vector>

namespace Kreate {

struct Mesh::Impl {
    SharedHandle<OmegaGTE::GEMesh> geMesh;
};

Mesh::Mesh() : impl(std::make_unique<Impl>()) {}
Mesh::~Mesh() = default;

std::size_t vertexStrideFor(std::uint32_t attributes) {
    // Tightly-packed CPU stride — what the caller lays their vertex
    // buffer out as. The GPU-side stride may be larger after std430
    // padding (handled internally in MeshFactory::create).
    std::size_t stride = 0;
    if (attributes & VertexAttribute::Position) stride += sizeof(float) * 3;
    if (attributes & VertexAttribute::UV2)      stride += sizeof(float) * 2;
    if (attributes & VertexAttribute::UV3)      stride += sizeof(float) * 3;
    if (attributes & VertexAttribute::Normal)   stride += sizeof(float) * 3;
    if (attributes & VertexAttribute::Color)    stride += sizeof(float) * 4;
    return stride;
}

namespace {

OmegaGTE::GEMeshTopology mapTopology(MeshTopology t) {
    switch (t) {
        case MeshTopology::Triangle:      return OmegaGTE::GEMeshTopology::Triangle;
        case MeshTopology::TriangleStrip: return OmegaGTE::GEMeshTopology::TriangleStrip;
    }
    return OmegaGTE::GEMeshTopology::Triangle;
}

OmegaGTE::GEMeshIndexType mapIndexFormat(IndexFormat f) {
    switch (f) {
        case IndexFormat::None:   return OmegaGTE::GEMeshIndexType::None;
        case IndexFormat::UInt16: return OmegaGTE::GEMeshIndexType::UInt16;
        case IndexFormat::UInt32: return OmegaGTE::GEMeshIndexType::UInt32;
    }
    return OmegaGTE::GEMeshIndexType::None;
}

std::size_t indexElementSize(IndexFormat f) {
    switch (f) {
        case IndexFormat::None:   return 0;
        case IndexFormat::UInt16: return sizeof(std::uint16_t);
        case IndexFormat::UInt32: return sizeof(std::uint32_t);
    }
    return 0;
}

// Build the ordered list of omegasl_data_type fields for the attribute
// mask. Order matches the documented Position -> UV2 -> UV3 -> Normal ->
// Color contract.
OmegaCommon::Vector<omegasl_data_type> shaderFieldsFor(std::uint32_t attrs) {
    OmegaCommon::Vector<omegasl_data_type> fields;
    if (attrs & VertexAttribute::Position) fields.push_back(OMEGASL_FLOAT3);
    if (attrs & VertexAttribute::UV2)      fields.push_back(OMEGASL_FLOAT2);
    if (attrs & VertexAttribute::UV3)      fields.push_back(OMEGASL_FLOAT3);
    if (attrs & VertexAttribute::Normal)   fields.push_back(OMEGASL_FLOAT3);
    if (attrs & VertexAttribute::Color)    fields.push_back(OMEGASL_FLOAT4);
    return fields;
}

// Drive a `GEBufferWriter` to copy one vertex's attributes from
// tightly-packed CPU bytes into the writer's accumulator, in the order
// the shader struct declares them. The writer handles std430 padding
// between attributes; we just hand it typed floats.
void writeOneVertex(OmegaGTE::GEBufferWriter &w,
                    std::uint32_t attrs,
                    const float *cpuFloats) {
    using OmegaGTE::FVec;
    std::size_t i = 0;

    if (attrs & VertexAttribute::Position) {
        FVec<3> v = FVec<3>::Create();
        v[0][0] = cpuFloats[i++];
        v[1][0] = cpuFloats[i++];
        v[2][0] = cpuFloats[i++];
        w.writeFloat3(v);
    }
    if (attrs & VertexAttribute::UV2) {
        FVec<2> v = FVec<2>::Create();
        v[0][0] = cpuFloats[i++];
        v[1][0] = cpuFloats[i++];
        w.writeFloat2(v);
    }
    if (attrs & VertexAttribute::UV3) {
        FVec<3> v = FVec<3>::Create();
        v[0][0] = cpuFloats[i++];
        v[1][0] = cpuFloats[i++];
        v[2][0] = cpuFloats[i++];
        w.writeFloat3(v);
    }
    if (attrs & VertexAttribute::Normal) {
        FVec<3> v = FVec<3>::Create();
        v[0][0] = cpuFloats[i++];
        v[1][0] = cpuFloats[i++];
        v[2][0] = cpuFloats[i++];
        w.writeFloat3(v);
    }
    if (attrs & VertexAttribute::Color) {
        FVec<4> v = FVec<4>::Create();
        v[0][0] = cpuFloats[i++];
        v[1][0] = cpuFloats[i++];
        v[2][0] = cpuFloats[i++];
        v[3][0] = cpuFloats[i++];
        w.writeFloat4(v);
    }
}

} // namespace

std::shared_ptr<Mesh> MeshFactory::create(OmegaGTE::GTE &gte,
                                          const MeshDesc &desc,
                                          const void *vertexData,
                                          std::size_t vertexBytes,
                                          unsigned vertexCount,
                                          const void *indexData,
                                          std::size_t indexBytes,
                                          unsigned indexCount) {
    if ((desc.attributes & VertexAttribute::Position) == 0) {
        std::cerr << "Kreate::Mesh: descriptor must include Position\n";
        return nullptr;
    }
    if (!vertexData || vertexBytes == 0 || vertexCount == 0) {
        std::cerr << "Kreate::Mesh: vertexData/vertexBytes/vertexCount must be non-zero\n";
        return nullptr;
    }
    const std::size_t cpuStride = vertexStrideFor(desc.attributes);
    if (cpuStride == 0) {
        std::cerr << "Kreate::Mesh: empty vertex layout\n";
        return nullptr;
    }
    if (vertexBytes != static_cast<std::size_t>(vertexCount) * cpuStride) {
        std::cerr << "Kreate::Mesh: vertexBytes (" << vertexBytes
                  << ") != vertexCount (" << vertexCount
                  << ") * stride (" << cpuStride << ")\n";
        return nullptr;
    }

    const bool hasIndices = desc.indexFormat != IndexFormat::None;
    if (hasIndices) {
        if (!indexData || indexBytes == 0 || indexCount == 0) {
            std::cerr << "Kreate::Mesh: indexFormat is not None but indexData/indexBytes/indexCount are zero\n";
            return nullptr;
        }
        const std::size_t ielem = indexElementSize(desc.indexFormat);
        if (indexBytes != static_cast<std::size_t>(indexCount) * ielem) {
            std::cerr << "Kreate::Mesh: indexBytes (" << indexBytes
                      << ") != indexCount (" << indexCount
                      << ") * element size (" << ielem << ")\n";
            return nullptr;
        }
    } else if (indexData || indexBytes != 0 || indexCount != 0) {
        std::cerr << "Kreate::Mesh: indexFormat == None but indexData/indexBytes/indexCount are non-zero\n";
        return nullptr;
    }

    auto *engine = gte.graphicsEngine.get();
    if (!engine) {
        std::cerr << "Kreate::Mesh: GTE has no graphicsEngine\n";
        return nullptr;
    }

    // GPU-side vertex stride is the std430 layout of the shader struct;
    // typically larger than the tight CPU stride (a vec3 followed by a
    // vec4 needs 32 B, not 28). `omegaSLStructStride` is the same helper
    // every backend uses to size `buffer<T>` accesses.
    auto fields = shaderFieldsFor(desc.attributes);
    const std::size_t gpuStride = OmegaGTE::omegaSLStructStride(fields,
        OmegaGTE::BufferDescriptor::Storage);

    OmegaGTE::BufferDescriptor vdesc;
    vdesc.usage        = OmegaGTE::BufferDescriptor::Upload;
    vdesc.len          = static_cast<std::size_t>(vertexCount) * gpuStride;
    vdesc.objectStride = gpuStride;
    vdesc.opts         = OmegaGTE::Shared;
    vdesc.role         = OmegaGTE::BufferDescriptor::Storage;

    auto vbuf = engine->makeBuffer(vdesc);
    if (!vbuf) {
        std::cerr << "Kreate::Mesh: vertex buffer allocation failed\n";
        return nullptr;
    }

    auto writer = OmegaGTE::GEBufferWriter::Create();
    writer->setOutputBuffer(vbuf);
    const float *cursor = static_cast<const float *>(vertexData);
    const std::size_t floatsPerVertex = cpuStride / sizeof(float);
    for (unsigned i = 0; i < vertexCount; ++i) {
        writer->structBegin();
        writeOneVertex(*writer, desc.attributes, cursor);
        writer->structEnd();
        writer->sendToBuffer();
        cursor += floatsPerVertex;
    }
    writer->flush();

    SharedHandle<OmegaGTE::GEBuffer> ibuf;
    if (hasIndices) {
        const std::size_t ielem = indexElementSize(desc.indexFormat);
        OmegaGTE::BufferDescriptor idesc;
        idesc.usage        = OmegaGTE::BufferDescriptor::Upload;
        idesc.len          = indexBytes;
        idesc.objectStride = ielem;
        idesc.opts         = OmegaGTE::Shared;
        idesc.role         = OmegaGTE::BufferDescriptor::Storage;

        ibuf = engine->makeBuffer(idesc);
        if (!ibuf) {
            std::cerr << "Kreate::Mesh: index buffer allocation failed\n";
            return nullptr;
        }

        // Index data is opaque uint16 / uint32 values; the writer goes
        // through `writeUint` so the cursor advancement matches the
        // backend's index-buffer expectations (4-byte alignment for
        // uint32; uint16 packs two-per-uint via the same path is
        // unsafe, so 16-bit indices need their own path — fall back to
        // writeUint for uint32 only here, and treat uint16 as a TODO).
        auto iwriter = OmegaGTE::GEBufferWriter::Create();
        iwriter->setOutputBuffer(ibuf);
        if (desc.indexFormat == IndexFormat::UInt32) {
            const std::uint32_t *idxs = static_cast<const std::uint32_t *>(indexData);
            for (unsigned i = 0; i < indexCount; ++i) {
                iwriter->structBegin();
                unsigned v = idxs[i];
                iwriter->writeUint(v);
                iwriter->structEnd();
                iwriter->sendToBuffer();
            }
        } else {
            // UInt16 indices are not yet wired through GEBufferWriter —
            // the writer is 4-byte-scalar oriented. Reject so the caller
            // sees a loud failure instead of a silently-empty index buffer.
            std::cerr << "Kreate::Mesh: UInt16 index format is not yet implemented; use UInt32 for Phase 1\n";
            return nullptr;
        }
        iwriter->flush();
    }

    auto ge = std::make_shared<OmegaGTE::GEMesh>();
    ge->vertexBuffer = std::move(vbuf);
    ge->indexBuffer  = std::move(ibuf);
    ge->vertexCount  = vertexCount;
    ge->indexCount   = indexCount;
    ge->vertexStride = gpuStride;
    ge->descriptor.attributes = desc.attributes;
    ge->descriptor.topology   = mapTopology(desc.topology);
    ge->descriptor.indexType  = mapIndexFormat(desc.indexFormat);

    auto m = std::shared_ptr<Mesh>(new Mesh());
    m->impl->geMesh = std::move(ge);
    return m;
}

SharedHandle<OmegaGTE::GEMesh> &MeshFactory::geMesh(Mesh &m) {
    return m.impl->geMesh;
}

} // namespace Kreate
