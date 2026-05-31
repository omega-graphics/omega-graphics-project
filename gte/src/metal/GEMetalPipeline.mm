#import "GEMetalPipeline.h"

_NAMESPACE_BEGIN_

GEMetalShader::GEMetalShader(NSSmartPtr & lib,NSSmartPtr & func):library(lib), function(func){

};

GEMetalRenderPipelineState::GEMetalRenderPipelineState(SharedHandle<GTEShader> & _vertexShader,
                                                       SharedHandle<GTEShader> & _fragmentShader,
                                                       NSSmartPtr & renderPipelineState,
                                                       bool hasDepthStencilState,
                                                       NSSmartPtr & depthStencilState,
                                                       GEMetalRasterizerState & rasterizerState):
        __GERenderPipelineState(_vertexShader,_fragmentShader),
        renderPipelineState(renderPipelineState),
        hasDepthStencilState(hasDepthStencilState),
        depthStencilState(depthStencilState),
        rasterizerState(rasterizerState){

};

GEMetalRenderPipelineState::GEMetalRenderPipelineState(SharedHandle<GTEShader> & _meshShader,
                                                       SharedHandle<GTEShader> & _fragmentShader,
                                                       NSSmartPtr & renderPipelineState,
                                                       bool hasDepthStencilState,
                                                       NSSmartPtr & depthStencilState,
                                                       GEMetalRasterizerState & rasterizerState,
                                                       bool /*meshVariant*/):
        __GERenderPipelineState(_meshShader,_fragmentShader),
        renderPipelineState(renderPipelineState),
        hasDepthStencilState(hasDepthStencilState),
        depthStencilState(depthStencilState),
        rasterizerState(rasterizerState) {
    isMesh = true;
}

GEMetalComputePipelineState::GEMetalComputePipelineState(SharedHandle<GTEShader> & _computeShader,
                                                         NSSmartPtr & computePipelineState):
        __GEComputePipelineState(_computeShader),
        computePipelineState(computePipelineState){
    
};

_NAMESPACE_END_
