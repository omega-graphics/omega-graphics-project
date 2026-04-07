#ifndef AQUA_CORE_AQUAAPPLICATION_H
#define AQUA_CORE_AQUAAPPLICATION_H

#include "aqua/Scene/AQUAScene.h"

AQUA_NAMESPACE_BEGIN

class AQUA_PUBLIC AQUAApplication {
public:
    bool Init();
    void Tick();
    void Shutdown();

    bool IsInitialized() const;
    std::uint64_t TickCount() const;

    void LoadScene(const SharedHandle<Scene> & scene);
    SharedHandle<Scene> CurrentScene() const;

private:
    bool initialized_ = false;
    std::uint64_t tickCount_ = 0;
    SharedHandle<Scene> currentScene_;
};

AQUA_NAMESPACE_END

#endif
