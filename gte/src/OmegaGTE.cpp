#include "OmegaGTE.h"

_NAMESPACE_BEGIN_

GTE Init(SharedHandle<GTEDevice> & device){
    auto ge = OmegaGraphicsEngine::Create(device);
#ifdef RUNTIME_SHADER_COMP_SUPPORT
    return {ge, OmegaTessellationEngine::Create(),OmegaSLCompiler::Create(device)};
#else
    return {ge, OmegaTessellationEngine::Create()};
#endif


};

GTE InitWithDefaultDevice(){
    auto devices = enumerateDevices();
    return Init(devices.front());
}


void Close(GTE &gte){
    gte.graphicsEngine.reset();
    gte.tessalationEngine.reset();
};

_NAMESPACE_END_