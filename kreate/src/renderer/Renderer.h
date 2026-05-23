#ifndef KREATE_INTERNAL_RENDERER_H
#define KREATE_INTERNAL_RENDERER_H

#include <kreate/Math.h>
#include <kreate/Window.h>
#include <OmegaGTE.h>
#include <memory>

namespace Kreate {

/// Internal renderer. Owns the OmegaGTE device stack for one window: the GTE
/// instance, its command queue, and the native render target backing the
/// window. Created by `App::Impl` during init via `Renderer::create`; not
/// exposed in public KREATE headers. Scene reaches it through friendship on
/// App::Impl, and the pipeline subsystem borrows its GTE via `gte()`.
class Renderer {
public:
    /// Initializes OmegaGTE with the default device, builds the command queue,
    /// and creates the native render target backing `window`. This is the sole
    /// owner of the GTE lifecycle — `App` no longer touches OmegaGTE directly.
    static std::unique_ptr<Renderer> create(Window &window);

    ~Renderer();

    /// The GTE instance owned by this renderer. The pipeline subsystem uses it
    /// to compile shaders and build pipeline state. Borrowed, not owned, by
    /// the caller — the renderer outlives every pipeline built from it.
    OmegaGTE::GTE &gte();

    /// Begins a render pass against the native target. Subsequent draws go
    /// into the open command buffer until `endFrameAndPresent` is called.
    void beginFrame(Color clearColor);

    /// Closes the open pass, submits the command buffer, and presents the
    /// swap-chain backing the native target.
    void endFrameAndPresent();

    // Future: void draw(Pipeline &pipeline, Mesh &mesh, const Mat4 &mvp);
    // Activates once GEMesh lands (see gte/docs/GEMesh-TextureAssets-Implementation-Plan.md).

private:
    Renderer(OmegaGTE::GTE gte,
             SharedHandle<OmegaGTE::GECommandQueue> queue,
             SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget);

    OmegaGTE::GTE gte_;
    SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget_;
    SharedHandle<OmegaGTE::GECommandQueue> queue_;
    SharedHandle<OmegaGTE::GECommandBuffer> currentBuffer_;
};

} // namespace Kreate

#endif // KREATE_INTERNAL_RENDERER_H
