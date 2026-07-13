#include "omegaGTE/GEMesh.h"
#include "omegaGTE/GE.h"
#include "omegaGTE/GTEShader.h"

#include <algorithm>
#include <iostream>

_NAMESPACE_BEGIN_

GPoint3D GEMeshBounds::center() const {
    GPoint3D c;
    c.x = (min.x + max.x) * 0.5f;
    c.y = (min.y + max.y) * 0.5f;
    c.z = (min.z + max.z) * 0.5f;
    return c;
}

GPoint3D GEMeshBounds::extent() const {
    GPoint3D e;
    e.x = max.x - min.x;
    e.y = max.y - min.y;
    e.z = max.z - min.z;
    return e;
}

float GEMeshBounds::longestExtent() const {
    const GPoint3D e = extent();
    return std::max(e.x, std::max(e.y, e.z));
}

GEMeshBounds geMeshComputeBounds(const float *packed,
                                 unsigned vertexCount,
                                 size_t vertexStride) {
    GEMeshBounds b;
    // A stride that cannot hold a float3 means the caller handed us a layout
    // without a position — there is nothing to bound, and reading three floats
    // anyway would walk off the end of the last vertex.
    if (packed == nullptr || vertexCount == 0 || vertexStride < sizeof(float) * 3) {
        return b;
    }

    const size_t floatsPerVertex = vertexStride / sizeof(float);
    b.min = GPoint3D{packed[0], packed[1], packed[2]};
    b.max = b.min;
    for (unsigned i = 1; i < vertexCount; ++i) {
        const float *p = packed + (static_cast<size_t>(i) * floatsPerVertex);
        b.min.x = std::min(b.min.x, p[0]);
        b.min.y = std::min(b.min.y, p[1]);
        b.min.z = std::min(b.min.z, p[2]);
        b.max.x = std::max(b.max.x, p[0]);
        b.max.y = std::max(b.max.y, p[1]);
        b.max.z = std::max(b.max.z, p[2]);
    }
    b.valid = true;
    return b;
}

namespace {

/// Grow `b` to contain `pt`. `valid` doubles as "has seen a point": the first
/// point seeds both corners, so an all-negative mesh is not silently stretched
/// to include the origin (which a zero-initialized box would do).
void expandBounds(GEMeshBounds &b, const GPoint3D &pt) {
    if (!b.valid) {
        b.min = pt;
        b.max = pt;
        b.valid = true;
        return;
    }
    b.min.x = std::min(b.min.x, pt.x);
    b.min.y = std::min(b.min.y, pt.y);
    b.min.z = std::min(b.min.z, pt.z);
    b.max.x = std::max(b.max.x, pt.x);
    b.max.y = std::max(b.max.y, pt.y);
    b.max.z = std::max(b.max.z, pt.z);
}

}  // namespace

namespace {

/// The mesh's attributes as OmegaSL field types, in the fixed order
/// `GEMeshVertexAttribute` documents. This is the bridge between GEMesh's
/// attribute bitmask and the buffer-layout authority in GTEShader.h — the one
/// place that knows what each backend's `buffer<T>` actually looks like.
OmegaCommon::Vector<omegasl_data_type> geMeshFieldsFor(uint32_t attributes) {
    OmegaCommon::Vector<omegasl_data_type> fields;
    if (attributes & GEMeshAttrPosition) fields.push_back(OMEGASL_FLOAT3);
    if (attributes & GEMeshAttrUV2)      fields.push_back(OMEGASL_FLOAT2);
    if (attributes & GEMeshAttrUV3)      fields.push_back(OMEGASL_FLOAT3);
    if (attributes & GEMeshAttrNormal)   fields.push_back(OMEGASL_FLOAT3);
    if (attributes & GEMeshAttrColor)    fields.push_back(OMEGASL_FLOAT4);
    return fields;
}

/// Component count of each field, parallel to geMeshFieldsFor().
OmegaCommon::Vector<size_t> geMeshFieldComponents(uint32_t attributes) {
    OmegaCommon::Vector<size_t> comps;
    if (attributes & GEMeshAttrPosition) comps.push_back(3);
    if (attributes & GEMeshAttrUV2)      comps.push_back(2);
    if (attributes & GEMeshAttrUV3)      comps.push_back(3);
    if (attributes & GEMeshAttrNormal)   comps.push_back(3);
    if (attributes & GEMeshAttrColor)    comps.push_back(4);
    return comps;
}

}  // namespace

size_t geMeshStrideFor(uint32_t attributes) {
    const auto fields = geMeshFieldsFor(attributes);
    if (fields.empty()) {
        return 0;
    }
    // Defer to the buffer-layout authority rather than summing component sizes.
    // A `buffer<T>` element is laid out by the backend's standard, and under
    // std430 a lone `float3` rounds up to a 16-byte stride — summing gives 12
    // and every vertex after the first is then read from the wrong offset.
    return omegaSLStructStride(fields, BufferDescriptor::Storage);
}

size_t geMeshTightStrideFor(uint32_t attributes) {
    size_t stride = 0;
    if (attributes & GEMeshAttrPosition) stride += sizeof(float) * 3;
    if (attributes & GEMeshAttrUV2)      stride += sizeof(float) * 2;
    if (attributes & GEMeshAttrUV3)      stride += sizeof(float) * 3;
    if (attributes & GEMeshAttrNormal)   stride += sizeof(float) * 3;
    if (attributes & GEMeshAttrColor)    stride += sizeof(float) * 4;
    return stride;
}

OmegaCommon::Vector<float> geMeshRepackToGPULayout(const OmegaCommon::Vector<float> & tightPacked,
                                                   uint32_t attributes,
                                                   unsigned vertexCount) {
    const size_t tightStride = geMeshTightStrideFor(attributes);
    const size_t gpuStride   = geMeshStrideFor(attributes);
    if (tightStride == 0 || gpuStride == 0 || vertexCount == 0) {
        return OmegaCommon::Vector<float>();
    }
    // The layouts already agree (D3D12's scalar StructuredBuffer, or any layout
    // that happens to need no padding) — nothing to move.
    if (tightStride == gpuStride) {
        return tightPacked;
    }

    const auto offsets = omegaSLStructMemberOffsets(geMeshFieldsFor(attributes),
                                                    BufferDescriptor::Storage);
    const auto comps   = geMeshFieldComponents(attributes);
    const size_t tightFloats = tightStride / sizeof(float);
    const size_t gpuFloats   = gpuStride / sizeof(float);

    // Zero-filled, so the pad bytes the standard inserts are deterministic
    // rather than whatever was on the heap — a NaN in padding is harmless today
    // but not worth leaving for someone to trip over.
    OmegaCommon::Vector<float> out(static_cast<size_t>(vertexCount) * gpuFloats, 0.f);
    for (unsigned v = 0; v < vertexCount; ++v) {
        const size_t src = static_cast<size_t>(v) * tightFloats;
        const size_t dst = static_cast<size_t>(v) * gpuFloats;
        size_t srcComp = 0;
        for (size_t f = 0; f < comps.size(); ++f) {
            const size_t dstComp = offsets[f] / sizeof(float);
            for (size_t c = 0; c < comps[f]; ++c) {
                out[dst + dstComp + c] = tightPacked[src + srcComp + c];
            }
            srcComp += comps[f];
        }
    }
    return out;
}

namespace {

void writeOneVertex(GEBufferWriter &w,
                    uint32_t attrs,
                    const GPoint3D &pt,
                    const std::optional<TETriangulationResult::AttachmentData> &att,
                    bool &warnedMissingAttachment) {
    w.structBegin();

    if (attrs & GEMeshAttrPosition) {
        FVec<3> v = FVec<3>::Create();
        v[0][0] = pt.x;
        v[1][0] = pt.y;
        v[2][0] = pt.z;
        w.writeFloat3(v);
    }

    const bool needAttachmentForExtras =
        (attrs & (GEMeshAttrUV2 | GEMeshAttrUV3 | GEMeshAttrNormal | GEMeshAttrColor)) != 0;
    if (needAttachmentForExtras && !att.has_value() && !warnedMissingAttachment) {
        std::cerr << "[GEMesh] warning: descriptor requests non-position attributes "
                     "but triangulation vertex has no AttachmentData; writing zeros."
                  << std::endl;
        warnedMissingAttachment = true;
    }

    if (attrs & GEMeshAttrUV2) {
        FVec<2> uv = FVec<2>::Create();
        if (att.has_value()) {
            uv = att->texture2Dcoord;
        }
        w.writeFloat2(uv);
    }
    if (attrs & GEMeshAttrUV3) {
        FVec<3> uv = FVec<3>::Create();
        if (att.has_value()) {
            uv = att->texture3Dcoord;
        }
        w.writeFloat3(uv);
    }
    if (attrs & GEMeshAttrNormal) {
        FVec<3> n = FVec<3>::Create();
        if (att.has_value()) {
            n = att->normal;
        }
        w.writeFloat3(n);
    }
    if (attrs & GEMeshAttrColor) {
        FVec<4> c = FVec<4>::Create();
        if (att.has_value()) {
            c = att->color;
        }
        w.writeFloat4(c);
    }

    w.structEnd();
    w.sendToBuffer();
}

}  // namespace

SharedHandle<GEMesh> buildMeshFromTriangulation(
    OmegaGraphicsEngine *engine,
    const TETriangulationResult &result,
    const GEMeshDescriptor &desc,
    SharedHandle<GETexture> diffuseTexture,
    unsigned diffuseSlot)
{
    if (engine == nullptr) {
        std::cerr << "[GEMesh] error: engine is null." << std::endl;
        return nullptr;
    }
    if ((desc.attributes & GEMeshAttrPosition) == 0) {
        std::cerr << "[GEMesh] error: descriptor must include Position." << std::endl;
        return nullptr;
    }
    if (desc.indexType == GEMeshIndexType::UInt16) {
        std::cerr << "[GEMesh] error: UInt16 index buffers are not supported by "
                     "the triangulation builder (no 16-bit GEBufferWriter path); "
                     "use UInt32." << std::endl;
        return nullptr;
    }
    if (desc.topology != GEMeshTopology::Triangle) {
        std::cerr << "[GEMesh] error: triangulation builder only supports "
                     "Triangle topology." << std::endl;
        return nullptr;
    }
    if (result.mesh.topology != TETriangulationResult::TEMesh::TopologyTriangle) {
        std::cerr << "[GEMesh] error: source TEMesh uses TriangleStrip; "
                     "the builder only consumes Triangle topology." << std::endl;
        return nullptr;
    }

    const size_t stride = geMeshStrideFor(desc.attributes);
    if (stride == 0) {
        std::cerr << "[GEMesh] error: empty vertex layout." << std::endl;
        return nullptr;
    }

    if (desc.indexType == GEMeshIndexType::None) {
        unsigned totalVerts = static_cast<unsigned>(result.mesh.vertexPolygons.size()) * 3u;
        if (totalVerts == 0) {
            std::cerr << "[GEMesh] warning: triangulation result has no polygons." << std::endl;
        }

        BufferDescriptor bdesc;
        bdesc.usage = BufferDescriptor::Upload;
        bdesc.len = static_cast<size_t>(totalVerts) * stride;
        bdesc.objectStride = stride;
        bdesc.opts = Shared;

        SharedHandle<GEBuffer> vbuf = engine->makeBuffer(bdesc);
        if (!vbuf) {
            std::cerr << "[GEMesh] error: makeBuffer failed." << std::endl;
            return nullptr;
        }

        auto writer = GEBufferWriter::Create();
        writer->setOutputBuffer(vbuf);

        bool warnedMissingAttachment = false;
        GEMeshBounds bounds;
        for (auto &poly : result.mesh.vertexPolygons) {
            writeOneVertex(*writer, desc.attributes, poly.a.pt, poly.a.attachment, warnedMissingAttachment);
            writeOneVertex(*writer, desc.attributes, poly.b.pt, poly.b.attachment, warnedMissingAttachment);
            writeOneVertex(*writer, desc.attributes, poly.c.pt, poly.c.attachment, warnedMissingAttachment);
            expandBounds(bounds, poly.a.pt);
            expandBounds(bounds, poly.b.pt);
            expandBounds(bounds, poly.c.pt);
        }
        writer->flush();

        auto out = std::make_shared<GEMesh>();
        out->vertexBuffer = vbuf;
        out->vertexCount = totalVerts;
        out->vertexStride = stride;
        out->descriptor = desc;
        out->bounds = bounds;
        if (diffuseTexture) {
            out->textureBindings[diffuseSlot] = std::move(diffuseTexture);
        }
        return out;
    }

    // desc.indexType == UInt32: consume the deduplicated representation.
    // The caller is responsible for calling TEMesh::buildIndexed() on the
    // source mesh first — this builder does not mutate the (const) result.
    if (!result.mesh.indexedData.has_value()) {
        std::cerr << "[GEMesh] error: indexed output requested but the source "
                     "TEMesh has no indexedData; call TEMesh::buildIndexed() "
                     "before building an indexed GEMesh." << std::endl;
        return nullptr;
    }
    const auto &indexed = *result.mesh.indexedData;

    unsigned totalVerts = static_cast<unsigned>(indexed.vertices.size());
    if (totalVerts == 0) {
        std::cerr << "[GEMesh] warning: indexed triangulation result has no vertices." << std::endl;
    }

    BufferDescriptor vbdesc;
    vbdesc.usage = BufferDescriptor::Upload;
    vbdesc.len = static_cast<size_t>(totalVerts) * stride;
    vbdesc.objectStride = stride;
    vbdesc.opts = Shared;

    SharedHandle<GEBuffer> vbuf = engine->makeBuffer(vbdesc);
    if (!vbuf) {
        std::cerr << "[GEMesh] error: makeBuffer failed (vertex buffer)." << std::endl;
        return nullptr;
    }

    auto vwriter = GEBufferWriter::Create();
    vwriter->setOutputBuffer(vbuf);
    bool warnedMissingAttachment = false;
    GEMeshBounds bounds;
    for (auto &v : indexed.vertices) {
        writeOneVertex(*vwriter, desc.attributes, v.pt, v.attachment, warnedMissingAttachment);
        expandBounds(bounds, v.pt);
    }
    vwriter->flush();

    unsigned indexCount = static_cast<unsigned>(indexed.indices.size());

    BufferDescriptor ibdesc;
    ibdesc.usage = BufferDescriptor::Upload;
    ibdesc.len = static_cast<size_t>(indexCount) * sizeof(uint32_t);
    ibdesc.objectStride = sizeof(uint32_t);
    ibdesc.opts = Shared;

    SharedHandle<GEBuffer> ibuf = engine->makeBuffer(ibdesc);
    if (!ibuf) {
        std::cerr << "[GEMesh] error: makeBuffer failed (index buffer)." << std::endl;
        return nullptr;
    }

    auto iwriter = GEBufferWriter::Create();
    iwriter->setOutputBuffer(ibuf);
    for (uint32_t idx : indexed.indices) {
        iwriter->structBegin();
        unsigned v = idx;
        iwriter->writeUint(v);
        iwriter->structEnd();
        iwriter->sendToBuffer();
    }
    iwriter->flush();

    auto out = std::make_shared<GEMesh>();
    out->vertexBuffer = vbuf;
    out->indexBuffer = ibuf;
    out->vertexCount = totalVerts;
    out->indexCount = indexCount;
    out->vertexStride = stride;
    out->descriptor = desc;
    out->bounds = bounds;
    if (diffuseTexture) {
        out->textureBindings[diffuseSlot] = std::move(diffuseTexture);
    }
    return out;
}

_NAMESPACE_END_
