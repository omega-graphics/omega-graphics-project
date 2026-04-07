#ifndef AQUA_AQUAFRONTEND_H
#define AQUA_AQUAFRONTEND_H

#include "aqua/Core/AQUAApplication.h"

AQUA_NAMESPACE_BEGIN

class AQUA_PUBLIC AQUAFrontend {
public:
    bool Launch();
    void Shutdown();
    void LoadScene(const SharedHandle<Scene> & scene);
    AQUAApplication & Application();
    const AQUAApplication & Application() const;

private:
    AQUAApplication application_;
};

AQUA_NAMESPACE_END

#endif
