#include <AQUA.h>
#include <omegaWTK/Main.h>

int omegaWTKMain(OmegaWTK::AppInst *app){
    (void)app;

    AQUA::AQUAFrontend frontend;
    if (!frontend.Launch()) {
        return 1;
    }

    auto scene = std::make_shared<AQUA::Scene>();
    scene->dimensions = {32u, 32u, 32u};
    auto entity = scene->createEntity("BootstrapEntity");
    entity->transform().Translate(0.0f, 0.0f, 0.0f);

    frontend.LoadScene(scene);
    frontend.Application().Tick();
    frontend.Shutdown();

    return 0;
}
