#include <aqua/App.h>
#include <aqua/Scene.h>
#include <cmath>

class BasicGame : public Aqua::App {
    std::shared_ptr<Aqua::Scene> scene;
public:
    BasicGame() : App({{.title = "AQUA - BasicGame", .width = 1280, .height = 720}}) {}

    void onInit() override {
        scene = Aqua::Scene::create();
        scene->setClearColor({0.1f, 0.1f, 0.1f, 1.0f});
        scene->setProjectionMatrix(
            Aqua::Mat4::perspective(60.f * 3.14159f / 180.f, 1280.f / 720.f, 0.1f, 100.f));
        scene->setViewMatrix(
            Aqua::Mat4::lookAt({0, 2, 5}, {0, 0, 0}, {0, 1, 0}));
    }

    void onFrame() override {
        scene->render(*this);
    }
};

std::unique_ptr<Aqua::App> Aqua::CreateApp() {
    return std::make_unique<BasicGame>();
}
