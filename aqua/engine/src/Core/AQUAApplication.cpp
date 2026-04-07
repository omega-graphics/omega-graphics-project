#include "aqua/Core/AQUAApplication.h"

AQUA_NAMESPACE_BEGIN

bool AQUAApplication::Init() {
    if (initialized_) {
        return true;
    }

    initialized_ = true;
    tickCount_ = 0;
    return true;
}

void AQUAApplication::Tick() {
    if (!initialized_) {
        return;
    }

    ++tickCount_;
}

void AQUAApplication::Shutdown() {
    currentScene_.reset();
    tickCount_ = 0;
    initialized_ = false;
}

bool AQUAApplication::IsInitialized() const {
    return initialized_;
}

std::uint64_t AQUAApplication::TickCount() const {
    return tickCount_;
}

void AQUAApplication::LoadScene(const SharedHandle<Scene> & scene) {
    currentScene_ = scene;
}

SharedHandle<Scene> AQUAApplication::CurrentScene() const {
    return currentScene_;
}

AQUA_NAMESPACE_END
