#include "RenderPass.h"

#include "Pipeline.h"
#include "RenderTarget.h"
#include "ResourceFactory.h"

#include "omegaGTE/GECommandQueue.h"

namespace OmegaWTK::Composition {

    namespace {
        inline PipelineRegistry & pipelineRegistry(){
            return BackendResourceFactory::instance().pipelines();
        }
    }

    FrameRenderPass::FrameRenderPass(BackendRenderTargetContext & owner):
        owner_(owner)
    {}

    void FrameRenderPass::applyScratchViewportAndScissor(SharedHandle<OmegaGTE::GECommandBuffer> & cb){
        OmegaGTE::GEViewport viewport {};
        viewport.nearDepth = 0.f;
        viewport.farDepth  = 1.f;
        OmegaGTE::GEScissorRect scissorRect {};
        if(captureActive_){
            // G.3.2-rev2 content-cache capture: window-sized viewport
            // offset by -(viewOrigin × scale) so the View's
            // window-coord ops land at the View-sized texture's origin;
            // scissor clips to the texture (View) bounds.
            viewport.x      = captureVpX_;
            viewport.y      = captureVpY_;
            viewport.width  = captureVpW_;
            viewport.height = captureVpH_;
            scissorRect.x      = 0;
            scissorRect.y      = 0;
            scissorRect.width  = captureScissorW_;
            scissorRect.height = captureScissorH_;
        }
        else {
            viewport.x      = 0;
            viewport.y      = 0;
            viewport.width  = static_cast<float>(scratchWidth_);
            viewport.height = static_cast<float>(scratchHeight_);
            scissorRect.x      = viewport.x;
            scissorRect.y      = viewport.y;
            scissorRect.width  = viewport.width;
            scissorRect.height = viewport.height;
        }
        cb->setViewports({viewport});
        cb->setScissorRects({scissorRect});
    }

    void FrameRenderPass::applyViewportAndScissor(SharedHandle<OmegaGTE::GECommandBuffer> & cb){
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
        auto & queue = owner_.commandQueue();
        if(nativeTarget == nullptr || queue == nullptr){
            return;
        }
        frameCB_ = queue->getAvailableBuffer();
        // Defense in depth: a backend can return null when it cannot hand
        // out a fresh buffer (e.g. D3D12 pool slot in a bad state). Skip
        // the frame rather than dereferencing null into startRenderPass.
        if(frameCB_ == nullptr){
            return;
        }

        OmegaGTE::GERenderPassDescriptor renderPassDesc {};
        renderPassDesc.nRenderTarget = nativeTarget.get();
        renderPassDesc.colorAttachments.push_back(OmegaGTE::GERenderPassDescriptor::ColorAttachment(
                OmegaGTE::GERenderPassDescriptor::ColorAttachment::ClearColor(clearR, clearG, clearB, clearA),
                OmegaGTE::GERenderPassDescriptor::ColorAttachment::Clear));
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
        auto & queue = owner_.commandQueue();
        frameCB_->finishRenderPass();
        if(queue != nullptr){
            queue->submitCommandBuffer(frameCB_);
        }
        frameCB_ = nullptr;
        frameActive_ = false;
    }

    FrameRenderPass::DrawScope
    FrameRenderPass::beginDraw(SharedHandle<OmegaGTE::GEFence> & textureFence){
        DrawScope scope {};
        auto & queue = owner_.commandQueue();

        // Scratch redirect: while a scratch pass is open, every draw goes to
        // the scratch CB. Texture-fence handling on the scratch CB matches
        // the in-frame mid-pass restart contract — but on the scratch target.
        if(scratchActive_ && scratchCB_ != nullptr){
            scope.cb = scratchCB_;
            if(textureFence != nullptr && scratchTarget_ != nullptr && queue != nullptr){
                scope.cb->finishRenderPass();
                queue->notifyCommandBuffer(scope.cb, textureFence);
                OmegaGTE::GERenderPassDescriptor restartDesc {};
                restartDesc.tRenderTarget = std::dynamic_pointer_cast<OmegaGTE::GETextureRenderTarget>(scratchTarget_).get();
                restartDesc.colorAttachments.push_back(OmegaGTE::GERenderPassDescriptor::ColorAttachment(
                        OmegaGTE::GERenderPassDescriptor::ColorAttachment::ClearColor(0.f,0.f,0.f,0.f),
                        OmegaGTE::GERenderPassDescriptor::ColorAttachment::LoadPreserve));
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
        if(textureFence != nullptr && frameActive_ && queue != nullptr){
            // Mid-frame restart: end the active pass, register the wait, then
            // restart with LoadPreserve to keep prior content. Force a
            // pipeline rebind on the next bind* call.
            auto & nativeTarget = owner_.getNativeRenderTarget();
            scope.cb->finishRenderPass();
            queue->notifyCommandBuffer(scope.cb, textureFence);
            OmegaGTE::GERenderPassDescriptor restartDesc {};
            restartDesc.nRenderTarget = nativeTarget.get();
            restartDesc.colorAttachments.push_back(OmegaGTE::GERenderPassDescriptor::ColorAttachment(
                    OmegaGTE::GERenderPassDescriptor::ColorAttachment::ClearColor(1.f,1.f,1.f,1.f),
                    OmegaGTE::GERenderPassDescriptor::ColorAttachment::LoadPreserve));
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

    void FrameRenderPass::bindTextPipeline(DrawScope & scope){
        if(lastPipelineKind_ != PipelineKind::Text){
            auto pipeline = pipelineRegistry().text();
            scope.cb->setRenderPipelineState(pipeline);
            lastPipelineKind_ = PipelineKind::Text;
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
        auto & queue = owner_.commandQueue();
        if(queue == nullptr){
            return;
        }

        // Suspend the frame's render pass on the native target. The CB is
        // submitted now; `resumeFrameAfterScratch()` acquires a fresh one
        // and restarts the pass with `LoadPreserve` so prior draws survive.
        frameCB_->finishRenderPass();
        queue->submitCommandBuffer(frameCB_);
        frameCB_ = nullptr;

        scratchTarget_ = scratchTarget;
        scratchWidth_  = width;
        scratchHeight_ = height;
        scratchCB_     = queue->getAvailableBuffer();

        OmegaGTE::GERenderPassDescriptor desc {};
        desc.tRenderTarget = scratchTarget.get();
        desc.colorAttachments.push_back(OmegaGTE::GERenderPassDescriptor::ColorAttachment(
                OmegaGTE::GERenderPassDescriptor::ColorAttachment::ClearColor(0.f,0.f,0.f,0.f),
                OmegaGTE::GERenderPassDescriptor::ColorAttachment::Clear));
        desc.depthStencilAttachment.disabled = true;
        scratchCB_->startRenderPass(desc);
        applyScratchViewportAndScissor(scratchCB_);

        scratchActive_ = true;
        // Force a pipeline rebind on the first draw inside the scratch pass.
        lastPipelineKind_ = PipelineKind::None;
    }

    void FrameRenderPass::beginCapturePass(SharedHandle<OmegaGTE::GETextureRenderTarget> & target,
                                           unsigned scissorW, unsigned scissorH,
                                           float vpX, float vpY, float vpW, float vpH){
        if(target == nullptr || scissorW == 0 || scissorH == 0){
            return;
        }
        if(scratchActive_){
            return;
        }
        if(!frameActive_ || frameCB_ == nullptr){
            return;
        }
        auto & queue = owner_.commandQueue();
        if(queue == nullptr){
            return;
        }

        // Arm capture mode BEFORE applyScratchViewportAndScissor so it
        // installs the offset viewport + texture-sized scissor.
        captureActive_   = true;
        captureVpX_      = vpX;
        captureVpY_      = vpY;
        captureVpW_      = vpW;
        captureVpH_      = vpH;
        captureScissorW_ = static_cast<float>(scissorW);
        captureScissorH_ = static_cast<float>(scissorH);

        // Suspend the frame's render pass (same as beginScratchPass).
        frameCB_->finishRenderPass();
        queue->submitCommandBuffer(frameCB_);
        frameCB_ = nullptr;

        scratchTarget_ = target;
        // scratchWidth_/Height_ are unused while captureActive_ (the
        // capture viewport/scissor override them), but set them to the
        // scissor size for any incidental reader.
        scratchWidth_  = scissorW;
        scratchHeight_ = scissorH;
        scratchCB_     = queue->getAvailableBuffer();

        OmegaGTE::GERenderPassDescriptor desc {};
        desc.tRenderTarget = target.get();
        desc.colorAttachments.push_back(OmegaGTE::GERenderPassDescriptor::ColorAttachment(
                OmegaGTE::GERenderPassDescriptor::ColorAttachment::ClearColor(0.f,0.f,0.f,0.f),
                OmegaGTE::GERenderPassDescriptor::ColorAttachment::Clear));
        desc.depthStencilAttachment.disabled = true;
        scratchCB_->startRenderPass(desc);
        applyScratchViewportAndScissor(scratchCB_);

        scratchActive_ = true;
        lastPipelineKind_ = PipelineKind::None;
    }

    void FrameRenderPass::endScratchPass(){
        if(!scratchActive_ || scratchCB_ == nullptr || scratchTarget_ == nullptr){
            return;
        }
        scratchCB_->finishRenderPass();
        auto & queue = owner_.commandQueue();
        if(queue != nullptr){
            queue->submitCommandBuffer(scratchCB_);
        }
        scratchCB_.reset();
        scratchTarget_.reset();
        scratchWidth_  = 0;
        scratchHeight_ = 0;
        scratchActive_ = false;
        // Disarm G.3.2-rev2 capture mode (no-op for a blur scratch).
        captureActive_   = false;
        captureVpX_      = 0.f;
        captureVpY_      = 0.f;
        captureVpW_      = 0.f;
        captureVpH_      = 0.f;
        captureScissorW_ = 0.f;
        captureScissorH_ = 0.f;
        // Frame is left "suspended": frameActive_ is true but frameCB_ is
        // null. `resumeFrameAfterScratch()` rebuilds the CB.
        lastPipelineKind_ = PipelineKind::None;
    }

    void FrameRenderPass::resumeFrameAfterScratch(SharedHandle<OmegaGTE::GEFence> & textureFence){
        auto & nativeTarget = owner_.getNativeRenderTarget();
        auto & queue = owner_.commandQueue();
        if(!frameActive_ || nativeTarget == nullptr || queue == nullptr){
            return;
        }
        if(frameCB_ != nullptr){
            // Defensive: a caller already restarted the pass.
            return;
        }
        frameCB_ = queue->getAvailableBuffer();
        if(textureFence != nullptr){
            // Wait must be registered outside a render pass.
            queue->notifyCommandBuffer(frameCB_, textureFence);
        }

        OmegaGTE::GERenderPassDescriptor desc {};
        desc.nRenderTarget = nativeTarget.get();
        desc.colorAttachments.push_back(OmegaGTE::GERenderPassDescriptor::ColorAttachment(
                OmegaGTE::GERenderPassDescriptor::ColorAttachment::ClearColor(0.f,0.f,0.f,0.f),
                OmegaGTE::GERenderPassDescriptor::ColorAttachment::LoadPreserve));
        desc.depthStencilAttachment.disabled = true;
        frameCB_->startRenderPass(desc);
        applyViewportAndScissor(frameCB_);

        lastPipelineKind_ = PipelineKind::None;
    }

}
