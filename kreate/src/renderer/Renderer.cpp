#include "Renderer.h"
#include <kreate/Pipeline.h>
#include <kreate/Mesh.h>
#include "../pipeline/PipelineFactory.h"
#include "../mesh/MeshFactory.h"

namespace Kreate {

std::unique_ptr<Renderer> Renderer::create(Window &window) {
    OmegaGTE::GTE gte = OmegaGTE::InitWithDefaultDevice();

    OmegaGTE::NativeRenderTargetDescriptor rtDesc{};
    window.fillNativeRenderTargetDesc(rtDesc);

    OmegaGTE::GECommandQueueDesc queueDesc{};
    queueDesc.maxBufferCount = 64;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);
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

void Renderer::draw(Pipeline &pipeline, Mesh &mesh, const OmegaGTE::FMatrix<4,4> &mvp) {
    if (!currentBuffer_) return;
    auto &state = PipelineFactory::state(pipeline);
    auto &geMesh = MeshFactory::geMesh(mesh);
    if (!state || !geMesh) return;

    currentBuffer_->setRenderPipelineState(state);
    // 64-byte push constant — single `float4x4` matches the Phase 1
    // shader's `constant<PushData> { float4x4 mvp; }` declaration. FMatrix is
    // column-major (`mvp[c][r]`), so column c goes to floats [4c..4c+3] — the
    // exact order the shader's column-major `float4x4` reads, no transpose.
    float flat[16];
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned r = 0; r < 4; ++r) {
            flat[c * 4 + r] = mvp[c][r];
        }
    }
    currentBuffer_->setRenderConstants(&flat[0],
                                       static_cast<unsigned>(sizeof(float) * 16),
                                       0);
    currentBuffer_->drawMesh(geMesh, /*vertexSlot=*/0);
}

void Renderer::endFrameAndPresent() {
    currentBuffer_->finishRenderPass();
    queue_->submitCommandBuffer(currentBuffer_);
    queue_->commitToGPU();
    renderTarget_->present();
    currentBuffer_.reset();
}

} // namespace Kreate
