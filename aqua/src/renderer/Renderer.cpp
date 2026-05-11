#include "Renderer.h"

namespace Aqua {

Renderer::Renderer(SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget,
                   SharedHandle<OmegaGTE::GECommandQueue> queue)
    : renderTarget_(std::move(renderTarget)), queue_(std::move(queue)) {}

Renderer::~Renderer() = default;

void Renderer::beginFrame(Color clearColor) {
    currentBuffer_ = queue_->getAvailableBuffer();

    OmegaGTE::GERenderPassDescriptor passDesc{};
    passDesc.nRenderTarget = renderTarget_.get();
    passDesc.colorAttachments.push_back(
        OmegaGTE::GERenderPassDescriptor::ColorAttachment(
            {clearColor.r, clearColor.g, clearColor.b, clearColor.a},
            OmegaGTE::GERenderPassDescriptor::ColorAttachment::Clear));

    currentBuffer_->startRenderPass(passDesc);
}

void Renderer::endFrameAndPresent() {
    currentBuffer_->finishRenderPass();
    queue_->submitCommandBuffer(currentBuffer_);
    queue_->commitToGPU();
    renderTarget_->present();
    currentBuffer_.reset();
}

} // namespace Aqua
