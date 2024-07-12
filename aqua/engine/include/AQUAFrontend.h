#include "aqua/Core/AQUACore.h"

AQUA_NAMESPACE_BEGIN

class Frontend {
public:

    /**
     * @brief Load inital scene
     * 
     */
    void launch();

    void loadScene(SharedHandle<Scene> & scene);


};

AQUA_NAMESPACE_END