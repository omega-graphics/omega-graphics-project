#include "RenderPass.h"

#include "Pipeline.h"
#include "RenderTarget.h"
#include "ResourceFactory.h"

namespace OmegaWTK::Composition {

    namespace {
        inline PipelineRegistry & pipelineRegistry(){
            return BackendResourceFactory::instance().pipelines();
        }
    }

    FrameRenderPass::FrameRenderPass(BackendRenderTargetContext & owner):
        owner_(owner)
    {}

    void FrameRenderPass::applyScratchViewportAndScissor(SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> & cb){
        OmegaGTE::GEViewport viewport {};
        viewport.x = 0;
        viewport.y = 0;
        viewport.nearDepth = 0.f;
        viewport.farDepth  = 1.f;
        viewport.width  = static_cast<float>(scratchWidth_);
        viewport.height = static_cast<float>(scratchHeight_);
        OmegaGTE::GEScissorRect scissorRect {
                viewport.x,
                viewport.y,
                viewport.width,
                viewport.height};
        cb->setViewports({viewport});
        cb->setScissorRects({scissorRect});
    }

    void FrameRenderPass::applyViewportAndScissor(SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> & cb){
        OmegaGTE::GEViewport viewport {};
        viewport.nearDepth = 0.f;
        viewport.farDepth  = 1.f;
        if(viewportOverride_.active){
            const float scale = owner_.renderScale();
            viewport.x      = viewportOverride_.offsetX * scale;
            viewport.y      = viewportOverride_.offsetY * scale;
            viewport.width  = viewportOverride_.width   * scale;
            viewport.height = viewportOverride_.height  * scale;
        }
        else {
            viewport.x      = 0;
            viewport.y      = 0;
            viewport.width  = static_cast<float>(owner_.getBackingWidth());
            viewport.height = static_cast<float>(owner_.getBackingHeight());
        }
        OmegaGTE::GEScissorRect scissorRect {
                viewport.x,
                viewport.y,
                viewport.width,
                viewport.height};
        cb->setViewports({viewport});
        cb->setScissorRects({scissorRect});
    }

    void FrameRenderPass::begin(float clearR, float clearG, float clearB, float clearA){
        auto & nativeTarget = owner_.getNativeRenderTarget();
        if(nativeTarget == nullptr){
            return;
        }
        frameCB_ = nativeTarget->commandBuffer();

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
        auto & nativeTarget = owner_.getNativeRenderTarget();
        frameCB_->endRenderPass();
        nativeTarget->submitCommandBuffer(frameCB_);
        frameCB_ = nullptr;
        frameActive_ = false;
    }

    FrameRenderPass::DrawScope
    FrameRenderPass::beginDraw(SharedHandle<OmegaGTE::GEFence> & textureFence){
        DrawScope scope {};

        // Scratch redirect: while a scratch pass is open, every draw goes to
        // the scratch CB. Texture-fence handling on the scratch CB matches
        // the in-frame mid-pass restart contract — but on the scratch target.
        if(scratchActive_ && scratchCB_ != nullptr){
            scope.cb = scratchCB_;
            if(textureFence != nullptr && scratchTarget_ != nullptr){
                scope.cb->endRenderPass();
                scratchTarget_->notifyCommandBuffer(scope.cb, textureFence);
                OmegaGTE::GERenderTarget::RenderPassDesc restartDesc {};
                restartDesc.colorAttachments.push_back(OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
                        OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor(0.f,0.f,0.f,0.f),
                        OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::LoadPreserve));
                restartDesc.depthStencilAttachment.disabled = true;
                scope.cb->startRenderPass(restartDesc);
                applyScratchViewportAndScissor(scope.cb);
                lastPipelineKind_ = PipelineKind::None;
            }
            return scope;
        }

        scope.cb = frameCB_;
        // Always-direct path: a draw outside an active frame is a contract
        // violation. Returning a null CB makes the caller short-circuit
        // rather than silently mis-recording onto a fresh standalone pass.
        if(scope.cb == nullptr){
            return scope;
        }

        // Texture fence: must be registered outside a render pass.
        if(textureFence != nullptr && frameActive_){
            // Mid-frame restart: end the active pass, register the wait, then
            // restart with LoadPreserve to keep prior content. Force a
            // pipeline rebind on the next bind* call.
            auto & nativeTarget = owner_.getNativeRenderTarget();
            scope.cb->endRenderPass();
            nativeTarget->notifyCommandBuffer(scope.cb, textureFence);
            OmegaGTE::GERenderTarget::RenderPassDesc restartDesc {};
            restartDesc.colorAttachments.push_back(OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
                    OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor(1.f,1.f,1.f,1.f),
                    OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::LoadPreserve));
            restartDesc.depthStencilAttachment.disabled = true;
            scope.cb->startRenderPass(restartDesc);
            applyViewportAndScissor(scope.cb);
            lastPipelineKind_ = PipelineKind::None;
        }
        return scope;
    }

    void FrameRenderPass::endDraw(DrawScope & /*scope*/){
        // No-op: the frame's CB is closed by `end()` and the scratch's by
        // `endScratchPass()`. Phase 4 retired the standalone-CB fallback,
        // so there is nothing to submit here.
    }

    void FrameRenderPass::bindColorPipeline(DrawScope & scope){
        if(lastPipelineKind_ != PipelineKind::Color){
            auto pipeline = pipelineRegistry().color();
            scope.cb->setRenderPipelineState(pipeline);
            lastPipelineKind_ = PipelineKind::Color;
        }
    }

    void FrameRenderPass::bindTexturePipeline(DrawScope & scope){
        if(lastPipelineKind_ != PipelineKind::Texture){
            auto pipeline = pipelineRegistry().texture();
            scope.cb->setRenderPipelineState(pipeline);
            lastPipelineKind_ = PipelineKind::Texture;
        }
    }

    void FrameRenderPass::bindSdfPipeline(DrawScope & scope){
        if(lastPipelineKind_ != PipelineKind::Sdf){
            auto pipeline = pipelineRegistry().sdf();
            scope.cb->setRenderPipelineState(pipeline);
            lastPipelineKind_ = PipelineKind::Sdf;
        }
    }

    void FrameRenderPass::bindPathPipeline(DrawScope & scope){
        if(lastPipelineKind_ != PipelineKind::Path){
            auto pipeline = pipelineRegistry().path();
            scope.cb->setRenderPipelineState(pipeline);
            lastPipelineKind_ = PipelineKind::Path;
        }
    }

    void FrameRenderPass::bindBitmapPipeline(DrawScope & scope){
        if(lastPipelineKind_ != PipelineKind::Bitmap){
            auto pipeline = pipelineRegistry().bitmap();
            scope.cb->setRenderPipelineState(pipeline);
            lastPipelineKind_ = PipelineKind::Bitmap;
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

    void FrameRenderPass::beginScratchPass(SharedHandle<OmegaGTE::GETextureRenderTarget> & scratchTarget,
                                           unsigned width, unsigned height){
        if(scratchTarget == nullptr || width == 0 || height == 0){
            return;
        }
        if(scratchActive_){
            // Nesting blurred layers is not modeled in Phase 2 — the
            // compositor walks slices flat. Drop the request rather than
            // silently corrupting state.
            return;
        }
        if(!frameActive_ || frameCB_ == nullptr){
            return;
        }

        // Suspend the frame's render pass on the native target. The CB is
        // submitted now; `resumeFrameAfterScratch()` acquires a fresh one
        // and restarts the pass with `LoadPreserve` so prior draws survive.
        auto & nativeTarget = owner_.getNativeRenderTarget();
        frameCB_->endRenderPass();
        nativeTarget->submitCommandBuffer(frameCB_);
        frameCB_ = nullptr;

        scratchTarget_ = scratchTarget;
        scratchWidth_  = width;
        scratchHeight_ = height;
        scratchCB_     = scratchTarget_->commandBuffer();

        OmegaGTE::GERenderTarget::RenderPassDesc desc {};
        desc.colorAttachments.push_back(OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor(0.f,0.f,0.f,0.f),
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::Clear));
        desc.depthStencilAttachment.disabled = true;
        scratchCB_->startRenderPass(desc);
        applyScratchViewportAndScissor(scratchCB_);

        scratchActive_ = true;
        // Force a pipeline rebind on the first draw inside the scratch pass.
        lastPipelineKind_ = PipelineKind::None;
    }

    void FrameRenderPass::endScratchPass(){
        if(!scratchActive_ || scratchCB_ == nullptr || scratchTarget_ == nullptr){
            return;
        }
        scratchCB_->endRenderPass();
        scratchTarget_->submitCommandBuffer(scratchCB_);
        scratchCB_.reset();
        scratchTarget_.reset();
        scratchWidth_  = 0;
        scratchHeight_ = 0;
        scratchActive_ = false;
        // Frame is left "suspended": frameActive_ is true but frameCB_ is
        // null. `resumeFrameAfterScratch()` rebuilds the CB.
        lastPipelineKind_ = PipelineKind::None;
    }

    void FrameRenderPass::resumeFrameAfterScratch(SharedHandle<OmegaGTE::GEFence> & textureFence){
        auto & nativeTarget = owner_.getNativeRenderTarget();
        if(!frameActive_ || nativeTarget == nullptr){
            return;
        }
        if(frameCB_ != nullptr){
            // Defensive: a caller already restarted the pass.
            return;
        }
        frameCB_ = nativeTarget->commandBuffer();
        if(textureFence != nullptr){
            // Wait must be registered outside a render pass.
            nativeTarget->notifyCommandBuffer(frameCB_, textureFence);
        }

        OmegaGTE::GERenderTarget::RenderPassDesc desc {};
        desc.colorAttachments.push_back(OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor(0.f,0.f,0.f,0.f),
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::LoadPreserve));
        desc.depthStencilAttachment.disabled = true;
        frameCB_->startRenderPass(desc);
        applyViewportAndScissor(frameCB_);

        lastPipelineKind_ = PipelineKind::None;
    }

}
