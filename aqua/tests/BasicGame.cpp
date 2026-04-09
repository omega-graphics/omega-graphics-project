#include <aqua/App.h>

class BasicGame : public Aqua::App {
public:
    BasicGame() : App({{.title = "AQUA - BasicGame", .width = 1280, .height = 720}}) {}

    void onFrame() override {
        auto &rt = renderTarget();
        auto cmdBuf = rt->commandBuffer();

        OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment color(
            {0.1f, 0.1f, 0.1f, 1.0f},
            OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::Clear);

        OmegaGTE::GERenderTarget::RenderPassDesc passDesc {};
        passDesc.colorAttachment = &color;

        cmdBuf->startRenderPass(passDesc);
        cmdBuf->endRenderPass();

        rt->submitCommandBuffer(cmdBuf);
        rt->commitAndPresent();
    }
};

std::unique_ptr<Aqua::App> Aqua::CreateApp() {
    return std::make_unique<BasicGame>();
}
