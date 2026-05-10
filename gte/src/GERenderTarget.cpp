#include "omegaGTE/GERenderTarget.h"
#include "omegaGTE/GTEBase.h"
#include "omegaGTE/GECommandQueue.h"
#include "omegaGTE/GEMesh.h"

#include <iostream>

_NAMESPACE_BEGIN_

namespace {
    GECommandBuffer::PolygonType meshPolygonType(GEMeshTopology t){
        switch(t){
            case GEMeshTopology::TriangleStrip: return GECommandBuffer::TriangleStrip;
            case GEMeshTopology::Triangle:
            default:                            return GECommandBuffer::Triangle;
        }
    }
}

void GECommandBuffer::bindMesh(SharedHandle<GEMesh> & mesh, unsigned vertexSlot){
    if(!mesh){
        std::cerr << "[GEMesh] bindMesh: mesh is null." << std::endl;
        return;
    }
    if(mesh->vertexBuffer){
        bindResourceAtVertexShader(mesh->vertexBuffer, vertexSlot);
    }
    if(mesh->indexBuffer && mesh->descriptor.indexType != GEMeshIndexType::None){
        IndexType it = (mesh->descriptor.indexType == GEMeshIndexType::UInt16)
                           ? IndexType::UInt16
                           : IndexType::UInt32;
        setIndexBuffer(mesh->indexBuffer, it);
    }
    for(auto & entry : mesh->textureBindings){
        SharedHandle<GETexture> tex = entry.second;
        if(tex){
            bindResourceAtFragmentShader(tex, entry.first);
        }
    }
}

void GECommandBuffer::drawMesh(SharedHandle<GEMesh> & mesh, unsigned vertexSlot){
    if(!mesh){
        std::cerr << "[GEMesh] drawMesh: mesh is null." << std::endl;
        return;
    }
    bindMesh(mesh, vertexSlot);
    PolygonType pt = meshPolygonType(mesh->descriptor.topology);
    if(mesh->indexBuffer && mesh->descriptor.indexType != GEMeshIndexType::None){
        drawIndexedPolygons(pt, mesh->indexCount, 0, 0);
    }
    else {
        drawPolygons(pt, mesh->vertexCount, 0);
    }
}

_NAMESPACE_END_
