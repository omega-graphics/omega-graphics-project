#include <aqua/App.h>

class BasicGame : public Aqua::App {
public:
    BasicGame() : App({{.title = "AQUA - BasicGame", .width = 1280, .height = 720}}) {}

    void onFrame() override {
        auto &rt = renderTarget();
        auto &queue = commandQueue();
        auto cmdBuf = queue->getAvailableBuffer();

        OmegaGTE::GERenderPassDescriptor passDesc {};
        passDesc.nRenderTarget = rt.get();
        passDesc.colorAttachments.push_back(OmegaGTE::GERenderPassDescriptor::ColorAttachment(
            {0.1f, 0.1f, 0.1f, 1.0f},
            OmegaGTE::GERenderPassDescriptor::ColorAttachment::Clear));

        cmdBuf->startRenderPass(passDesc);
        cmdBuf->finishRenderPass();

        queue->submitCommandBuffer(cmdBuf);
        queue->commitToGPU();
        rt->present();
    }
};

std::unique_ptr<Aqua::App> Aqua::CreateApp() {
    return std::make_unique<BasicGame>();
}
