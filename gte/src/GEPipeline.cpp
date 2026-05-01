#include "omegaGTE/GEPipeline.h"
#include "omegaGTE/GTEShader.h"
#include "omegasl.h"

_NAMESPACE_BEGIN_

struct __GERenderPipelineState {
    SharedHandle<GTEShader> vertexShader;
    SharedHandle<GTEShader> fragmentShader;
    __GERenderPipelineState(SharedHandle<GTEShader> & _vertexShader,SharedHandle<GTEShader> & _fragmentShader):
    vertexShader(_vertexShader),
    fragmentShader(_fragmentShader){

    };
};

struct __GEComputePipelineState {
    SharedHandle<GTEShader> computeShader;
    explicit __GEComputePipelineState(SharedHandle<GTEShader> & _computeShader):computeShader(_computeShader){

    }
};

_NAMESPACE_END_
