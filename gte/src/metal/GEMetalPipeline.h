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
    /// §5 — the optional amplification stage of a mesh pipeline (Metal calls it
    /// the OBJECT stage). Null on every other pipeline kind, and on a mesh
    /// pipeline built without one.
    ///
    /// Its own slot rather than a doubling onto an existing one (the way the
    /// mesh shader doubles onto `vertexShader`) because amplification does not
    /// REPLACE any stage — it runs upstream of the mesh stage and coexists with
    /// it, with its own per-stage binding table (`setObjectBuffer:` &c.). There
    /// is nothing for it to double onto. `drawMeshTasks` also reads its
    /// `threadgroupDesc` for `threadsPerObjectThreadgroup:`, which Phase 4c had
    /// to pin at (1,1,1) precisely because no object stage could be bound.
    SharedHandle<GTEShader> amplificationShader;
    /// §16 Phase E — tessellation-pipeline variant. When true, `vertexShader`
    /// holds the DOMAIN (post-tessellation vertex) shader and the fields below
    /// carry the hull compute stage + tessellation config the deferred
    /// `startTessRenderPass` / `drawPatches` path needs. Set by
    /// `makeRenderPipelineState` when `hullFunc` + `domainFunc` are supplied.
    bool isTessellation = false;
    /// Compute pipeline built from the hull kernel; dispatched one thread per
    /// patch to produce the post-hull control points + tessellation factors.
    NSSmartPtr hullComputePipelineState = NSObjectHandle{nullptr};
    /// Control points per patch (== hull `outputcontrolpoints`).
    unsigned tessControlPointCount = 0;
    /// Byte size of one `MTL{Triangle,Quad}TessellationFactorsHalf` (8 / 12).
    unsigned tessFactorStructSize = 0;
    /// Byte stride of one control point (drives the engine-owned post-hull
    /// control-point buffer size), from the pipeline's vertex input layout.
    unsigned controlPointStride = 0;
    /// Vertex-descriptor buffer index the domain's `patch_control_point`
    /// stage-in is bound to (the post-hull control-point buffer slot).
    unsigned cpStageInBufferIndex = 0;
    /// Threads-per-threadgroup for the hull dispatch (from the hull's
    /// serialized `threadgroupDesc`).
    unsigned hullThreadgroupSize = 32;
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
