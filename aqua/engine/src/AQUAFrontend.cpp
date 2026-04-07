#include "AQUAFrontend.h"

AQUA_NAMESPACE_BEGIN

bool AQUAFrontend::Launch() {
    return application_.Init();
}

void AQUAFrontend::Shutdown() {
    application_.Shutdown();
}

void AQUAFrontend::LoadScene(const SharedHandle<Scene> & scene) {
    application_.LoadScene(scene);
}

AQUAApplication & AQUAFrontend::Application() {
    return application_;
}

const AQUAApplication & AQUAFrontend::Application() const {
    return application_;
}

AQUA_NAMESPACE_END
