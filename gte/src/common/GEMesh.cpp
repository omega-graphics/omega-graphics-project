#include "omegaGTE/GEMesh.h"
#include "omegaGTE/GE.h"
#include "omegaGTE/GTEShader.h"

#include <iostream>

_NAMESPACE_BEGIN_

size_t geMeshStrideFor(uint32_t attributes) {
    size_t stride = 0;
    if (attributes & GEMeshAttrPosition) stride += sizeof(float) * 3;
    if (attributes & GEMeshAttrUV2)      stride += sizeof(float) * 2;
    if (attributes & GEMeshAttrUV3)      stride += sizeof(float) * 3;
    if (attributes & GEMeshAttrNormal)   stride += sizeof(float) * 3;
    if (attributes & GEMeshAttrColor)    stride += sizeof(float) * 4;
    return stride;
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
        for (auto &poly : result.mesh.vertexPolygons) {
            writeOneVertex(*writer, desc.attributes, poly.a.pt, poly.a.attachment, warnedMissingAttachment);
            writeOneVertex(*writer, desc.attributes, poly.b.pt, poly.b.attachment, warnedMissingAttachment);
            writeOneVertex(*writer, desc.attributes, poly.c.pt, poly.c.attachment, warnedMissingAttachment);
        }
        writer->flush();

        auto out = std::make_shared<GEMesh>();
        out->vertexBuffer = vbuf;
        out->vertexCount = totalVerts;
        out->vertexStride = stride;
        out->descriptor = desc;
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
    for (auto &v : indexed.vertices) {
        writeOneVertex(*vwriter, desc.attributes, v.pt, v.attachment, warnedMissingAttachment);
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
    if (diffuseTexture) {
        out->textureBindings[diffuseSlot] = std::move(diffuseTexture);
    }
    return out;
}

_NAMESPACE_END_
