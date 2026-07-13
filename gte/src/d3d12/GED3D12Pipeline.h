#include "GED3D12.h"
#include "../GEPipeline.cpp"

#ifndef OMEGAGTE_D3D12_GED3D12PIPELINE_H
#define OMEGAGTE_D3D12_GED3D12PIPELINE_H

_NAMESPACE_BEGIN_

struct GED3D12Shader : public GTEShader {
    D3D12_SHADER_BYTECODE shaderBytecode {};
};

class GED3D12RenderPipelineState : public __GERenderPipelineState {
public:
     ComPtr<ID3D12PipelineState> pipelineState;
     ComPtr<ID3D12RootSignature> rootSignature;
     D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc;
     /// Mesh-Shader-Plan Phase 4b.1 — variant flag. When true, the
     /// `vertexShader` slot inherited from `__GERenderPipelineState`
     /// holds the mesh shader (the geometry stage's role is taken by
     /// mesh in this variant; both stage types are
     /// `SharedHandle<GTEShader>` and the resource-binding paths read
     /// `shader->internal` uniformly, so the slot doubles cleanly).
     /// `GED3D12CommandBuffer::drawMeshTasks` asserts on this flag
     /// before issuing `DispatchMesh`; `drawPolygons` family is a
     /// logic error against a mesh PSO and assertion-bound the same way.
     bool isMesh = false;
     /// §5 — the optional amplification (AS) stage of a mesh pipeline. Null on
     /// every other pipeline kind, and on a mesh pipeline built without one.
     ///
     /// Its own slot rather than a doubling onto an existing one (the way the
     /// mesh shader doubles onto `vertexShader`) because amplification does not
     /// REPLACE any stage — it runs upstream of the mesh stage and coexists with
     /// it, with its own resource table and its own register space (space2).
     /// There is nothing for it to double onto.
     /// `bindResourceAtAmplificationShader` and `setRenderConstants` read
     /// `internal` off this handle to resolve the amp's root parameters.
     SharedHandle<GTEShader> amplificationShader;
     /// §16 Phase H — tessellation-pipeline flag + per-patch control-point
     /// count. Set by `makeRenderPipelineState` after construction when the
     /// descriptor carried `hullFunc`/`domainFunc` (the PSO was built with
     /// `.HS`/`.DS` and a `_PATCH` topology type). `GED3D12CommandBuffer::
     /// drawPatches` reads `patchControlPoints` to select the N-control-point
     /// patch-list IA topology and size the draw (`patchCount *
     /// patchControlPoints` vertices); `setRenderPipelineState` rejects a
     /// pipeline whose `isTess` is true unless a `startTessRenderPass` scope is
     /// active (a tessellated draw must go through that entry point).
     bool isTess = false;
     std::uint32_t patchControlPoints = 0;
     ~GED3D12RenderPipelineState();
    GED3D12RenderPipelineState(SharedHandle<GTEShader> & _vertShader,SharedHandle<GTEShader> & _fragShader,ID3D12PipelineState *state,ID3D12RootSignature *signature,D3D12_ROOT_SIGNATURE_DESC1 & rootSignatureDesc);
    /// Mesh-Shader-Plan Phase 4b.1 — mesh-variant constructor. Same
    /// shape as the graphics constructor; `_meshShader` lands in the
    /// `vertexShader` base slot and `isMesh` is stamped true.
    GED3D12RenderPipelineState(SharedHandle<GTEShader> & _meshShader,
                               SharedHandle<GTEShader> & _fragShader,
                               ID3D12PipelineState *state,
                               ID3D12RootSignature *signature,
                               D3D12_ROOT_SIGNATURE_DESC1 & rootSignatureDesc,
                               bool meshVariant);
};

class GED3D12ComputePipelineState : public __GEComputePipelineState {
public:
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12RootSignature> rootSignature;
    D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc;
    ~GED3D12ComputePipelineState();
    GED3D12ComputePipelineState(SharedHandle<GTEShader> & _shader,ID3D12PipelineState *state,ID3D12RootSignature *signature,D3D12_ROOT_SIGNATURE_DESC1 & rootSignatureDesc);
};

// Blit pipeline (Extension 3): a wrapper over a regular render pipeline whose
// vertex shader is the engine-supplied full-screen triangle.
class GED3D12BlitPipelineState : public __GEBlitPipelineState {
public:
    SharedHandle<GERenderPipelineState> renderPipeline;
    explicit GED3D12BlitPipelineState(SharedHandle<GERenderPipelineState> & rp)
        : renderPipeline(rp) {}
};

_NAMESPACE_END_

#endif