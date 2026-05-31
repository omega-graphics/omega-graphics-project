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
    /// Mesh-Shader-Plan Phase 4c.1 — variant flag. When true, the
    /// `vertexShader` slot inherited from `__GERenderPipelineState`
    /// holds the mesh shader (mesh replaces the vertex stage in this
    /// variant; both stage types are `SharedHandle<GTEShader>` and the
    /// per-shader-info reads go through `shader->internal` uniformly,
    /// so the slot doubles cleanly). `drawMeshTasks` asserts on this
    /// flag before issuing `drawMeshThreadgroups:`. Matches the
    /// `isMesh` flag on the D3D12 sibling at GED3D12Pipeline.h:27.
    bool isMesh = false;
    GEMetalRenderPipelineState(SharedHandle<GTEShader> & _vertexShader,
                               SharedHandle<GTEShader> & _fragmentShader,
                               NSSmartPtr & renderPipelineState,bool hasDepthStencilState,
                                NSSmartPtr & depthStencilState,GEMetalRasterizerState & rasterizerState);
    /// Mesh-Shader-Plan Phase 4c.1 — mesh-variant constructor. Same
    /// shape as the graphics constructor; `_meshShader` lands in the
    /// `vertexShader` base slot and `isMesh` is stamped true.
    GEMetalRenderPipelineState(SharedHandle<GTEShader> & _meshShader,
                               SharedHandle<GTEShader> & _fragmentShader,
                               NSSmartPtr & renderPipelineState,
                               bool hasDepthStencilState,
                               NSSmartPtr & depthStencilState,
                               GEMetalRasterizerState & rasterizerState,
                               bool meshVariant);
};

class GEMetalComputePipelineState : public __GEComputePipelineState {
public:
    NSSmartPtr computePipelineState;
    GEMetalComputePipelineState(SharedHandle<GTEShader> & _computeShader,
                                NSSmartPtr & computePipelineState);
};

// Extension 3: wraps a regular render pipeline whose vertex stage is the
// engine-supplied full-screen-triangle shader.
class GEMetalBlitPipelineState : public __GEBlitPipelineState {
public:
    SharedHandle<GERenderPipelineState> renderPipeline;
    explicit GEMetalBlitPipelineState(SharedHandle<GERenderPipelineState> & rp)
        : renderPipeline(rp) {}
};

_NAMESPACE_END_

#endif
