#include "GEMetal.h"
#include "../GEPipeline.cpp"

#import <Metal/Metal.h>

#ifndef OMEGAGTE_METAL_GEMETALPIPELINE_H
#define OMEGAGTE_METAL_GEMETALPIPELINE_H

_NAMESPACE_BEGIN_

struct GEMetalShader : public GTEShader {
    NSSmartPtr library;
    NSSmartPtr function;
    GEMetalShader(NSSmartPtr & lib,NSSmartPtr & func);
};

struct GEMetalRasterizerState {
    MTLWinding winding;
    MTLCullMode cullMode;
    MTLTriangleFillMode fillMode;
    float depthBias,slopeScale,depthClamp;
};

class GEMetalRenderPipelineState : public __GERenderPipelineState {
public:
    NSSmartPtr renderPipelineState;
    bool hasDepthStencilState;
    NSSmartPtr depthStencilState;
    GEMetalRasterizerState rasterizerState;
    GEMetalRenderPipelineState(SharedHandle<GTEShader> & _vertexShader,
                               SharedHandle<GTEShader> & _fragmentShader,
                               NSSmartPtr & renderPipelineState,bool hasDepthStencilState,
                                NSSmartPtr & depthStencilState,GEMetalRasterizerState & rasterizerState);
};

class GEMetalComputePipelineState : public __GEComputePipelineState {
public:
    NSSmartPtr computePipelineState;
    GEMetalComputePipelineState(SharedHandle<GTEShader> & _computeShader,
                                NSSmartPtr & computePipelineState);
};

_NAMESPACE_END_

#endif
