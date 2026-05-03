#include "RenderPass.h"

#include "Pipeline.h"
#include "ResourceFactory.h"
#include "Texture.h"

namespace OmegaWTK::Composition {

    namespace {
        inline PipelineRegistry & pipelineRegistry(){
            return BackendResourceFactory::instance().pipelines();
        }
    }

    FrameRenderPass::FrameRenderPass(BackingTextureSet & textures,
                                     SharedHandle<OmegaGTE::GENativeRenderTarget> & nativeTarget):
        textures_(textures),
        nativeTarget_(nativeTarget)
    {}

    void FrameRenderPass::applyViewportAndScissor(SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> & cb){
        OmegaGTE::GEViewport viewport {};
        viewport.nearDepth = 0.f;
        viewport.farDepth  = 1.f;
        if(viewportOverride_.active){
            const float scale = textures_.renderScale();
            viewport.x      = viewportOverride_.offsetX * scale;
            viewport.y      = viewportOverride_.offsetY * scale;
            viewport.width  = viewportOverride_.width   * scale;
            viewport.height = viewportOverride_.height  * scale;
        }
        else {
            viewport.x      = 0;
            viewport.y      = 0;
            viewport.width  = static_cast<float>(textures_.backingWidth());
            viewport.height = static_cast<float>(textures_.backingHeight());
        }
        OmegaGTE::GEScissorRect scissorRect {
                viewport.x,
                viewport.y,
                viewport.width,
                viewport.height};
        cb->setViewports({viewport});
        cb->setScissorRects({scissorRect});
    }

    void FrameRenderPass::startStandalonePass(SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> & cb){
        OmegaGTE::GERenderTarget::RenderPassDesc renderPassDesc {};
        renderPassDesc.colorAttachments.push_back(OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor(1.f,1.f,1.f,1.f),
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::LoadPreserve));
        renderPassDesc.depthStencilAttachment.disabled = true;
        cb->startRenderPass(renderPassDesc);
        applyViewportAndScissor(cb);
    }

    void FrameRenderPass::clearOnce(float r, float g, float b, float a){
        if(nativeTarget_ == nullptr){
            return;
        }
        auto cb = nativeTarget_->commandBuffer();

        OmegaGTE::GERenderTarget::RenderPassDesc renderPassDesc {};
        renderPassDesc.colorAttachments.push_back(OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor(r,g,b,a),
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::Clear));
        renderPassDesc.depthStencilAttachment.disabled = true;
        cb->startRenderPass(renderPassDesc);
        cb->endRenderPass();
        nativeTarget_->submitCommandBuffer(cb);
    }

    void FrameRenderPass::begin(float clearR, float clearG, float clearB, float clearA){
        if(nativeTarget_ == nullptr){
            return;
        }
        frameCB_ = nativeTarget_->commandBuffer();

        OmegaGTE::GERenderTarget::RenderPassDesc renderPassDesc {};
        renderPassDesc.colorAttachments.push_back(OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor(clearR, clearG, clearB, clearA),
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::Clear));
        renderPassDesc.depthStencilAttachment.disabled = true;
        frameCB_->startRenderPass(renderPassDesc);

        applyViewportAndScissor(frameCB_);

        frameActive_      = true;
        lastPipelineKind_ = PipelineKind::None;  // first draw must rebind
    }

    void FrameRenderPass::end(){
        if(!frameActive_ || frameCB_ == nullptr){
            return;
        }
        frameCB_->endRenderPass();
        nativeTarget_->submitCommandBuffer(frameCB_);
        frameCB_ = nullptr;
        frameActive_ = false;
    }

    FrameRenderPass::DrawScope
    FrameRenderPass::beginDraw(SharedHandle<OmegaGTE::GEFence> & textureFence){
        DrawScope scope {};
        scope.cb = frameCB_;
        if(scope.cb == nullptr){
            // Fallback: not inside a beginFrame/endFrame pair. Unreachable on
            // the always-direct path; retained until Phase 4 cleanup.
            if(nativeTarget_ == nullptr){
                return scope;
            }
            scope.cb = nativeTarget_->commandBuffer();
        }
        scope.standalone = (scope.cb != frameCB_);

        // Texture fence: must be registered outside a render pass.
        if(textureFence != nullptr && frameActive_){
            // Mid-frame restart: end the active pass, register the wait, then
            // restart with LoadPreserve to keep prior content. Force a
            // pipeline rebind on the next bind* call.
            scope.cb->endRenderPass();
            nativeTarget_->notifyCommandBuffer(scope.cb, textureFence);
            OmegaGTE::GERenderTarget::RenderPassDesc restartDesc {};
            restartDesc.colorAttachments.push_back(OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
                    OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor(1.f,1.f,1.f,1.f),
                    OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::LoadPreserve));
            restartDesc.depthStencilAttachment.disabled = true;
            scope.cb->startRenderPass(restartDesc);
            applyViewportAndScissor(scope.cb);
            lastPipelineKind_ = PipelineKind::None;
        }
        else if(textureFence != nullptr && scope.standalone){
            nativeTarget_->notifyCommandBuffer(scope.cb, textureFence);
        }

        if(scope.standalone){
            startStandalonePass(scope.cb);
        }
        return scope;
    }

    void FrameRenderPass::endDraw(DrawScope & scope){
        if(scope.standalone && scope.cb != nullptr){
            scope.cb->endRenderPass();
            nativeTarget_->submitCommandBuffer(scope.cb);
        }
    }

    void FrameRenderPass::bindColorPipeline(DrawScope & scope){
        if(scope.standalone || lastPipelineKind_ != PipelineKind::Color){
            auto pipeline = pipelineRegistry().color();
            scope.cb->setRenderPipelineState(pipeline);
            lastPipelineKind_ = PipelineKind::Color;
        }
    }

    void FrameRenderPass::bindTexturePipeline(DrawScope & scope){
        if(scope.standalone || lastPipelineKind_ != PipelineKind::Texture){
            auto pipeline = pipelineRegistry().texture();
            scope.cb->setRenderPipelineState(pipeline);
            lastPipelineKind_ = PipelineKind::Texture;
        }
    }

    void FrameRenderPass::setViewportOverride(float offsetX, float offsetY,
                                              float width, float height){
        viewportOverride_.active   = true;
        viewportOverride_.offsetX  = offsetX;
        viewportOverride_.offsetY  = offsetY;
        viewportOverride_.width    = width;
        viewportOverride_.height   = height;
    }

    void FrameRenderPass::clearViewportOverride(){
        viewportOverride_.active = false;
    }

}
