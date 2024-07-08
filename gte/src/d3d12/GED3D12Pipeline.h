#include "GED3D12.h"
#include "../GEPipeline.cpp"

#ifndef OMEGAGTE_D3D12_GED3D12PIPELINE_H
#define OMEGAGTE_D3D12_GED3D12PIPELINE_H

_NAMESPACE_BEGIN_

struct GED3D12Shader : public GTEShader {
    D3D12_SHADER_BYTECODE shaderBytecode;
};

class GED3D12RenderPipelineState : public __GERenderPipelineState {
public:
     ComPtr<ID3D12PipelineState> pipelineState;
     ComPtr<ID3D12RootSignature> rootSignature;
     D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc;
     ~GED3D12RenderPipelineState();
    GED3D12RenderPipelineState(SharedHandle<GTEShader> & _vertShader,SharedHandle<GTEShader> & _fragShader,ID3D12PipelineState *state,ID3D12RootSignature *signature,D3D12_ROOT_SIGNATURE_DESC1 & rootSignatureDesc);
};

class GED3D12ComputePipelineState : public __GEComputePipelineState {
public: 
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12RootSignature> rootSignature;
    D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc;
    ~GED3D12ComputePipelineState();
    GED3D12ComputePipelineState(SharedHandle<GTEShader> & _shader,ID3D12PipelineState *state,ID3D12RootSignature *signature,D3D12_ROOT_SIGNATURE_DESC1 & rootSignatureDesc);
};

_NAMESPACE_END_

#endif