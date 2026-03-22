#include "OmegaGTE.h"
#include <iostream>
#include <cstdlib>

_NAMESPACE_BEGIN_

GTE Init(SharedHandle<GTEDevice> & device){
    auto ge = OmegaGraphicsEngine::Create(device);
#ifdef RUNTIME_SHADER_COMP_SUPPORT
    return {ge, OmegaTriangulationEngine::Create(),OmegaSLCompiler::Create(device)};
#else
    return {ge, OmegaTriangulationEngine::Create()};
#endif


};

GTE InitWithDefaultDevice(){
    auto devices = enumerateDevices();
    if(devices.empty()){
        std::cerr << "OmegaGTE InitWithDefaultDevice failed: no graphics devices were discovered." << std::endl;
        std::abort();
    }
    return Init(devices.front());
}


void Close(GTE &gte){
    gte.graphicsEngine.reset();
    gte.triangulationEngine.reset();
};

_NAMESPACE_END_
