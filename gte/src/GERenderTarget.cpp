#include "omegaGTE/GERenderTarget.h"
#include "omegaGTE/GTEBase.h"
#include "omegaGTE/GECommandQueue.h"
#include "omegaGTE/GEMesh.h"

#include <iostream>

_NAMESPACE_BEGIN_

GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor::ClearColor(float r,float g,float b,float a):r(r),g(g),b(b),a(a){
    
};

GERenderTarget::RenderPassDesc::ColorAttachment::ColorAttachment(GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor clearColor,GERenderTarget::RenderPassDesc::ColorAttachment::LoadAction loadAction):clearColor(clearColor.r,clearColor.g,clearColor.b,clearColor.a),loadAction(loadAction),texture(nullptr){

};

GERenderTarget::RenderPassDesc::ColorAttachment::ColorAttachment(GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor clearColor,GERenderTarget::RenderPassDesc::ColorAttachment::LoadAction loadAction,SharedHandle<GETexture> texture):clearColor(clearColor.r,clearColor.g,clearColor.b,clearColor.a),loadAction(loadAction),texture(std::move(texture)){

};

GERenderTarget::CommandBuffer::CommandBuffer(GERenderTarget *renderTarget,GERTType type,SharedHandle<GECommandBuffer> commandBuffer):
renderTargetPtr(renderTarget),
commandBuffer(std::move(commandBuffer)),
renderTargetTy(type){
    
};

void *GERenderTarget::CommandBuffer::native() {
    return commandBuffer->native();
}

void GERenderTarget::CommandBuffer::setName(OmegaCommon::StrRef name) {
    commandBuffer->setName(name);
}

void GERenderTarget::CommandBuffer::startRenderPass(const GERenderTarget::RenderPassDesc & desc){
    GERenderPassDescriptor renderPassDesc;
    if(renderTargetTy == Native){
        renderPassDesc.nRenderTarget = (GENativeRenderTarget *)renderTargetPtr;
    }
    else if(renderTargetTy == Texture){
        renderPassDesc.tRenderTarget = (GETextureRenderTarget *)renderTargetPtr;
    };
    renderPassDesc.colorAttachments = desc.colorAttachments;

    commandBuffer->startRenderPass(renderPassDesc);
};

void GERenderTarget::CommandBuffer::setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState){
    commandBuffer->setRenderPipelineState(pipelineState);
};

void GERenderTarget::CommandBuffer::bindResourceAtVertexShader(SharedHandle<GEBuffer> & buffer,unsigned id){
    commandBuffer->bindResourceAtVertexShader(buffer,id);
};

void GERenderTarget::CommandBuffer::bindResourceAtVertexShader(SharedHandle<GETexture> & texture,unsigned id){
    commandBuffer->bindResourceAtVertexShader(texture,id);
};

void GERenderTarget::CommandBuffer::bindResourceAtFragmentShader(SharedHandle<GEBuffer> & buffer,unsigned id){
    commandBuffer->bindResourceAtFragmentShader(buffer,id);
};

void GERenderTarget::CommandBuffer::bindResourceAtFragmentShader(SharedHandle<GETexture> & texture,unsigned id){
    commandBuffer->bindResourceAtFragmentShader(texture,id);
};

void GERenderTarget::CommandBuffer::drawPolygons(PolygonType polygonType, unsigned vertexCount, size_t start){
    commandBuffer->drawPolygons(polygonType,vertexCount,start);
};

void GERenderTarget::CommandBuffer::setIndexBuffer(SharedHandle<GEBuffer> & buffer, IndexType indexType){
    commandBuffer->setIndexBuffer(buffer,indexType);
};

void GERenderTarget::CommandBuffer::drawIndexedPolygons(PolygonType polygonType, unsigned indexCount,
                                                        size_t startIndex, int baseVertex){
    commandBuffer->drawIndexedPolygons(polygonType,indexCount,startIndex,baseVertex);
};

void GERenderTarget::CommandBuffer::drawPolygonsInstanced(PolygonType polygonType, unsigned vertexCount,
                                                          size_t start, unsigned instanceCount,
                                                          unsigned firstInstance){
    commandBuffer->drawPolygonsInstanced(polygonType,vertexCount,start,instanceCount,firstInstance);
};

void GERenderTarget::CommandBuffer::drawIndexedPolygonsInstanced(PolygonType polygonType, unsigned indexCount,
                                                                 size_t startIndex, int baseVertex,
                                                                 unsigned instanceCount, unsigned firstInstance){
    commandBuffer->drawIndexedPolygonsInstanced(polygonType,indexCount,startIndex,baseVertex,instanceCount,firstInstance);
};

void GERenderTarget::CommandBuffer::drawPolygonsIndirect(PolygonType polygonType,
                                                         SharedHandle<GEBuffer> & argumentBuffer,
                                                         size_t argumentBufferOffset){
    commandBuffer->drawPolygonsIndirect(polygonType,argumentBuffer,argumentBufferOffset);
};

void GERenderTarget::CommandBuffer::drawIndexedPolygonsIndirect(PolygonType polygonType,
                                                                SharedHandle<GEBuffer> & argumentBuffer,
                                                                size_t argumentBufferOffset){
    commandBuffer->drawIndexedPolygonsIndirect(polygonType,argumentBuffer,argumentBufferOffset);
};

namespace {
    GERenderTarget::CommandBuffer::PolygonType meshPolygonType(GEMeshTopology t){
        switch(t){
            case GEMeshTopology::TriangleStrip: return GERenderTarget::CommandBuffer::TriangleStrip;
            case GEMeshTopology::Triangle:
            default:                            return GERenderTarget::CommandBuffer::Triangle;
        }
    }
}

void GERenderTarget::CommandBuffer::bindMesh(SharedHandle<GEMesh> & mesh, unsigned vertexSlot){
    if(!mesh){
        std::cerr << "[GEMesh] bindMesh: mesh is null." << std::endl;
        return;
    }
    if(mesh->vertexBuffer){
        commandBuffer->bindResourceAtVertexShader(mesh->vertexBuffer, vertexSlot);
    }
    if(mesh->indexBuffer && mesh->descriptor.indexType != GEMeshIndexType::None){
        IndexType it = (mesh->descriptor.indexType == GEMeshIndexType::UInt16)
                           ? IndexType::UInt16
                           : IndexType::UInt32;
        commandBuffer->setIndexBuffer(mesh->indexBuffer, it);
    }
    for(auto & entry : mesh->textureBindings){
        SharedHandle<GETexture> tex = entry.second;
        if(tex){
            commandBuffer->bindResourceAtFragmentShader(tex, entry.first);
        }
    }
};

void GERenderTarget::CommandBuffer::drawMesh(SharedHandle<GEMesh> & mesh, unsigned vertexSlot){
    if(!mesh){
        std::cerr << "[GEMesh] drawMesh: mesh is null." << std::endl;
        return;
    }
    bindMesh(mesh, vertexSlot);
    PolygonType pt = meshPolygonType(mesh->descriptor.topology);
    if(mesh->indexBuffer && mesh->descriptor.indexType != GEMeshIndexType::None){
        commandBuffer->drawIndexedPolygons(pt, mesh->indexCount, 0, 0);
    }
    else {
        commandBuffer->drawPolygons(pt, mesh->vertexCount, 0);
    }
};

void GERenderTarget::CommandBuffer::setViewports(std::vector<GEViewport> viewports) {
    commandBuffer->setViewports(viewports);
}

void GERenderTarget::CommandBuffer::setScissorRects(std::vector<GEScissorRect> scissorRects) {
    commandBuffer->setScissorRects(scissorRects);
}

void GERenderTarget::CommandBuffer::endRenderPass(){
    commandBuffer->finishRenderPass();
};

void GERenderTarget::CommandBuffer::startComputePass(SharedHandle<GEComputePipelineState> & pipelineState){
    GEComputePassDescriptor comp;
    commandBuffer->startComputePass(comp);
    commandBuffer->setComputePipelineState(pipelineState);
};

void GERenderTarget::CommandBuffer::endComputePass(){
    commandBuffer->finishComputePass();
};

void GERenderTarget::CommandBuffer::bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned int id) {
    commandBuffer->bindResourceAtComputeShader(buffer,id);
}

void GERenderTarget::CommandBuffer::bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int id) {
    commandBuffer->bindResourceAtComputeShader(texture,id);
}

void GERenderTarget::CommandBuffer::dispatchThreadgroups(unsigned int x, unsigned int y, unsigned int z) {
    commandBuffer->dispatchThreadgroups(x,y,z);
}

void GERenderTarget::CommandBuffer::dispatchThreads(unsigned int x, unsigned int y, unsigned int z) {
    commandBuffer->dispatchThreads(x,y,z);
}

void GERenderTarget::CommandBuffer::dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                                                 size_t argumentBufferOffset){
    commandBuffer->dispatchThreadgroupsIndirect(argumentBuffer,argumentBufferOffset);
}

void GERenderTarget::CommandBuffer::reset(){
    commandBuffer->reset();
};

void GERenderTarget::CommandBuffer::setCompletionHandler(GECommandBufferCompletionHandler handler){
    commandBuffer->setCompletionHandler(std::move(handler));
}





_NAMESPACE_END_
