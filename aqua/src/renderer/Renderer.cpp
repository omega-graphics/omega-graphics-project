#include "Renderer.h"

namespace Aqua {

std::unique_ptr<Renderer> Renderer::create(Window &window) {
    OmegaGTE::GTE gte = OmegaGTE::InitWithDefaultDevice();

    OmegaGTE::NativeRenderTargetDescriptor rtDesc{};
    window.fillNativeRenderTargetDesc(rtDesc);

    auto queue = gte.graphicsEngine->makeCommandQueue(64);
    auto renderTarget = gte.graphicsEngine->makeNativeRenderTarget(rtDesc, queue);

    return std::unique_ptr<Renderer>(
        new Renderer(std::move(gte), std::move(queue), std::move(renderTarget)));
}

Renderer::Renderer(OmegaGTE::GTE gte,
                   SharedHandle<OmegaGTE::GECommandQueue> queue,
                   SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget)
    : gte_(std::move(gte)),
      renderTarget_(std::move(renderTarget)),
      queue_(std::move(queue)) {}

Renderer::~Renderer() {
    // Release the GTE-owned handles before tearing down the engine, mirroring
    // the original App::Impl teardown order.
    currentBuffer_.reset();
    renderTarget_.reset();
    queue_.reset();
    OmegaGTE::Close(gte_);
}

OmegaGTE::GTE &Renderer::gte() { return gte_; }

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
