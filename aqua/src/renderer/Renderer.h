#ifndef AQUA_INTERNAL_RENDERER_H
#define AQUA_INTERNAL_RENDERER_H

#include <aqua/Math.h>
#include <OmegaGTE.h>

namespace Aqua {

/// Internal renderer. Owns command-buffer encoding for one window's native
/// render target. Created by `App::Impl` during init; not exposed in
/// public AQUA headers. Scene reaches it through friendship on App::Impl.
class Renderer {
public:
    Renderer(SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget,
             SharedHandle<OmegaGTE::GECommandQueue> queue);
    ~Renderer();

    /// Begins a render pass against the native target. Subsequent draws go
    /// into the open command buffer until `endFrameAndPresent` is called.
    void beginFrame(Color clearColor);

    /// Closes the open pass, submits the command buffer, and presents the
    /// swap-chain backing the native target.
    void endFrameAndPresent();

    // Future: void draw(Pipeline &pipeline, Mesh &mesh, const Mat4 &mvp);
    // Activates once GEMesh lands (see gte/docs/GEMesh-TextureAssets-Implementation-Plan.md).

private:
    SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget_;
    SharedHandle<OmegaGTE::GECommandQueue> queue_;
    SharedHandle<OmegaGTE::GECommandBuffer> currentBuffer_;
};

} // namespace Aqua

#endif // AQUA_INTERNAL_RENDERER_H
